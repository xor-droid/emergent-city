/* viz.c — shared rendering helpers (no backend-specific types). */
#include "viz.h"
#include "llm.h"
#include <stdio.h>
#include <stdlib.h>

const char *g_blit = "default";

const char *GOD_TOOL_NAME[G_NTOOLS] =
    {"Smite","Bless","Starve","Incite","Spawn","Gang","Cult","Riot"};

static Agent *near_other(World *w, int tx, int ty, int radius, int except_id) {
    Agent *best = NULL; int bd = radius * radius + 1;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->id == except_id) continue;
        int d = ((int)a->x - tx) * ((int)a->x - tx) + ((int)a->y - ty) * ((int)a->y - ty);
        if (d <= bd) { bd = d; best = a; }
    }
    return best;
}

void god_apply(World *w, int tool, int tx, int ty, char *flash, int fn) {
    Agent *a = world_agent_at(w, tx, ty, 5);
    switch (tool) {
        case G_SMITE:
            if (a) { a->alive = 0; w->deaths++;
                char t[96]; snprintf(t, sizeof(t), "%s smited by god", a->name);
                events_post(w, EV_DEATH, a->id, -1, (int)a->x, (int)a->y, 0.9, t);
                snprintf(flash, fn, "Smote %s", a->name);
            } else snprintf(flash, fn, "No one there");
            break;
        case G_BLESS:
            if (a) { Needs *n = &a->needs; n->hunger=n->energy=n->safety=1;
                n->social=n->meaning=n->belonging=1; n->money += 100;
                snprintf(flash, fn, "Blessed %s", a->name);
            } else snprintf(flash, fn, "No one there");
            break;
        case G_STARVE:
            if (a) { a->needs.hunger = 0.03; a->needs.energy = 0.03;
                snprintf(flash, fn, "Starved %s", a->name);
            } else snprintf(flash, fn, "No one there");
            break;
        case G_INCITE:
            if (a) { Agent *tg = near_other(w, (int)a->x, (int)a->y, 4, a->id);
                crime_attempt(w, a, tg, tg ? "assault" : "theft");
                snprintf(flash, fn, "%s turns to crime", a->name);
            } else snprintf(flash, fn, "No one there");
            break;
        case G_SPAWN: {
            int id = world_spawn_agent(w, tx, ty);
            snprintf(flash, fn, "%s", id >= 0 ? "Spawned citizen" : "No room");
            break; }
        case G_GANG: {
            int id = factions_raise(w, 0, tx, ty);
            snprintf(flash, fn, "%s", id >= 0 ? "Raised a gang" : "Not enough recruits");
            break; }
        case G_CULT: {
            int id = factions_raise(w, 1, tx, ty);
            snprintf(flash, fn, "%s", id >= 0 ? "Raised a cult" : "Not enough recruits");
            break; }
        case G_RIOT: {
            int n = 0;
            for (int i = 0; i < w->n_agents; i++) {
                Agent *a2 = &w->agents[i];
                if (!a2->alive || a2->is_police) continue;
                if (abs((int)a2->x - tx) <= 10 && abs((int)a2->y - ty) <= 10) {
                    a2->needs.safety = 0.2;
                    if (rng_double(&w->rng) < 0.7) {
                        crime_attempt(w, a2, near_other(w, (int)a2->x, (int)a2->y, 3, a2->id), "riot");
                        n++;
                    }
                }
            }
            snprintf(flash, fn, "Riot! %d rioters", n);
            break; }
        default: break;
    }
}

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
