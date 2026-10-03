/* viz.c — shared rendering helpers (no backend-specific types). */
#include "viz.h"
#include "llm.h"
#include <stdio.h>
#include <stdlib.h>


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
        case T_ROAD:   R = 42;  G = 40;  B = 48;  break;  /* asphalt */
        case T_HOME:   R = 162; G = 122; B = 96;  break;  /* brick/terracotta */
        case T_SHOP:   R = 198; G = 162; B = 72;  break;  /* amber storefronts */
        case T_WORK:   R = 118; G = 124; B = 140; break;  /* steel/glass offices */
        case T_BAR:    R = 150; G = 110; B = 170; break;  /* neon violet */
        case T_CHURCH: R = 208; G = 192; B = 160; break;  /* pale stone */
        case T_POLICE: R = 70;  G = 100; B = 180; break;  /* blue */
        case T_PARK:   R = 64;  G = 112; B = 66;  break;  /* bright greenery */
        case T_WATER:  R = 46;  G = 86;  B = 128; break;  /* river/lake */
        default:       R = 40;  G = 58;  B = 46;  break;  /* grass/undeveloped */
    }
    *r = R; *g = G; *b = B;
}

int tile_is_building(TileType t) {
    switch (t) {
        case T_HOME: case T_SHOP: case T_WORK:
        case T_BAR:  case T_CHURCH: case T_POLICE: return 1;
        default: return 0;
    }
}

const LegendEntry LEGEND[] = {
    { T_HOME,   "Home"   }, { T_SHOP,   "Shop"   }, { T_WORK,  "Office" },
    { T_BAR,    "Bar"    }, { T_CHURCH, "Church" }, { T_POLICE,"Police" },
    { T_PARK,   "Park"   }, { T_WATER,  "Water"  },
};
const int LEGEND_N = (int)(sizeof(LEGEND)/sizeof(LEGEND[0]));

int tile_shade_jitter(int x, int y, int range) {
    unsigned h = (unsigned)(x*73856093) ^ (unsigned)(y*19349663);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (int)(h % (unsigned)(2*range+1)) - range;
}

void hud_string(const World *w, char *buf, int n, float speed, int paused,
                int fps, const char *backend) {
    int alive = 0, factions = 0;
    for (int i = 0; i < w->n_agents; i++) alive += w->agents[i].alive;
    for (int i = 0; i < w->n_factions; i++) factions += (w->factions[i].active && w->factions[i].members > 0);
    double demand = alive * FOOD_PER_CAPITA;
    double days_supply = demand > 0 ? w->econ.food_stock / demand : 0.0;
    snprintf(buf, (size_t)n,
             "Day %d  %02d:00  Pop %d  Deaths %d  Crimes %d  Wanted %d  Jail %d  Factions %d  "
             "Food %.0fd ($%.1fx)  GDP %.0f  [x%.0f%s]  %d FPS (%s)",
             w->day, (int)w->hour, alive, w->deaths, w->crimes,
             crime_wanted_count(w), crime_jailed_count(w), factions,
             days_supply, w->econ.food_price, w->econ.gdp_prev,
             speed, paused ? " PAUSED" : "", fps, backend);
}
