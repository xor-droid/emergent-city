/* record.c — session recording + deterministic replay (see record.h). */
#define _POSIX_C_SOURCE 200809L   /* open_memstream */
#include "record.h"
#include "viz.h"        /* god_apply */
#include "llm.h"        /* llm_enabled */
#include "os_client.h" /* os_ingest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── byte-identical fingerprint ──────────────────────────────────────────── */
uint64_t world_checksum(const World *w) {
    const unsigned char *p = (const unsigned char *)w;
    uint64_t h = 1469598103934665603ULL;             /* FNV-1a 64 */
    for (size_t i = 0; i < sizeof(World); i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* ── the live-config knob table: one row per replayable value, as doubles.
 * Both the startup-config snapshot and `tune` events go through this table, so a
 * knob name means the same thing when recorded and when replayed. ──────────── */
typedef struct { const char *name; double (*get)(void); void (*set)(double); } Knob;

static double gf_ypd(void){return get_years_per_day();}           static void sf_ypd(double v){set_years_per_day(v);}
static double gf_fshare(void){return get_family_share();}         static void sf_fshare(double v){set_family_share(v);}
static double gf_kmin(void){return get_family_kids_min();}        static void sf_kmin(double v){set_family_kids((int)v,get_family_kids_max());}
static double gf_kmax(void){return get_family_kids_max();}        static void sf_kmax(double v){set_family_kids(get_family_kids_min(),(int)v);}
static double gf_pop(void){return get_pop_target();}              static void sf_pop(double v){set_pop_target((int)v);}
static double gf_rrate(void){return get_research_rate();}         static void sf_rrate(double v){set_research_rate(v);}
static double gf_prod(void){return get_craft_bonus();}            static void sf_prod(double v){set_craft_bonus(v);}
static double gf_child(void){return get_child_cost();}            static void sf_child(double v){set_child_cost(v);}
static double gf_vis(void){return get_vision();}                  static void sf_vis(double v){set_vision((int)v);}
static double gf_visr(void){return get_vision_radius();}          static void sf_visr(double v){set_vision_radius((int)v);}
static double gf_hear(void){return get_hearing();}                static void sf_hear(double v){set_hearing((int)v);}
static double gf_hearr(void){return get_hearing_radius();}        static void sf_hearr(double v){set_hearing_radius((int)v);}
static double gf_noise(void){return get_noise_worldgen();}        static void sf_noise(double v){set_noise_worldgen((int)v);}
static double gf_mevery(void){return get_metrics_every();}        static void sf_mevery(double v){set_metrics_every(v);}
static double gf_occ(void){return get_occ_pay_spread();}          static void sf_occ(double v){set_occ_pay_spread(v);}
static double gf_nbhd(void){return get_neighborhoods();}          static void sf_nbhd(double v){set_neighborhoods((int)v);}
static double gf_cw(void){return get_crime_wealth();}             static void sf_cw(double v){set_crime_wealth((int)v);}
static double gf_pbias(void){return get_police_bias();}           static void sf_pbias(double v){set_police_bias((int)v);}
static double gf_fstep(void){return get_fixed_step();}            static void sf_fstep(double v){set_fixed_step((int)v);}
static double gf_fdt(void){return get_fixed_dt();}                static void sf_fdt(double v){set_fixed_dt(v);}
static double gf_weather(void){return get_weather();}             static void sf_weather(double v){set_weather((int)v);}
static double gf_wperiod(void){return get_weather_period();}      static void sf_wperiod(double v){set_weather_period(v);}
static double gf_heatc(void){return get_heat_cost();}             static void sf_heatc(double v){set_heat_cost(v);}
static double gf_biomes(void){return get_biomes();}               static void sf_biomes(double v){set_biomes((int)v);}
static double gf_bweight(void){return get_biome_value_weight();}  static void sf_bweight(double v){set_biome_value_weight(v);}
static double gf_tax(void){return get_taxation();}                static void sf_tax(double v){set_taxation((int)v);}
static double gf_taxr(void){return get_tax_rate();}               static void sf_taxr(double v){set_tax_rate(v);}
static double gf_welf(void){return get_welfare();}                static void sf_welf(double v){set_welfare(v);}
static double gf_wtaxr(void){return get_wealth_tax_rate();}       static void sf_wtaxr(double v){set_wealth_tax_rate(v);}
static double gf_wtaxt(void){return get_wealth_tax_threshold();}  static void sf_wtaxt(double v){set_wealth_tax_threshold(v);}
static double gf_taxbr(void){return get_tax_brackets();}          static void sf_taxbr(double v){set_tax_brackets((int)v);}
static double gf_srccap(void){return get_source_caps();}          static void sf_srccap(double v){set_source_caps((int)v);}
static double gf_rentcap(void){return get_rent_cap();}            static void sf_rentcap(double v){set_rent_cap(v);}
static double gf_stab(void){return get_stabilizers();}            static void sf_stab(double v){set_stabilizers((int)v);}
static double gf_benefit(void){return get_benefit();}             static void sf_benefit(double v){set_benefit(v);}
static double gf_just(void){return get_justice();}                static void sf_just(double v){set_justice((int)v);}
static double gf_corr(void){return get_corruption();}             static void sf_corr(double v){set_corruption(v);}
static double gf_overs(void){return get_oversight();}             static void sf_overs(double v){set_oversight(v);}

static const Knob KNOBS[] = {
    {"years_per_day",gf_ypd,sf_ypd}, {"family_share",gf_fshare,sf_fshare},
    {"kids_min",gf_kmin,sf_kmin}, {"kids_max",gf_kmax,sf_kmax}, {"pop_target",gf_pop,sf_pop},
    {"research_rate",gf_rrate,sf_rrate}, {"production",gf_prod,sf_prod}, {"child_cost",gf_child,sf_child},
    {"vision",gf_vis,sf_vis}, {"vision_radius",gf_visr,sf_visr},
    {"hearing",gf_hear,sf_hear}, {"hearing_radius",gf_hearr,sf_hearr},
    {"noise_worldgen",gf_noise,sf_noise}, {"metrics_every",gf_mevery,sf_mevery},
    {"occ_pay_spread",gf_occ,sf_occ}, {"neighborhoods",gf_nbhd,sf_nbhd},
    {"crime_wealth",gf_cw,sf_cw}, {"police_bias",gf_pbias,sf_pbias},
    {"fixed_step",gf_fstep,sf_fstep}, {"fixed_dt",gf_fdt,sf_fdt},
    {"weather",gf_weather,sf_weather}, {"weather_period",gf_wperiod,sf_wperiod}, {"heat_cost",gf_heatc,sf_heatc},
    {"biomes",gf_biomes,sf_biomes}, {"biome_value_weight",gf_bweight,sf_bweight},
    {"taxation",gf_tax,sf_tax}, {"tax_rate",gf_taxr,sf_taxr}, {"welfare",gf_welf,sf_welf},
    {"wealth_tax_rate",gf_wtaxr,sf_wtaxr}, {"wealth_tax_threshold",gf_wtaxt,sf_wtaxt},
    {"tax_brackets",gf_taxbr,sf_taxbr},
    {"source_caps",gf_srccap,sf_srccap}, {"rent_cap",gf_rentcap,sf_rentcap},
    {"stabilizers",gf_stab,sf_stab}, {"benefit",gf_benefit,sf_benefit},
    {"justice",gf_just,sf_just}, {"corruption",gf_corr,sf_corr}, {"oversight",gf_overs,sf_overs},
};
static const int N_KNOBS = (int)(sizeof(KNOBS)/sizeof(KNOBS[0]));
static int knob_index(const char *name) {
    for (int i = 0; i < N_KNOBS; i++) if (!strcmp(KNOBS[i].name, name)) return i;
    return -1;
}

/* ── session state (writer) ──────────────────────────────────────────────── */
typedef struct { uint64_t tick; double gtime; char kind[8]; char knob[24]; double value; int tool, tx, ty; } RecEvent;

static struct {
    int active;
    char session_id[128];       /* room for "<parent>-fork-<tick>" */
    char reason[96];
    char parent_id[64];         /* set for a fork; "" otherwise */
    uint64_t fork_tick;         /* tick the fork diverged from the parent */
    uint64_t seed; int pop;
    double cfg[48];              /* startup snapshot, parallel to KNOBS */
    RecEvent *ev; int n, cap;
    uint64_t end_tick; uint64_t checksum;
} S;

int rec_active(void) { return S.active; }

void rec_begin(uint64_t seed, int pop, const char *reason) {
    memset(&S, 0, sizeof(S));
    S.active = 1; S.seed = seed; S.pop = pop;
    snprintf(S.reason, sizeof S.reason, "%s", reason ? reason : "");
    snprintf(S.session_id, sizeof S.session_id, "csim-%llu-%lld",
             (unsigned long long)seed, (long long)time(NULL));
    for (int i = 0; i < N_KNOBS; i++) S.cfg[i] = KNOBS[i].get();   /* freeze startup config */
}

static void push_event(RecEvent e) {
    if (!S.active) return;
    if (S.n == S.cap) { S.cap = S.cap ? S.cap*2 : 64; S.ev = realloc(S.ev, (size_t)S.cap*sizeof(RecEvent)); }
    S.ev[S.n++] = e;
}
void rec_tune(const World *w, const char *knob, double value) {
    RecEvent e; memset(&e, 0, sizeof e);
    e.tick = w->tick; e.gtime = w->day + w->hour/24.0;
    snprintf(e.kind, sizeof e.kind, "tune"); snprintf(e.knob, sizeof e.knob, "%s", knob); e.value = value;
    push_event(e);
}
void rec_tune_now(const World *w, const char *knob) {
    int k = knob_index(knob); if (k < 0) return;
    rec_tune(w, knob, KNOBS[k].get());
}
void rec_god(const World *w, int tool, int tx, int ty) {
    RecEvent e; memset(&e, 0, sizeof e);
    e.tick = w->tick; e.gtime = w->day + w->hour/24.0;
    snprintf(e.kind, sizeof e.kind, "god"); e.tool = tool; e.tx = tx; e.ty = ty;
    push_event(e);
}
const char *rec_session_id(void) { return S.active ? S.session_id : ""; }

/* ship the session manifest + all events to OpenSearch (via Data Prepper) as a JSON
 * array of docs. Best-effort; a down sink never affects the local recording/replay. */
static void rec_ship_opensearch(void) {
    if (!S.active || !os_ingest_enabled()) return;
    char *buf = NULL; size_t sz = 0; FILE *m = open_memstream(&buf, &sz);
    if (!m) return;
    fprintf(m, "[{\"doc_type\":\"session\",\"session_id\":\"%s\",\"seed\":%llu,\"pop\":%d,"
               "\"reason\":\"%s\",\"llm\":%d,\"end_tick\":%llu,\"checksum\":\"%llu\"",
            S.session_id, (unsigned long long)S.seed, S.pop, S.reason, llm_enabled(),
            (unsigned long long)S.end_tick, (unsigned long long)S.checksum);
    for (int i = 0; i < N_KNOBS; i++) fprintf(m, ",\"cfg_%s\":%.10g", KNOBS[i].name, S.cfg[i]);
    if (S.parent_id[0]) fprintf(m, ",\"parent_id\":\"%s\",\"fork_tick\":%llu", S.parent_id, (unsigned long long)S.fork_tick);
    fprintf(m, "}");
    for (int i = 0; i < S.n; i++) {
        RecEvent *e = &S.ev[i];
        if (!strcmp(e->kind, "tune"))
            fprintf(m, ",{\"doc_type\":\"event\",\"session_id\":\"%s\",\"tick\":%llu,"
                       "\"kind\":\"tune\",\"knob\":\"%s\",\"value\":%.10g}",
                    S.session_id, (unsigned long long)e->tick, e->knob, e->value);
        else if (!strcmp(e->kind, "god"))
            fprintf(m, ",{\"doc_type\":\"event\",\"session_id\":\"%s\",\"tick\":%llu,"
                       "\"kind\":\"god\",\"tool\":%d,\"tx\":%d,\"ty\":%d}",
                    S.session_id, (unsigned long long)e->tick, e->tool, e->tx, e->ty);
    }
    fprintf(m, "]");
    fclose(m);
    os_ingest(buf);
    free(buf);
}

void rec_end(const World *w) {
    if (!S.active) return;
    S.end_tick = w->tick; S.checksum = world_checksum(w);
    rec_ship_opensearch();
}

int rec_save_file(const char *path) {
    if (!S.active || !path || !*path) return 0;
    FILE *f = fopen(path, "w"); if (!f) return 0;
    fprintf(f, "session %s\n", S.session_id);
    fprintf(f, "reason %s\n", S.reason);
    if (S.parent_id[0]) { fprintf(f, "parent %s\n", S.parent_id); fprintf(f, "fork_tick %llu\n", (unsigned long long)S.fork_tick); }
    fprintf(f, "seed %llu\n", (unsigned long long)S.seed);
    fprintf(f, "pop %d\n", S.pop);
    fprintf(f, "llm %d\n", llm_enabled());
    for (int i = 0; i < N_KNOBS; i++) fprintf(f, "cfg %s %.10g\n", KNOBS[i].name, S.cfg[i]);
    fprintf(f, "end_tick %llu\n", (unsigned long long)S.end_tick);
    fprintf(f, "checksum %llu\n", (unsigned long long)S.checksum);
    for (int i = 0; i < S.n; i++) {
        RecEvent *e = &S.ev[i];
        if (!strcmp(e->kind, "tune")) fprintf(f, "event %llu tune %s %.10g\n", (unsigned long long)e->tick, e->knob, e->value);
        else if (!strcmp(e->kind, "god")) fprintf(f, "event %llu god %d %d %d\n", (unsigned long long)e->tick, e->tool, e->tx, e->ty);
    }
    fclose(f);
    return 1;
}

/* ── replay (reader) ─────────────────────────────────────────────────────── */
typedef struct { uint64_t tick; int is_tune; char knob[24]; double value; int tool, tx, ty; } PlayEvent;
typedef struct { char sid[64]; uint64_t seed; int pop; uint64_t end_tick, checksum;
                 double cfg[48]; PlayEvent *ev; int n; } Loaded;

/* parse a session file and APPLY its startup config (so seed+config are set for replay). */
static int load_session(const char *path, Loaded *L) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return -1; }
    memset(L, 0, sizeof *L); L->seed = 1337; L->pop = 150;
    int cap = 0; char line[256];
    set_fixed_step(1);   /* deterministic stepping; cfg lines below set the real dt */
    while (fgets(line, sizeof line, f)) {
        if      (!strncmp(line, "session ", 8)) sscanf(line+8, "%63s", L->sid);
        else if (!strncmp(line, "seed ", 5))    L->seed = strtoull(line+5, NULL, 10);
        else if (!strncmp(line, "pop ", 4))     L->pop = atoi(line+4);
        else if (!strncmp(line, "end_tick ", 9))L->end_tick = strtoull(line+9, NULL, 10);
        else if (!strncmp(line, "checksum ", 9))L->checksum = strtoull(line+9, NULL, 10);
        else if (!strncmp(line, "cfg ", 4)) {
            char name[32]; double val;
            if (sscanf(line+4, "%31s %lf", name, &val) == 2) {
                int k = knob_index(name); if (k >= 0) { L->cfg[k] = val; KNOBS[k].set(val); }
            }
        } else if (!strncmp(line, "event ", 6)) {
            unsigned long long t; char kind[8];
            if (sscanf(line+6, "%llu %7s", &t, kind) == 2) {
                if (L->n == cap) { cap = cap ? cap*2 : 64; L->ev = realloc(L->ev, (size_t)cap*sizeof(PlayEvent)); }
                PlayEvent *e = &L->ev[L->n++]; memset(e, 0, sizeof *e); e->tick = t;
                if (!strcmp(kind, "tune")) { e->is_tune = 1; sscanf(line+6, "%*s %*s %23s %lf", e->knob, &e->value); }
                else if (!strcmp(kind, "god")) { e->is_tune = 0; sscanf(line+6, "%*s %*s %d %d %d", &e->tool, &e->tx, &e->ty); }
            }
        }
    }
    fclose(f);
    return 0;
}

static void play_event(World *w, const PlayEvent *e) {
    if (e->is_tune) { int k = knob_index(e->knob); if (k >= 0) KNOBS[k].set(e->value); }
    else { char fl[128]; god_apply(w, e->tool, e->tx, e->ty, fl, sizeof fl); }
}

int replay_session_file(const char *path, int verbose) {
    Loaded L; if (load_session(path, &L) != 0) return -1;
    double dt = get_fixed_dt(); if (dt <= 0) dt = 0.25;
    World *w = malloc(sizeof(World)); if (!w) { free(L.ev); return -2; }
    world_init(w, L.seed);
    world_populate(w, L.pop);
    int ei = 0;
    while (w->tick < L.end_tick) {
        while (ei < L.n && L.ev[ei].tick == w->tick) play_event(w, &L.ev[ei++]);
        world_tick(w, dt);
    }
    while (ei < L.n && L.ev[ei].tick == L.end_tick) play_event(w, &L.ev[ei++]);  /* final-tick events */
    uint64_t got = world_checksum(w);
    int ok = (got == L.checksum);
    if (verbose) {
        printf("replay %s: seed=%llu pop=%d ticks=%llu events=%d\n", L.sid, (unsigned long long)L.seed, L.pop,
               (unsigned long long)L.end_tick, L.n);
        printf("  checksum recorded=%llu replayed=%llu  -> %s\n",
               (unsigned long long)L.checksum, (unsigned long long)got, ok ? "MATCH (byte-identical)" : "MISMATCH");
    }
    free(L.ev); free(w);
    return ok ? 0 : 1;
}

/* ── interactive player (GUI): step a recorded session, allow a fork on edit ─── */
static struct { int loaded, forked; char sid[64]; uint64_t seed; int pop, idx, n;
                uint64_t end_tick; double cfg[48]; PlayEvent *ev; } Pl;

int replay_load(const char *path, uint64_t *seed_out, int *pop_out) {
    Loaded L; if (load_session(path, &L) != 0) return -1;
    memset(&Pl, 0, sizeof Pl);
    Pl.loaded = 1; Pl.seed = L.seed; Pl.pop = L.pop; Pl.end_tick = L.end_tick;
    snprintf(Pl.sid, sizeof Pl.sid, "%s", L.sid);
    memcpy(Pl.cfg, L.cfg, sizeof Pl.cfg);
    Pl.ev = L.ev; Pl.n = L.n; Pl.idx = 0;
    if (seed_out) *seed_out = L.seed;
    if (pop_out)  *pop_out  = L.pop;
    return 0;
}
int      replay_in_progress(void)   { return Pl.loaded && !Pl.forked; }
int      replay_is_loaded(void)     { return Pl.loaded; }
uint64_t replay_end_tick(void)      { return Pl.end_tick; }
const char *replay_session_name(void){ return Pl.loaded ? Pl.sid : ""; }

void replay_apply_due(World *w) {
    if (!Pl.loaded || Pl.forked) return;
    while (Pl.idx < Pl.n && Pl.ev[Pl.idx].tick == w->tick) play_event(w, &Pl.ev[Pl.idx++]);
}

/* fork: from this tick on, stop replaying the parent and start recording a child
 * session seeded with the parent's startup config + the parent events already applied,
 * so the child replays byte-identically to the fork point then diverges. */
void replay_fork(const World *w) {
    if (!Pl.loaded || Pl.forked) return;
    Pl.forked = 1;
    memset(&S, 0, sizeof S);
    S.active = 1; S.seed = Pl.seed; S.pop = Pl.pop;
    snprintf(S.reason, sizeof S.reason, "fork");
    snprintf(S.parent_id, sizeof S.parent_id, "%s", Pl.sid);
    S.fork_tick = w->tick;
    snprintf(S.session_id, sizeof S.session_id, "%s-fork-%llu", Pl.sid, (unsigned long long)w->tick);
    memcpy(S.cfg, Pl.cfg, sizeof S.cfg);
    for (int i = 0; i < Pl.n; i++) {                      /* inherit parent events before the fork */
        if (Pl.ev[i].tick >= w->tick) break;
        RecEvent e; memset(&e, 0, sizeof e); e.tick = Pl.ev[i].tick;
        if (Pl.ev[i].is_tune) { snprintf(e.kind, sizeof e.kind, "tune");
            snprintf(e.knob, sizeof e.knob, "%s", Pl.ev[i].knob); e.value = Pl.ev[i].value; }
        else { snprintf(e.kind, sizeof e.kind, "god"); e.tool = Pl.ev[i].tool; e.tx = Pl.ev[i].tx; e.ty = Pl.ev[i].ty; }
        push_event(e);
    }
}
