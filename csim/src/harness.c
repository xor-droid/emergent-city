/* harness.c — controlled displacement harness (native C port of
 * tools/displacement_harness.py), linking the sim core directly.
 *
 * Comparing two policies in a stochastic sim by eyeballing one run each is noisy: a
 * different --police-bias changes the catch chance, which changes the RNG trajectory,
 * so the runs diverge for reasons unrelated to the policy. This uses common random
 * numbers — it runs each bias mode on the SAME seeds (paired) and averages the
 * per-seed difference, so trajectory noise cancels and the policy's spatial effect
 * on where crime lands stands out with an error bar.
 *
 * Displacement is measured SPATIALLY via World.crimes_loc_tier[] — crimes counted by
 * the affluence of the NEIGHBOURHOOD they happened in (loc poor/mid/rich). Every run
 * fixes --neighborhoods --crime-wealth; the LLM is never initialised, so runs are
 * deterministic (matching csim_headless with OPENROUTER unset).
 *
 * Build: linked as the csim_harness target (core sources + libm).
 * Usage: csim_harness [--seeds N] [--days D] [--modes crime,money,balanced]
 */
#define _POSIX_C_SOURCE 200809L
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_SEEDS 2000
#define MAX_MODES 8

static int police_bias_of(const char *m) {
    if (!m) return 0;
    if (!strcmp(m, "money") || !strcmp(m, "1")) return 1;
    if (!strcmp(m, "balanced") || !strcmp(m, "2")) return 2;
    return 0;   /* crime / 0 / anything else */
}

/* run one sim in-process; return the crime counts by neighbourhood affluence tier */
static void run_one(unsigned int seed, int days, int bias, int loc[3]) {
    set_neighborhoods(1);
    set_crime_wealth(1);
    set_police_bias(bias);
    World *w = malloc(sizeof(World));          /* ~1MB — too big for the stack */
    if (!w) { fprintf(stderr, "out of memory\n"); exit(1); }
    /* silence the core's [roles]/[law] diagnostics during the run */
    fflush(stdout); fflush(stderr);
    int so = dup(1), se = dup(2), dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
    world_init(w, seed);
    world_populate(w, 150);
    const double DT = 0.25;
    long ticks = (long)(days * 24 * SECONDS_PER_HOUR / DT);
    for (long i = 0; i < ticks; i++) world_tick(w, DT);   /* LLM never inited → off, deterministic */
    fflush(stdout); fflush(stderr);
    if (dn >= 0) { dup2(so, 1); dup2(se, 2); close(dn); }
    close(so); close(se);
    loc[0] = w->crimes_loc_tier[0]; loc[1] = w->crimes_loc_tier[1]; loc[2] = w->crimes_loc_tier[2];
    free(w);
}

static double mean_of(const double *x, int n) { double s = 0; for (int i = 0; i < n; i++) s += x[i]; return n ? s/n : 0; }
static double stderr_of(const double *x, int n) {
    if (n < 2) return 0; double m = mean_of(x, n), s = 0;
    for (int i = 0; i < n; i++) s += (x[i]-m)*(x[i]-m);
    return sqrt(s/(n-1)) / sqrt((double)n);
}

int main(int argc, char **argv) {
    int seeds = 20, days = 30;
    char modebuf[128] = "crime,money,balanced";
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--seeds") && i+1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--days")  && i+1 < argc) days  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--modes") && i+1 < argc) { snprintf(modebuf, sizeof modebuf, "%s", argv[++i]); }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [--seeds N] [--days D] [--modes crime,money,balanced]\n", argv[0]); return 0;
        } else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (seeds < 1) seeds = 1; if (seeds > MAX_SEEDS) seeds = MAX_SEEDS;

    char modes[MAX_MODES][16]; int nmodes = 0;
    for (char *tok = strtok(modebuf, ","); tok && nmodes < MAX_MODES; tok = strtok(NULL, ","))
        snprintf(modes[nmodes++], 16, "%s", tok);

    static double poor[MAX_MODES][MAX_SEEDS], rich[MAX_MODES][MAX_SEEDS];

    printf("police-bias displacement harness (native) — %d seeds x %d days x [", seeds, days);
    for (int m = 0; m < nmodes; m++) printf("%s%s", m ? ", " : "", modes[m]);
    printf("]\n  common random numbers (paired by seed), LLM off, --neighborhoods --crime-wealth\n");
    printf("  spatial displacement: crimes by the affluence of the NEIGHBOURHOOD they occur in\n\n");

    for (int s = 0; s < seeds; s++) {
        for (int m = 0; m < nmodes; m++) {
            int loc[3]; run_one((unsigned)(s+1), days, police_bias_of(modes[m]), loc);
            int tot = loc[0] + loc[1] + loc[2];
            poor[m][s] = tot ? 100.0 * loc[0] / tot : 0.0;
            rich[m][s] = tot ? 100.0 * loc[2] / tot : 0.0;
        }
        printf("\r  ran seed %d/%d", s+1, seeds); fflush(stdout);
    }
    printf("\n\n");

    printf("  %-10s%16s%16s\n", "mode", "poor-block %", "rich-block %");
    for (int m = 0; m < nmodes; m++)
        printf("  %-10s%10.1f \xc2\xb1%-3.1f%10.1f \xc2\xb1%-3.1f\n", modes[m],
               mean_of(poor[m], seeds), stderr_of(poor[m], seeds),
               mean_of(rich[m], seeds), stderr_of(rich[m], seeds));

    int base = -1;
    for (int m = 0; m < nmodes; m++) if (!strcmp(modes[m], "crime")) base = m;
    if (base >= 0) {
        printf("\n  paired vs 'crime' (per-seed diff, so trajectory noise cancels):\n");
        for (int m = 0; m < nmodes; m++) {
            if (m == base) continue;
            double dpoor[MAX_SEEDS], drich[MAX_SEEDS];
            for (int s = 0; s < seeds; s++) { dpoor[s] = poor[m][s] - poor[base][s]; drich[s] = rich[m][s] - rich[base][s]; }
            double pm = mean_of(dpoor, seeds), pse = stderr_of(dpoor, seeds);
            double rm = mean_of(drich, seeds), rse = stderr_of(drich, seeds);
            const char *sig = (fabs(pm) > 2*pse && pse > 0) ? "significant" : "not significant";
            printf("    %9s: poor-block share %+.1f pp \xc2\xb1%.1f  (crime displaced %s to poor blocks; %s)\n",
                   modes[m], pm, pse, pm > 0 ? "more" : "less", sig);
            printf("    %9s  rich-block share %+.1f pp \xc2\xb1%.1f\n", "", rm, rse);
        }
    }
    return 0;
}
