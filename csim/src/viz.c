/* viz.c — shared rendering helpers (no backend-specific types). */
#include "viz.h"
#include "llm.h"
#include <stdio.h>

const char *g_blit = "default";

void tile_rgb(TileType t, unsigned char *r, unsigned char *g, unsigned char *b) {
    unsigned char R, G, B;
    switch (t) {
        case T_ROAD:   R = 48;  G = 44;  B = 52;  break;
        case T_HOME:   R = 162; G = 122; B = 96;  break;
        case T_SHOP:   R = 170; G = 140; B = 80;  break;
        case T_WORK:   R = 90;  G = 90;  B = 95;  break;
        case T_BAR:    R = 150; G = 110; B = 170; break;
        case T_CHURCH: R = 200; G = 180; B = 150; break;
        case T_POLICE: R = 70;  G = 90;  B = 160; break;
        default:       R = 52;  G = 78;  B = 58;  break;  /* grass */
    }
    *r = R; *g = G; *b = B;
}

void hud_string(const World *w, char *buf, int n, float speed, int paused,
                int fps, const char *backend) {
    int alive = 0, factions = 0;
    for (int i = 0; i < w->n_agents; i++) alive += w->agents[i].alive;
    for (int i = 0; i < w->n_factions; i++) factions += (w->factions[i].active && w->factions[i].members > 0);
    snprintf(buf, (size_t)n,
             "Day %d  %02d:00  Pop %d  Deaths %d  Crimes %d  Wanted %d  Jail %d  Factions %d  "
             "LLM %d  [x%.0f%s]  %d FPS (%s)",
             w->day, (int)w->hour, alive, w->deaths, w->crimes,
             crime_wanted_count(w), crime_jailed_count(w), factions,
             llm_total_calls(), speed, paused ? " PAUSED" : "", fps, backend);
}
