/* main_headless.c — run the C core without graphics and print a report.
 * Proves the port works (and builds) with no external dependencies.
 *
 *   cc -O2 -o csim_headless src/main_headless.c src/sim.c -lm && ./csim_headless
 */
#include "sim.h"
#include <stdio.h>

int main(void) {
    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);

    int start = w.n_agents;
    printf("Emergent City — C core (headless)\n");
    printf("world %dx%d, %d agents, seed 1337\n\n", WORLD_W, WORLD_H, start);

    const double DT = 0.25;              /* real seconds per tick */
    const int DAYS = 6;
    long ticks = (long)(DAYS * 24 * SECONDS_PER_HOUR / DT);

    int prev_day = w.day;
    printf("day | alive | avgEnergy avgMoney avgHunger | deaths crimes\n");
    for (long i = 0; i < ticks; i++) {
        world_tick(&w, DT);
        if (w.day != prev_day) {
            prev_day = w.day;
            int alive = 0; double en = 0, mo = 0, hu = 0;
            for (int k = 0; k < w.n_agents; k++) {
                Agent *a = &w.agents[k];
                if (!a->alive) continue;
                alive++; en += a->needs.energy; mo += a->needs.money; hu += a->needs.hunger;
            }
            if (alive) { en /= alive; mo /= alive; hu /= alive; }
            printf("%3d | %5d | %8.2f %8.0f %9.2f | %6d %6d\n",
                   w.day, alive, en, mo, hu, w.deaths, w.crimes);
        }
    }

    /* action histogram on the final frame */
    long hist[A_COUNT] = {0};
    int alive = 0;
    for (int k = 0; k < w.n_agents; k++)
        if (w.agents[k].alive) { hist[w.agents[k].action]++; alive++; }
    printf("\nalive=%d/%d  deaths=%d  crimes=%d\n", alive, start, w.deaths, w.crimes);
    printf("final action mix:");
    for (int i = 0; i < A_COUNT; i++)
        if (hist[i]) printf(" %s=%ld", action_name((Action)i), hist[i]);
    printf("\n");
    return 0;
}
