/* record.c — session recording + deterministic replay (see record.h). */
#include "record.h"
#include "viz.h"        /* god_apply */
#include "llm.h"        /* llm_enabled */
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
    char session_id[64];
    char reason[96];
    uint64_t seed; int pop;
    double cfg[32];              /* startup snapshot, parallel to KNOBS */
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
void rec_end(const World *w) {
    if (!S.active) return;
    S.end_tick = w->tick; S.checksum = world_checksum(w);
}

int rec_save_file(const char *path) {
    if (!S.active || !path || !*path) return 0;
    FILE *f = fopen(path, "w"); if (!f) return 0;
    fprintf(f, "session %s\n", S.session_id);
    fprintf(f, "reason %s\n", S.reason);
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

int replay_session_file(const char *path, int verbose) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return -1; }

    uint64_t seed = 1337, end_tick = 0, want_sum = 0; int pop = 150; char sid[64] = "";
    PlayEvent *ev = NULL; int nev = 0, cap = 0;
    char line[256];
    /* default fixed-step for a deterministic re-run; the cfg lines below set the real dt */
    set_fixed_step(1);
    while (fgets(line, sizeof line, f)) {
        if      (!strncmp(line, "session ", 8)) sscanf(line+8, "%63s", sid);
        else if (!strncmp(line, "seed ", 5))    seed = strtoull(line+5, NULL, 10);
        else if (!strncmp(line, "pop ", 4))     pop = atoi(line+4);
        else if (!strncmp(line, "end_tick ", 9))end_tick = strtoull(line+9, NULL, 10);
        else if (!strncmp(line, "checksum ", 9))want_sum = strtoull(line+9, NULL, 10);
        else if (!strncmp(line, "cfg ", 4)) {
            char name[32]; double val;
            if (sscanf(line+4, "%31s %lf", name, &val) == 2) { int k = knob_index(name); if (k >= 0) KNOBS[k].set(val); }
        } else if (!strncmp(line, "event ", 6)) {
            unsigned long long t; char kind[8];
            if (sscanf(line+6, "%llu %7s", &t, kind) == 2) {
                if (nev == cap) { cap = cap ? cap*2 : 64; ev = realloc(ev, (size_t)cap*sizeof(PlayEvent)); }
                PlayEvent *e = &ev[nev++]; memset(e, 0, sizeof *e); e->tick = t;
                if (!strcmp(kind, "tune")) { e->is_tune = 1; sscanf(line+6, "%*s %*s %23s %lf", e->knob, &e->value); }
                else if (!strcmp(kind, "god")) { e->is_tune = 0; sscanf(line+6, "%*s %*s %d %d %d", &e->tool, &e->tx, &e->ty); }
            }
        }
    }
    fclose(f);

    double dt = get_fixed_dt(); if (dt <= 0) dt = 0.25;
    World *w = malloc(sizeof(World)); if (!w) { free(ev); return -2; }
    world_init(w, seed);
    world_populate(w, pop);

    int ei = 0;
    while (w->tick < end_tick) {
        while (ei < nev && ev[ei].tick == w->tick) {           /* apply this tick's events, then step */
            PlayEvent *e = &ev[ei++];
            if (e->is_tune) { int k = knob_index(e->knob); if (k >= 0) KNOBS[k].set(e->value); }
            else { char fl[128]; god_apply(w, e->tool, e->tx, e->ty, fl, sizeof fl); }
        }
        world_tick(w, dt);
    }
    while (ei < nev && ev[ei].tick == end_tick) {              /* final-tick interventions (no step after) */
        PlayEvent *e = &ev[ei++];
        if (e->is_tune) { int k = knob_index(e->knob); if (k >= 0) KNOBS[k].set(e->value); }
        else { char fl[128]; god_apply(w, e->tool, e->tx, e->ty, fl, sizeof fl); }
    }

    uint64_t got = world_checksum(w);
    int ok = (got == want_sum);
    if (verbose) {
        printf("replay %s: seed=%llu pop=%d ticks=%llu events=%d\n", sid, (unsigned long long)seed, pop,
               (unsigned long long)end_tick, nev);
        printf("  checksum recorded=%llu replayed=%llu  -> %s\n",
               (unsigned long long)want_sum, (unsigned long long)got, ok ? "MATCH (byte-identical)" : "MISMATCH");
    }
    free(ev); free(w);
    return ok ? 0 : 1;
}
