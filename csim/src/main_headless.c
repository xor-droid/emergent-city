/* main_headless.c — run the full C core without graphics and print a report.
 * Builds with just a C compiler + libm.
 *   cc -O2 -o csim_headless src/main_headless.c src/sim.c src/systems.c src/world.c -lm
 *
 * Flags (all also available as CSIM_* env vars):
 *   --seed N            worldgen seed (default 1337)
 *   --days N            game-days to simulate (default 6)
 *   --metrics PATH      append one CSV row of all daily aggregates per game-day to PATH
 *                       (live; also writes PATH.meta recording seed+config for re-run)
 *   --replay IN.csv     stream a recorded CSV back out (to --metrics, else stdout) one
 *     [--replay-interval SECS]   row at a time (default 0.3s) so a dashboard animates it
 *   --rerun IN.meta     deterministically re-run from a recorded manifest (seed+config)
 */
#define _POSIX_C_SOURCE 200809L
#include "sim.h"
#include "llm.h"
#include "record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void husage(const char *a0) {
    printf("usage: %s [--seed N] [--days N] [--metrics PATH]\n", a0);
    printf("       %s --replay IN.csv [--metrics OUT.csv] [--replay-interval SECS]\n", a0);
    printf("       %s --rerun IN.meta [--metrics OUT.csv]\n", a0);
    printf("\n");
    printf("Runs the full city core headless and prints an end-of-run report. With\n");
    printf("--metrics it also writes one CSV row of ALL daily aggregates per game-day\n");
    printf("(and PATH.meta: seed+config) for the live web dashboard in tools/dashboard/.\n");
    printf("\n");
    printf("  --seed N              worldgen seed (default 1337); same seed+config = same run.\n");
    printf("  --days N              game-days to simulate (default 6).\n");
    printf("  --metrics PATH        live balance CSV export (point at the dashboard dir).\n");
    printf("  --metrics-every H     sample cadence in game-hours (default 24 = once/day; 1 = hourly).\n");
    printf("  --metrics-hourly      shorthand for --metrics-every 1.\n");
    printf("  --replay IN.csv       replay a recorded run: stream its rows to --metrics (or\n");
    printf("                        stdout) at --replay-interval seconds each (pure playback,\n");
    printf("                        no simulation). Lets the dashboard animate a past session.\n");
    printf("  --replay-interval S   seconds between replayed rows (default 0.3).\n");
    printf("  --record PATH         record this run as a replayable session (seed+config+events).\n");
    printf("  --replay-session FILE re-run a recorded session deterministically and verify its\n");
    printf("                        final-state checksum (byte-identical proof).\n");
    printf("  --rerun IN.meta       re-run deterministically from a PATH.meta manifest; the\n");
    printf("                        regenerated CSV reproduces the original run byte-for-byte\n");
    printf("                        (with the LLM off — OPENROUTER_* unset — as reproducibility\n");
    printf("                        always requires; an LLM consult makes the run non-deterministic).\n");
    printf("  -h, --help            show this help.\n");
    printf("\n");
    printf("Config knobs (env, applied unless --rerun overrides them):\n");
    printf("  CSIM_YEARS_PER_DAY CSIM_FAMILY_SHARE CSIM_FAMILY_KIDS_MIN/MAX CSIM_POP_TARGET\n");
    printf("  CSIM_RESEARCH_RATE CSIM_PRODUCTION CSIM_CHILD_COST CSIM_VISION[_RADIUS]\n");
    printf("  CSIM_HEARING[_RADIUS] CSIM_NOISE_WORLDGEN CSIM_SEED CSIM_DAYS CSIM_METRICS\n");
    printf("  CSIM_OCC_PAY_SPREAD (--occ-pay-spread F: 0 flat/legacy .. 1 full occupation pay tiers)\n");
    printf("  CSIM_NEIGHBORHOODS  (--neighborhoods: home value + residential sorting by wealth)\n");
    printf("  CSIM_CRIME_WEALTH   (--crime-wealth: loot scales with target wealth + EV targeting)\n");
    printf("  CSIM_POLICE_BIAS    (crime|money|balanced: where police concentrate; needs --neighborhoods)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --days 40 --metrics tools/dashboard/metrics.csv   record a 40-day run\n", a0);
    printf("  %s --replay old.csv --metrics tools/dashboard/metrics.csv   animate it\n", a0);
    printf("  %s --rerun tools/dashboard/metrics.csv.meta          reproduce that run\n", a0);
}

static void sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* Playback: copy IN.csv to out_path (or stdout) one row at a time, sleeping between
 * data rows, so a polling dashboard animates the recorded run. No simulation. */
static int do_replay(const char *in_path, const char *out_path, double interval) {
    FILE *in = fopen(in_path, "r");
    if (!in) { fprintf(stderr, "replay: cannot open %s\n", in_path); return 1; }
    FILE *out = (out_path && *out_path) ? fopen(out_path, "w") : stdout;
    if (!out) { fprintf(stderr, "replay: cannot open %s\n", out_path); fclose(in); return 1; }
    char line[16384];
    int lineno = 0, rows = 0;
    while (fgets(line, sizeof line, in)) {
        fputs(line, out);
        fflush(out);
        if (lineno++ == 0) continue;     /* header row: emit immediately, no delay */
        rows++;
        sleep_seconds(interval);
    }
    fclose(in);
    if (out != stdout) fclose(out);
    fprintf(stderr, "replay: streamed %d row(s) from %s%s%s at %.2fs/row\n",
            rows, in_path, (out_path && *out_path) ? " -> " : "",
            (out_path && *out_path) ? out_path : "", interval);
    return 0;
}

/* Apply a KEY=VALUE manifest (written by metrics_write_manifest) to the global knobs.
 * Fills *seed and *days from it. Returns 0 on success. */
static int load_manifest(const char *path, long *seed, int *days) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "rerun: cannot open %s\n", path); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line, *val = eq + 1;
        double d = atof(val); int iv = atoi(val);
        if      (!strcmp(key, "seed"))           *seed = atol(val);
        else if (!strcmp(key, "days"))           *days = iv;
        else if (!strcmp(key, "years_per_day"))  set_years_per_day(d);
        else if (!strcmp(key, "family_share"))   set_family_share(d);
        else if (!strcmp(key, "kids_min"))       set_family_kids(iv, get_family_kids_max());
        else if (!strcmp(key, "kids_max"))       set_family_kids(get_family_kids_min(), iv);
        else if (!strcmp(key, "pop_target"))     set_pop_target(iv);
        else if (!strcmp(key, "research_rate"))  set_research_rate(d);
        else if (!strcmp(key, "production"))      set_craft_bonus(d);
        else if (!strcmp(key, "child_cost"))      set_child_cost(d);
        else if (!strcmp(key, "metrics_every"))   set_metrics_every(d);
        else if (!strcmp(key, "occ_pay_spread"))  set_occ_pay_spread(d);
        else if (!strcmp(key, "neighborhoods"))   set_neighborhoods(iv);
        else if (!strcmp(key, "crime_wealth"))    set_crime_wealth(iv);
        else if (!strcmp(key, "police_bias"))     set_police_bias(iv);
        else if (!strcmp(key, "vision"))          set_vision(iv);
        else if (!strcmp(key, "vision_radius"))   set_vision_radius(iv);
        else if (!strcmp(key, "hearing"))         set_hearing(iv);
        else if (!strcmp(key, "hearing_radius"))  set_hearing_radius(iv);
        else if (!strcmp(key, "noise_worldgen"))  set_noise_worldgen(iv);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *metrics = NULL, *replay_in = NULL, *rerun_in = NULL;
    const char *record = NULL, *replay_session = NULL;
    double replay_interval = 0.3;
    double mevery = -1.0;   /* metrics cadence (game-hours); <0 = unset, CLI wins */
    double ospread = -1.0;  /* occupation pay-tier spread; <0 = unset, CLI wins */
    long seed = -1;      /* <0 = unset */
    int  days = -1;      /* <0 = unset */

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { husage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)            seed = atol(argv[++i]);
        else if (!strncmp(argv[i], "--seed=", 7))                        seed = atol(argv[i] + 7);
        else if (!strcmp(argv[i], "--days") && i + 1 < argc)             days = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--days=", 7))                        days = atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "--metrics") && i + 1 < argc)          metrics = argv[++i];
        else if (!strncmp(argv[i], "--metrics=", 10))                    metrics = argv[i] + 10;
        else if (!strcmp(argv[i], "--metrics-every") && i + 1 < argc)    mevery = atof(argv[++i]);
        else if (!strncmp(argv[i], "--metrics-every=", 16))              mevery = atof(argv[i] + 16);
        else if (!strcmp(argv[i], "--metrics-hourly"))                   mevery = 1.0;
        else if (!strcmp(argv[i], "--occ-pay-spread") && i + 1 < argc)   ospread = atof(argv[++i]);
        else if (!strncmp(argv[i], "--occ-pay-spread=", 17))             ospread = atof(argv[i] + 17);
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc)           replay_in = argv[++i];
        else if (!strncmp(argv[i], "--replay=", 9))                      replay_in = argv[i] + 9;
        else if (!strcmp(argv[i], "--replay-interval") && i + 1 < argc)  replay_interval = atof(argv[++i]);
        else if (!strncmp(argv[i], "--replay-interval=", 18))            replay_interval = atof(argv[i] + 18);
        else if (!strcmp(argv[i], "--rerun") && i + 1 < argc)            rerun_in = argv[++i];
        else if (!strncmp(argv[i], "--rerun=", 8))                       rerun_in = argv[i] + 8;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc)           record = argv[++i];
        else if (!strncmp(argv[i], "--record=", 9))                      record = argv[i] + 9;
        else if (!strcmp(argv[i], "--replay-session") && i + 1 < argc)   replay_session = argv[++i];
        else if (!strncmp(argv[i], "--replay-session=", 17))             replay_session = argv[i] + 17;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); husage(argv[0]); return 2; }
    }

    if (!metrics) metrics = getenv("CSIM_METRICS");

    /* Replay mode: pure playback of a recorded CSV — no simulation. */
    if (replay_in)
        return do_replay(replay_in, metrics, replay_interval);

    /* Session replay: re-run a recorded session deterministically + verify checksum. */
    if (replay_session)
        return replay_session_file(replay_session, 1);

    /* --- config knobs: env first; --rerun manifest overrides; CLI seed/days last --- */
    { const char *e = getenv("CSIM_YEARS_PER_DAY"); if (e) set_years_per_day(atof(e)); }
    { const char *e = getenv("CSIM_FAMILY_SHARE"); if (e) set_family_share(atof(e)); }
    { const char *lo = getenv("CSIM_FAMILY_KIDS_MIN"), *hi = getenv("CSIM_FAMILY_KIDS_MAX");
      if (lo || hi) set_family_kids(lo ? atoi(lo) : get_family_kids_min(), hi ? atoi(hi) : get_family_kids_max()); }
    { const char *e = getenv("CSIM_POP_TARGET"); if (e) set_pop_target(atoi(e)); }
    { const char *e = getenv("CSIM_RESEARCH_RATE"); if (e) set_research_rate(atof(e)); }
    { const char *e = getenv("CSIM_PRODUCTION"); if (e) set_craft_bonus(atof(e)); }
    { const char *e = getenv("CSIM_CHILD_COST"); if (e) set_child_cost(atof(e)); }
    { const char *e = getenv("CSIM_VISION_RADIUS"); if (e) set_vision_radius(atoi(e)); }
    { const char *e = getenv("CSIM_VISION"); if (e) set_vision(atoi(e)); }
    { const char *e = getenv("CSIM_HEARING_RADIUS"); if (e) set_hearing_radius(atoi(e)); }
    { const char *e = getenv("CSIM_HEARING"); if (e) set_hearing(atoi(e)); }
    { const char *e = getenv("CSIM_NOISE_WORLDGEN"); if (e) set_noise_worldgen(atoi(e)); }
    { const char *e = getenv("CSIM_METRICS_EVERY"); if (e) set_metrics_every(atof(e)); }
    { const char *e = getenv("CSIM_OCC_PAY_SPREAD"); if (e) set_occ_pay_spread(atof(e)); }
    { const char *e = getenv("CSIM_NEIGHBORHOODS"); if (e) set_neighborhoods(atoi(e)); }
    { const char *e = getenv("CSIM_CRIME_WEALTH"); if (e) set_crime_wealth(atoi(e)); }
    { const char *e = getenv("CSIM_POLICE_BIAS");
      if (e) set_police_bias((!strcmp(e,"money")||!strcmp(e,"1")) ? 1 : (!strcmp(e,"balanced")||!strcmp(e,"2")) ? 2 : 0); }

    if (rerun_in) {
        long mseed = -1; int mdays = -1;
        if (load_manifest(rerun_in, &mseed, &mdays) != 0) return 1;
        if (seed < 0 && mseed >= 0) seed = mseed;   /* CLI --seed still wins if given */
        if (days < 0 && mdays >= 0) days = mdays;
        fprintf(stderr, "rerun: reproducing %s (seed=%ld, days=%d)\n", rerun_in, seed, days);
    }

    if (mevery > 0.0) set_metrics_every(mevery);   /* CLI --metrics-every wins over env/manifest */
    if (ospread >= 0.0) set_occ_pay_spread(ospread);  /* CLI --occ-pay-spread wins over env/manifest */

    if (seed < 0) { const char *e = getenv("CSIM_SEED"); if (e) seed = atol(e); }
    if (seed < 0) seed = 1337;
    if (days < 0) { const char *e = getenv("CSIM_DAYS"); if (e) days = atoi(e); }
    if (days < 0) days = 6;

    World w;
    world_init(&w, (unsigned int)seed);
    world_populate(&w, 150);
    if (metrics && *metrics) {
        metrics_open(metrics);
        metrics_write_manifest(metrics, (unsigned int)seed, days);
    }
    if (!record) record = getenv("CSIM_RECORD");
    if (record && *record) rec_begin((unsigned int)seed, 150, "headless");   /* snapshot config */
    dotenv_autoload();   /* pick up the project .env */
    llm_init();          /* set OPENROUTER_API_KEY to enable Qwen consults */
    int start = w.n_agents;

    int police = 0;
    for (int i = 0; i < w.n_agents; i++) police += w.agents[i].is_police;

    printf("Emergent City — C core (headless, full port)\n");
    printf("world %dx%d, %d agents (%d police), %d buildings, %d factions, seed %ld\n\n",
           WORLD_W, WORLD_H, start, police, w.n_buildings, w.n_factions, seed);

    const double DT = 0.25;
    const int DAYS = days;
    long ticks = (long)(DAYS * 24 * SECONDS_PER_HOUR / DT);

    int prev_day = w.day;
    printf("day | alive | avgMoney | wanted jail | deaths crimes\n");
    for (long i = 0; i < ticks; i++) {
        world_tick(&w, DT);
        if (w.day != prev_day) {
            prev_day = w.day;
            int alive = 0; double mo = 0;
            for (int k = 0; k < w.n_agents; k++)
                if (w.agents[k].alive) { alive++; mo += w.agents[k].needs.money; }
            if (alive) mo /= alive;
            printf("%3d | %5d | %8.0f | %6d %4d | %6d %6d\n",
                   w.day, alive, mo, crime_wanted_count(&w), crime_jailed_count(&w),
                   w.deaths, w.crimes);
        }
    }

    if (rec_active()) {
        rec_end(&w);
        if (rec_save_file(record)) printf("\nrecorded session -> %s (tick=%llu, checksum=%llu)\n",
                                          record, (unsigned long long)w.tick, (unsigned long long)world_checksum(&w));
    }

    /* friendship + faction summary */
    int alive = 0, total_friends = 0, max_friends = 0, in_faction = 0;
    int married = 0, pregnant = 0, born = 0;
    int children = 0, youths = 0, adults = 0, elders = 0; long age_sum = 0;
    for (int k = 0; k < w.n_agents; k++) {
        Agent *a = &w.agents[k];
        if (a->mother_id >= 0) born++;    /* born into the city during the run */
        if (!a->alive) continue;
        alive++;
        age_sum += a->age;
        switch (life_stage(a)) { case LS_CHILD: children++; break; case LS_YOUTH: youths++; break;
                                 case LS_ELDER: elders++; break; default: adults++; }
        int fr = 0;
        for (int j = 0; j < a->rels.n; j++)
            if (a->rels.rel[j].affinity >= FRIENDSHIP_AFFINITY) fr++;
        total_friends += fr;
        if (fr > max_friends) max_friends = fr;
        if (a->faction_id != -1) in_faction++;
        if (a->spouse_id >= 0) married++;
        if (a->pregnant_ticks > 0) pregnant++;
    }
    printf("\nalive=%d/%d  deaths=%d  crimes=%d  llm_enabled=%d  llm_calls=%d\n",
           alive, start, w.deaths, w.crimes, llm_enabled(), llm_total_calls());
    printf("avg friends/agent=%.1f (max %d)   agents in a faction=%d\n",
           alive ? (double)total_friends / alive : 0.0, max_friends, in_faction);
    printf("family: married=%d (couples %d)  pregnant=%d  children born=%d\n",
           married, married / 2, pregnant, born);
    printf("life cycle (%.1f yr/day): avg age=%ld  children=%d youths=%d adults=%d elders=%d\n",
           get_years_per_day(), alive ? age_sum / alive : 0, children, youths, adults, elders);
    { double edu_sum = 0, int_sum = 0, craft_sum = 0; int na2 = 0;
      for (int k = 0; k < w.n_agents; k++) if (w.agents[k].alive) { edu_sum += w.agents[k].education; int_sum += w.agents[k].intellect; craft_sum += w.agents[k].craft; na2++; }
      printf("knowledge: theories=%d  research=%.0f  avg_edu=%.0f%%  avg_craft=%.0f%%  tech:",
             w.sci.theories, w.sci.research, na2 ? edu_sum/na2*100 : 0, na2 ? craft_sum/na2*100 : 0);
      int any = 0;
      for (int t = 0; t < TECH_COUNT; t++) if (w.sci.discovered[t]) { printf(" %s(%.0f%%)", tech_name(t), w.sci.adoption[t]*100); any = 1; }
      if (!any) printf(" none");
      printf("\n");
    }
    { int landlords = 0, indebted = 0; double debt_sum = 0;
      for (int k = 0; k < w.n_agents; k++) {
          Agent *a = &w.agents[k];
          if (!a->alive) continue;
          if (count_properties(&w, a->id) > 0) landlords++;
          if (a->debt > 0.5) { indebted++; debt_sum += a->debt; }
      }
      printf("economy: goods_price=%.2fx  wage=%.2fx  landlords=%d  indebted=%d  total_debt=%.0f\n",
             w.econ.goods_price, w.econ.wage_mult,
             landlords, indebted, debt_sum);
    }
    printf("factions:");
    for (int i = 0; i < w.n_factions; i++)
        if (w.factions[i].active && w.factions[i].members)
            printf(" %s(%d%s)", w.factions[i].name, w.factions[i].members,
                   w.factions[i].war_with >= 0 ? ",WAR" : "");
    { int wars = 0, war_cas = 0;
      for (int i = 0; i < w.n_factions; i++) {
          if (w.factions[i].war_with > i) wars++;   /* count each pair once */
          war_cas += w.factions[i].casualties;
      }
      printf("\ngovernance: wars active=%d  war casualties=%d  crackdown=%s\n",
             wars, war_cas, w.crackdown_days > 0 ? "ON" : "off");
    }
    { int faith[FAITH_COUNT] = {0}, lang[LANG_COUNT] = {0}; double edu_sum = 0; int na = 0;
      for (int k = 0; k < w.n_agents; k++) {
          Agent *a = &w.agents[k];
          if (!a->alive) continue;
          na++; faith[a->faith]++; lang[a->language]++; edu_sum += a->education;
      }
      printf("culture: avg education=%.0f%%  religious=%d/%d  Common-tongue=%d/%d\n",
             na ? edu_sum / na * 100 : 0, na - faith[FAITH_NONE], na, lang[LANG_COMMON], na);
      printf("  faith:");
      for (int i = 0; i < FAITH_COUNT; i++) if (faith[i]) printf(" %s=%d", faith_name((unsigned char)i), faith[i]);
      printf("\n  tongues:");
      for (int i = 0; i < LANG_COUNT; i++) if (lang[i]) printf(" %s=%d", language_name((unsigned char)i), lang[i]);
      printf("\n");
    }
    int ehist[EV_KIND_COUNT] = {0};
    for (int i = 0; i < w.ev_count; i++) {
        const WorldEvent *e = events_recent(&w, i);
        if (e) ehist[e->kind]++;
    }
    printf("\nrecent event kinds (last %d):", w.ev_count);
    for (int i = 0; i < EV_KIND_COUNT; i++)
        if (ehist[i]) printf(" %s=%d", event_kind_name((EventKind)i), ehist[i]);
    printf("\n");
    printf("crimes by kind:");
    for (int i = 0; i < CK_COUNT; i++)
        if (w.crime_kind[i]) printf(" %s=%d", crime_kind_name(i), w.crime_kind[i]);
    printf("\n");
    llm_shutdown();
    return 0;
}
