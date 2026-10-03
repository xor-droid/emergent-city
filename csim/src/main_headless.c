/* main_headless.c — run the full C core without graphics and print a report.
 * Builds with just a C compiler + libm.
 *   cc -O2 -o csim_headless src/main_headless.c src/sim.c src/systems.c src/world.c -lm
 */
#include "sim.h"
#include "llm.h"
#include <stdio.h>

int main(void) {
    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);
    dotenv_autoload();   /* pick up the project .env */
    llm_init();          /* set OPENROUTER_API_KEY to enable Qwen consults */
    int start = w.n_agents;

    int police = 0;
    for (int i = 0; i < w.n_agents; i++) police += w.agents[i].is_police;

    printf("Emergent City — C core (headless, full port)\n");
    printf("world %dx%d, %d agents (%d police), %d buildings, %d factions, seed 1337\n\n",
           WORLD_W, WORLD_H, start, police, w.n_buildings, w.n_factions);

    const double DT = 0.25;
    const int DAYS = 6;
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

    /* friendship + faction summary */
    int alive = 0, total_friends = 0, max_friends = 0, in_faction = 0;
    int married = 0, pregnant = 0, born = 0;
    for (int k = 0; k < w.n_agents; k++) {
        Agent *a = &w.agents[k];
        if (a->mother_id >= 0) born++;    /* born into the city during the run */
        if (!a->alive) continue;
        alive++;
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
            printf(" %s(%d)", w.factions[i].name, w.factions[i].members);
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
