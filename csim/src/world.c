/* world.c — world generation, population, tick orchestration, save/load. */
#include "sim.h"
#include "llm.h"
#include "record.h"
#include "os_client.h"
#define FNL_IMPL                 /* generate the FastNoiseLite C implementation here (one TU) */
#if defined(__GNUC__)
#pragma GCC diagnostic push      /* silence a benign warning in the vendored cellular-noise tables */
#pragma GCC diagnostic ignored "-Waggressive-loop-optimizations"
#endif
#include "FastNoiseLite.h"       /* vendored POSIX C port (Auburn/FastNoiseLite, C API) */
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static const char *FIRST_NAMES[] = {
    "Alex","Anna","Boris","Vera","Victor","Grace","Dmitri","Eugene","Catherine",
    "Ivan","Igor","Irene","Conrad","Laura","Leo","Linda","Max","Marina","Natalie",
    "Nicholas","Oleg","Olga","Paul","Polina","Roman","Svetlana","Sergei","Tanya",
    "Yuri","Julia","Andrew","Arthur","Michael","Zoe","Stephen","Timothy","Rose",
    "Fred","Tamara","Lydia"};
static const char *LAST_NAMES[] = {
    "Ivanov","Petrov","Sidorov","Kuznetsov","Smirnov","Vasiliev","Popov","Sokolov",
    "Mikhailov","Novikov","Fedorov","Morozov","Volkov","Alexeev","Lebedev","Semenov",
    "Egorov","Pavlov","Kozlov","Stepanov","Nikolaev","Orlov","Andreev","Makarov",
    "Nikitin","Zakharov","Zaitsev","Soloviev"};

int world_is_night(const World *w) {
    int h = (int)w->hour;
    return h < DAYTIME_START || h >= NIGHT_START;
}
int world_alive(const World *w) {
    int c = 0;
    for (int i = 0; i < w->n_agents; i++) c += w->agents[i].alive;
    return c;
}

/* ── Generation ──────────────────────────────────────────────────────────── */
static unsigned char occ_for(World *w, Agent *a);
static void culture_assign(World *w, Agent *a);

static void place_building(World *w, int x, int y, TileType t, int cap) {
    if (w->n_buildings >= MAX_BUILDINGS) return;
    Building *b = &w->buildings[w->n_buildings];
    b->id = w->n_buildings; b->type = t; b->x = x; b->y = y; b->w = 1; b->h = 1;
    b->capacity = cap; b->n_residents = 0; b->n_workers = 0;
    b->owner_id = -1;
    w->tile[x][y] = (uint8_t)t;
    w->n_buildings++;
}

static int default_cap(TileType t) {
    switch (t) { case T_HOME: return 4; case T_SHOP: return 8; case T_WORK: return 10;
                 case T_BAR: return 6; case T_CHURCH: return 20; case T_POLICE: return 6;
                 default: return 4; }
}

/* Pick a building type for a lot given its normalised distance from downtown
 * (0 = city centre, 1 = edge). Gives a real city gradient: a dense office/retail
 * core, mixed commercial inner ring, residential belt, and a sparse rural fringe. */
static TileType district_building(Rng *r, double d) {
    double roll = rng_double(r);
    if (d < 0.20)                 /* downtown: offices + retail, some nightlife */
        return roll<0.55?T_WORK : roll<0.85?T_SHOP : roll<0.95?T_BAR : T_HOME;
    if (d < 0.38)                 /* inner ring: mixed commercial */
        return roll<0.35?T_SHOP : roll<0.55?T_WORK : roll<0.68?T_BAR
             : roll<0.73?T_CHURCH : T_HOME;
    if (d < 0.62)                 /* residential belt */
        return roll<0.78?T_HOME : roll<0.88?T_SHOP : roll<0.93?T_CHURCH
             : roll<0.97?T_WORK : T_BAR;
    return roll<0.88?T_HOME : roll<0.95?T_SHOP : T_CHURCH;  /* outskirts */
}

/* Probability a buildable lot is actually built, by district — leaves yards,
 * green space and vacant lots so blocks don't read as solid colour. */
static double district_fill(double d) {
    if (d < 0.20) return 0.92; if (d < 0.38) return 0.80;
    if (d < 0.62) return 0.66; if (d < 0.82) return 0.42; return 0.18;
}

/* ── Connectivity (so water-blocking pathfinding doesn't strand agents) ────────
   Flood-fill walkable tiles into connected components; essential targets (shops,
   workplaces, homes) are then chosen within the agent's own component, so they're
   always reachable without crossing water. Derived from tiles, recomputed at
   world_init and after load. */
static int g_comp[WORLD_W * WORLD_H];
static void compute_components(World *w) {
    static int queue[WORLD_W * WORLD_H];
    for (int i = 0; i < WORLD_W * WORLD_H; i++) g_comp[i] = -1;
    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
    int comp = 0;
    for (int sy = 0; sy < WORLD_H; sy++) for (int sx = 0; sx < WORLD_W; sx++) {
        int si = sy*WORLD_W + sx;
        if (g_comp[si] != -1 || !tile_walkable(w, sx, sy)) continue;
        int head = 0, tail = 0; queue[tail++] = si; g_comp[si] = comp;
        while (head < tail) {
            int c = queue[head++], cx = c % WORLD_W, cy = c / WORLD_W;
            for (int d = 0; d < 4; d++) {
                int nx = cx + DX[d], ny = cy + DY[d];
                if (nx < 0 || nx >= WORLD_W || ny < 0 || ny >= WORLD_H) continue;
                int ni = ny*WORLD_W + nx;
                if (g_comp[ni] == -1 && tile_walkable(w, nx, ny)) { g_comp[ni] = comp; queue[tail++] = ni; }
            }
        }
        comp++;
    }
}
static int comp_at(int x, int y) {
    if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) return -1;
    return g_comp[y*WORLD_W + x];
}

/* Per-tile "nearest service by walking distance" fields (flow fields): a
   multi-source BFS from every building of a type fills each walkable tile with the
   id of the closest such building *by path*. So agents always head to the genuinely
   nearest shop/bar/church, not one a river-detour away — the main cure for
   distance-related deaths once water blocks movement. */
enum { SVC_SHOP, SVC_BAR, SVC_CHURCH, SVC_COUNT };
static int g_svc[SVC_COUNT][WORLD_W * WORLD_H];
static const TileType SVC_TYPE[SVC_COUNT] = { T_SHOP, T_BAR, T_CHURCH };
static void compute_services(World *w) {
    static int queue[WORLD_W * WORLD_H];
    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
    for (int s = 0; s < SVC_COUNT; s++) {
        int *near = g_svc[s];
        for (int i = 0; i < WORLD_W * WORLD_H; i++) near[i] = -1;
        int head = 0, tail = 0;
        for (int b = 0; b < w->n_buildings; b++) {       /* seed from every such building */
            if (w->buildings[b].type != SVC_TYPE[s]) continue;
            int bi = w->buildings[b].y * WORLD_W + w->buildings[b].x;
            if (near[bi] == -1) { near[bi] = b; queue[tail++] = bi; }
        }
        while (head < tail) {
            int c = queue[head++], cx = c % WORLD_W, cy = c / WORLD_W;
            for (int d = 0; d < 4; d++) {
                int nx = cx + DX[d], ny = cy + DY[d];
                if (nx < 0 || nx >= WORLD_W || ny < 0 || ny >= WORLD_H) continue;
                int ni = ny*WORLD_W + nx;
                if (near[ni] == -1 && tile_walkable(w, nx, ny)) { near[ni] = near[c]; queue[tail++] = ni; }
            }
        }
    }
}
/* the nearest-by-path building id of a service type for an agent at (fx,fy), or -1 */
static int nearest_service(int fx, int fy, int svc) {
    if (fx < 0 || fx >= WORLD_W || fy < 0 || fy >= WORLD_H) return -1;
    return g_svc[svc][fy*WORLD_W + fx];
}
/* nearest building of `type` in the same component as (fx,fy); falls back to the
   nearest of any component so a caller always gets something. */
static int building_nearest_reachable(const World *w, int fx, int fy, TileType type) {
    int myc = comp_at(fx, fy);
    int best = -1, bestd = 0, any = -1, anyd = 0;
    for (int i = 0; i < w->n_buildings; i++) {
        if (w->buildings[i].type != type) continue;
        int bx = w->buildings[i].x, by = w->buildings[i].y;
        int d = abs(bx - fx) + abs(by - fy);
        if (any < 0 || d < anyd) { any = i; anyd = d; }
        if (comp_at(bx, by) == myc && (best < 0 || d < bestd)) { best = i; bestd = d; }
    }
    return best >= 0 ? best : any;
}

void world_init(World *w, uint64_t seed) {
    memset(w, 0, sizeof(*w));
    rng_seed(&w->rng, seed, 0xCAFEu);
    Rng *r = &w->rng;
    const double cx = WORLD_W * 0.5, cy = WORLD_H * 0.5;
    const double maxd = sqrt(cx*cx + cy*cy);

    /* Optional coherent-noise worldgen (FastNoiseLite C port). Seeded from `seed`,
       kept out of w->rng so it's deterministic and the default path is unchanged. */
    int noise = get_noise_worldgen();
    fnl_state n_water, n_dens, n_park;
    if (noise) {
        n_water = fnlCreateState(); n_water.seed = (int)seed;        n_water.noise_type = FNL_NOISE_OPENSIMPLEX2; n_water.frequency = 0.035f;
        n_dens  = fnlCreateState(); n_dens.seed  = (int)seed + 1337; n_dens.noise_type  = FNL_NOISE_OPENSIMPLEX2; n_dens.frequency  = 0.060f;
        n_park  = fnlCreateState(); n_park.seed  = (int)seed + 7777; n_park.noise_type  = FNL_NOISE_OPENSIMPLEX2; n_park.frequency  = 0.090f;
    }

    /* 1. Base layer: grass + water. Default = a meandering east-side river;
     *    noise mode = organic lakes/rivers where an elevation field dips below sea level. */
    for (int y = 0; y < WORLD_H; y++) {
        double rc = WORLD_W*0.72 + 10.0*sin(y*0.10) + 5.0*sin(y*0.31);
        for (int x = 0; x < WORLD_W; x++) {
            if (noise) {
                float e = fnlGetNoise2D(&n_water, (float)x, (float)y);   /* -1..1 */
                w->tile[x][y] = (e < -0.62f) ? T_WATER : T_GRASS;        /* only the lowest land floods, so the city stays connected */
            } else {
                w->tile[x][y] = (fabs(x - rc) < 1.6) ? T_WATER : T_GRASS;
            }
        }
    }

    /* 2. Street grid: minor streets every 7/6 tiles, major avenues every 21/18.
     *    Streets stop at the river banks; major avenues bridge across it. */
    for (int x = 0; x < WORLD_W; x++)
        for (int y = 0; y < WORLD_H; y++) {
            int major = (x % 21 == 0) || (y % 18 == 0);
            int minor = (x % 7  == 0) || (y % 6  == 0);
            if (w->tile[x][y] == T_WATER) { if (major) w->tile[x][y] = T_ROAD; continue; }
            if (major || minor) w->tile[x][y] = T_ROAD;
        }

    /* 3. Parks. Default = a handful of random blocks; noise mode = organic greens
     *    wherever a park field peaks. */
    if (noise) {
        for (int x = 0; x < WORLD_W; x++)
            for (int y = 0; y < WORLD_H; y++)
                if (w->tile[x][y] == T_GRASS && fnlGetNoise2D(&n_park, (float)x, (float)y) > 0.58f)
                    w->tile[x][y] = T_PARK;
    } else {
        for (int p = 0; p < 6; p++) {
            int px = 3 + rng_int(r, WORLD_W - 11), py = 3 + rng_int(r, WORLD_H - 9);
            int pw = 4 + rng_int(r, 4), ph = 3 + rng_int(r, 3);
            for (int x = px; x < px+pw && x < WORLD_W; x++)
                for (int y = py; y < py+ph && y < WORLD_H; y++)
                    if (w->tile[x][y] == T_GRASS) w->tile[x][y] = T_PARK;
        }
    }

    /* 4. Fill lots with buildings. Default = a smooth downtown-to-fringe density
     *    gradient; noise mode = organic neighborhoods clustered where a density
     *    field is high (with a gentle pull toward the centre). */
    for (int x = 0; x < WORLD_W; x++)
        for (int y = 0; y < WORLD_H; y++) {
            if (w->tile[x][y] != T_GRASS) continue;
            if (w->n_buildings >= MAX_BUILDINGS) goto built;
            double dx = x - cx, dy = y - cy, d = sqrt(dx*dx + dy*dy) / maxd;
            double fill; double dtype = d;
            if (noise) {
                double n01 = fnlGetNoise2D(&n_dens, (float)x, (float)y) * 0.5 + 0.5;  /* 0..1 */
                fill  = n01 * (1.0 - 0.5 * d);                 /* dense where noise high + near centre */
                dtype = clampd(d - (n01 - 0.5) * 0.5, 0, 1);   /* high-density pockets read as "downtown" */
            } else {
                fill = district_fill(d);
            }
            if (rng_double(r) > fill) continue;   /* leave as yard/grass */
            TileType t = district_building(r, dtype);
            place_building(w, x, y, t, default_cap(t));
        }
built:;

    /* 5. Spread a few police stations across the quadrants (on open lots). */
    for (int p = 0; p < 4 && w->n_buildings < MAX_BUILDINGS; p++) {
        int tx = (p & 1) ? WORLD_W*3/4 : WORLD_W/4;
        int ty = (p < 2) ? WORLD_H/4   : WORLD_H*3/4;
        for (int rad = 0; rad < 10; rad++) {        /* spiral out to an open lot */
            int ox = tx + rng_int(r, 2*rad+1) - rad, oy = ty + rng_int(r, 2*rad+1) - rad;
            if (ox<0||ox>=WORLD_W||oy<0||oy>=WORLD_H) continue;
            if (w->tile[ox][oy]==T_GRASS || w->tile[ox][oy]==T_PARK) {
                place_building(w, ox, oy, T_POLICE, default_cap(T_POLICE)); break;
            }
        }
    }

    compute_components(w);   /* for reachability-aware targeting (pathing avoids water) */
    compute_services(w);     /* per-tile nearest shop/bar/church by walking distance */
    w->hour = 8.0; w->day = 1;
    w->econ.goods_price = 1.0; w->econ.wage_mult = 1.0;
    factions_seed(w);
}

static int random_building(World *w, TileType t) {
    int ids[MAX_BUILDINGS], n = 0;
    for (int i = 0; i < w->n_buildings; i++) if (w->buildings[i].type == t) ids[n++] = i;
    return n ? ids[rng_int(&w->rng, n)] : -1;
}

void world_populate(World *w, int n) {
    if (n > MAX_AGENTS) n = MAX_AGENTS;
    Rng *r = &w->rng;
    for (int k = 0; k < n; k++) {
        Agent *a = &w->agents[w->n_agents];
        memset(a, 0, sizeof(*a));
        a->id = w->next_id++;
        a->alive = 1;
        snprintf(a->name, sizeof(a->name), "%s %s",
                 FIRST_NAMES[rng_int(r, 40)], LAST_NAMES[rng_int(r, 28)]);
        a->age = (int)clampd(rng_gauss(r, 35, 14), 16, 90);
        int home = random_building(w, T_HOME);
        a->home_id = home;
        if (home >= 0) { a->x = w->buildings[home].x; a->y = w->buildings[home].y; }
        else { a->x = rng_int(r, WORLD_W); a->y = rng_int(r, WORLD_H); }
        a->tx = -1; a->ty = -1;
        /* workplace: weighted so police are a small minority (~3%); reachable from home */
        double wr = rng_double(r);
        TileType wtype = wr < 0.55 ? T_WORK : wr < 0.80 ? T_SHOP
                       : wr < 0.90 ? T_BAR  : wr < 0.97 ? T_CHURCH : T_POLICE;
        a->workplace_id = building_nearest_reachable(w, (int)a->x, (int)a->y, wtype);
        if (a->workplace_id < 0) a->workplace_id = building_nearest_reachable(w, (int)a->x, (int)a->y, T_WORK);
        if (a->workplace_id >= 0 && w->buildings[a->workplace_id].type == T_POLICE)
            a->is_police = 1;
        a->faction_id = -1;
        a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
        a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
        a->needs.money = clampd(rng_gauss(r, START_MONEY_MEAN, START_MONEY_SD), 0, 1e9);
        personality_random(&a->pers, r);
        a->action = A_WANDER; a->faction_id = -1;
        a->wanted_for[0] = a->jailed_for[0] = '\0';
        a->sex = (unsigned char)rng_int(r, 2);
        a->spouse_id = a->mother_id = a->father_id = -1;
        a->n_children = 0; a->pregnant_ticks = 0;
        w->n_agents++;
    }
    assign_crime_roles(w);   /* career criminals, dealers, kingpins, users, a rare killer */
    factions_populate(w);    /* enlist members into gangs/cults so factions are real actors */
    economy_setup(w);        /* occupations, landlords/tenants */
    culture_setup(w);        /* faith, cultural group, language, schooling */
    households_daily(w);     /* seed per-home aggregates so class/metrics work from day 0 */
}

Agent *world_agent_by_id(World *w, int id) {
    for (int i = 0; i < w->n_agents; i++) if (w->agents[i].id == id) return &w->agents[i];
    return NULL;
}

/* Get a slot for a new agent: reuse a dead one (turnover reclaims the array), or
   append. Returns NULL only when truly full. All refs are by id, so reuse is safe. */
static Agent *alloc_agent(World *w) {
    for (int i = 0; i < w->n_agents; i++) if (!w->agents[i].alive) return &w->agents[i];
    if (w->n_agents >= MAX_AGENTS) return NULL;
    return &w->agents[w->n_agents++];
}

/* Spawn + register one new citizen at (tx,ty) (God Mode). Returns id or -1. */
int world_spawn_agent(World *w, int tx, int ty) {
    Rng *r = &w->rng;
    Agent *a = alloc_agent(w);
    if (!a) return -1;
    memset(a, 0, sizeof(*a));
    a->id = w->next_id++;
    a->alive = 1;
    snprintf(a->name, sizeof(a->name), "%s %s", FIRST_NAMES[rng_int(r, 40)], LAST_NAMES[rng_int(r, 28)]);
    /* newcomers skew young — young adults with most of their fertile life ahead,
       so the city replenishes through births, not just arrivals */
    a->age = (int)clampd(rng_gauss(r, 26, 7), 18, 55);
    a->home_id = random_building(w, T_HOME);
    /* spawn at home when possible (god-mode spawns honor tx,ty) so home is reachable */
    if (tx >= 0 && tx < WORLD_W && ty >= 0 && ty < WORLD_H) { a->x = tx; a->y = ty; }
    else if (a->home_id >= 0) { a->x = w->buildings[a->home_id].x; a->y = w->buildings[a->home_id].y; }
    else { a->x = rng_int(r, WORLD_W); a->y = rng_int(r, WORLD_H); }
    a->tx = -1; a->ty = -1;
    double wr = rng_double(r);
    TileType wtype = wr < 0.6 ? T_WORK : wr < 0.85 ? T_SHOP : wr < 0.95 ? T_BAR : T_CHURCH;
    a->workplace_id = building_nearest_reachable(w, (int)a->x, (int)a->y, wtype);
    a->faction_id = -1;
    a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
    a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
    a->needs.money = clampd(rng_gauss(r, START_MONEY_MEAN, START_MONEY_SD), 0, 1e9);
    personality_random(&a->pers, r);
    a->action = A_WANDER;
    a->sex = (unsigned char)rng_int(r, 2);
    a->spouse_id = a->mother_id = a->father_id = -1;
    a->n_children = 0; a->pregnant_ticks = 0;
    if (a->workplace_id >= 0 && w->buildings[a->workplace_id].type == T_POLICE) a->is_police = 1;
    a->occupation = occ_for(w, a);
    a->debt = 0.0;
    culture_assign(w, a);
    /* newcomers bring diversity: often a foreign tongue they'll assimilate over time */
    if (rng_double(r) < 0.6) { a->culture = CUL_NEWCOMER; a->language = (unsigned char)rng_int_incl(r, 1, LANG_COUNT - 1); }
    char t[96]; snprintf(t, sizeof(t), "%s appeared in the city", a->name);
    events_post(w, EV_BIRTH, a->id, -1, a->x, a->y, 0.4, t);
    return a->id;
}

/* ── kinship & births ───────────────────────────────────────────────────────
   The city marries fond, familiar, eligible couples and raises children from
   them.  Called once per game-day from world_tick. */

/* Append a newborn carrying the father's surname and the family's home. */
static int spawn_child(World *w, Agent *mum, Agent *dad) {
    Rng *r = &w->rng;
    Agent *a = alloc_agent(w);
    if (!a) return -1;
    memset(a, 0, sizeof(*a));
    a->id = w->next_id++;
    a->alive = 1;
    /* given name + father's surname (fallback to mother's) */
    const char *sur = strrchr(dad->name, ' ');
    if (!sur) sur = strrchr(mum->name, ' ');
    snprintf(a->name, sizeof(a->name), "%s%s", FIRST_NAMES[rng_int(r, 40)],
             sur ? sur : " Doe");
    a->age = 0;
    a->sex = (unsigned char)rng_int(r, 2);
    a->home_id = mum->home_id >= 0 ? mum->home_id : dad->home_id;
    if (a->home_id >= 0) { a->x = w->buildings[a->home_id].x; a->y = w->buildings[a->home_id].y; }
    else { a->x = mum->x; a->y = mum->y; }
    a->tx = -1; a->ty = -1;
    a->workplace_id = -1;                 /* children don't work yet */
    a->faction_id = -1;
    a->mother_id = mum->id; a->father_id = dad->id;
    a->spouse_id = -1;
    a->dealer_id = -1;
    a->status = 2;
    /* children inherit the family's culture, tongue and faith; schooling starts at 0 */
    a->culture = mum->culture;
    a->language = mum->language;
    a->faith = mum->faith != FAITH_NONE ? mum->faith : dad->faith;
    a->education = 0.0f;
    a->intellect = (float)clampd((mum->intellect + dad->intellect) * 0.5 + rng_gauss(r, 0, 0.1), 0.05, 1.0);
    a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
    a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
    a->needs.money = 0;
    personality_random(&a->pers, r);
    a->action = A_WANDER;
    char t[112]; snprintf(t, sizeof(t), "%.20s and %.20s had a child, %.24s",
                          mum->name, dad->name, a->name);
    events_post(w, EV_BIRTH, a->id, mum->id, a->x, a->y, 0.7, t);
    return a->id;
}

/* Immigrate a whole young family — a married couple plus 1-3 children — so the
   city's age pyramid fills from the bottom (native marriage can't keep pace with
   compressed aging). Returns the number of people added. */
int world_spawn_family(World *w) {
    Rng *r = &w->rng;
    int da = world_spawn_agent(w, -1, -1); if (da < 0) return 0;
    int ma = world_spawn_agent(w, -1, -1); if (ma < 0) return 1;
    Agent *dad = world_agent_by_id(w, da), *mum = world_agent_by_id(w, ma);
    if (!dad || !mum) return 2;
    dad->sex = 1; mum->sex = 0;
    dad->age = (int)clampd(rng_gauss(r, 32, 5), 22, 45);
    mum->age = (int)clampd(rng_gauss(r, 30, 5), 20, 42);
    dad->spouse_id = mum->id; mum->spouse_id = dad->id;
    if (dad->home_id >= 0) mum->home_id = dad->home_id;   /* one household */
    mum->culture = dad->culture; mum->language = dad->language;
    if (dad->faith != FAITH_NONE) mum->faith = dad->faith;
    int count = 2;
    int nkids = rng_int_incl(r, get_family_kids_min(), get_family_kids_max());
    for (int k = 0; k < nkids; k++) {
        int cid = spawn_child(w, mum, dad);
        if (cid < 0) break;
        Agent *c = world_agent_by_id(w, cid);
        if (c) { c->age = rng_int_incl(r, 0, 12); mum->n_children++; dad->n_children++; }
        count++;
    }
    return count;
}

void kinship_daily(World *w) {
    Rng *r = &w->rng;
    int n0 = w->n_agents;   /* freeze: newborns this tick are not courted/born again */

    /* ── courtship -> marriage ── */
    for (int i = 0; i < n0; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->spouse_id >= 0 || a->age < MARRY_MIN_AGE) continue;
        if (a->arrested_ticks > 0) continue;
        /* pick the fondest, most familiar eligible opposite-sex partner */
        int best = -1; double bestaff = MARRY_AFFINITY;
        for (int k = 0; k < a->rels.n; k++) {
            Relation *rl = &a->rels.rel[k];
            if (rl->affinity < bestaff || rl->familiarity < MARRY_FAMILIAR) continue;
            Agent *o = world_agent_by_id(w, rl->other_id);
            if (!o || !o->alive || o->spouse_id >= 0) continue;
            if (o->age < MARRY_MIN_AGE || o->sex == a->sex) continue;
            if (o->arrested_ticks > 0) continue;
            best = rl->other_id; bestaff = rl->affinity;
        }
        if (best < 0) continue;
        if (rng_double(r) >= MARRY_CHANCE) continue;
        Agent *o = world_agent_by_id(w, best);
        a->spouse_id = o->id; o->spouse_id = a->id;
        /* the pair settles into one home and warms to each other */
        if (a->home_id >= 0) o->home_id = a->home_id;
        else if (o->home_id >= 0) a->home_id = o->home_id;
        rel_adjust(&a->rels, o->id, 0.2); rel_adjust(&o->rels, a->id, 0.2);
        a->reputation += 0.03f; o->reputation += 0.03f;
        char t[112]; snprintf(t, sizeof(t), "%.24s and %.24s got married", a->name, o->name);
        events_post(w, EV_MARRIAGE, a->id, o->id, (int)a->x, (int)a->y, 0.75, t);
    }

    /* ── conception & birth (mother carries the pregnancy) ── */
    for (int i = 0; i < n0; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->sex != 0) continue;         /* mothers only */
        if (a->pregnant_ticks > 0) {                     /* advance an existing pregnancy */
            if (--a->pregnant_ticks == 0) {
                Agent *dad = world_agent_by_id(w, a->spouse_id);
                if (dad && dad->alive) {
                    int cid = spawn_child(w, a, dad);
                    if (cid >= 0) { a->n_children++; dad->n_children++; }
                }
            }
            continue;
        }
        if (a->spouse_id < 0 || a->age > FERTILE_MAX_AGE || a->n_children >= MAX_CHILDREN) continue;
        Agent *dad = world_agent_by_id(w, a->spouse_id);
        if (!dad || !dad->alive) continue;
        if (rng_double(r) < CONCEIVE_CHANCE) a->pregnant_ticks = GESTATION_DAYS;
    }
}

/* ── economy: occupations, land ownership, the food larder ───────────────────── */

const char *occupation_name(unsigned char occ) {
    static const char *n[OCC_COUNT] = {
        "Idle","Laborer","Shopkeeper","Barkeep","Clergy","Officer" };
    return occ < OCC_COUNT ? n[occ] : "?";
}

static unsigned char occ_for(World *w, Agent *a) {
    if (a->is_police) return OCC_OFFICER;
    if (a->workplace_id < 0) return OCC_NONE;
    switch (w->buildings[a->workplace_id].type) {
        case T_SHOP:   return OCC_SHOPKEEP;
        case T_BAR:    return OCC_BARKEEP;
        case T_CHURCH: return OCC_CLERGY;
        case T_WORK:   return OCC_LABORER;
        default:       return OCC_LABORER;
    }
}

int count_properties(const World *w, int owner_id) {
    int c = 0;
    for (int i = 0; i < w->n_buildings; i++)
        if (w->buildings[i].type == T_HOME && w->buildings[i].owner_id == owner_id) c++;
    return c;
}

/* ── Neighborhoods: home value + residential sorting (Phase 2, --neighborhoods) ── */
/* desirability 0..1 of a home: downtown-central + near parks (crime penalty is
 * applied later as heat builds, via households_daily). */
static float home_value(const World *w, int bx, int by) {
    const double cx = WORLD_W * 0.5, cy = WORLD_H * 0.5;
    const double maxd = sqrt(cx*cx + cy*cy);
    double d = sqrt((bx-cx)*(bx-cx) + (by-cy)*(by-cy)) / maxd;   /* 0 centre .. 1 edge */
    double v = 0.15 + 0.70 * (1.0 - d);
    int parks = 0, R = 4;
    for (int yy = by-R; yy <= by+R; yy++) for (int xx = bx-R; xx <= bx+R; xx++) {
        if (xx < 0 || yy < 0 || xx >= WORLD_W || yy >= WORLD_H) continue;
        if (w->tile[xx][yy] == T_PARK) parks++;
    }
    v += 0.02 * parks;
    return (float)(v < 0 ? 0 : v > 1 ? 1 : v);
}

typedef struct { double key; int idx; } RankD;   /* sort helper (money or value) */
static int rankd_desc(const void *a, const void *b) {
    const RankD *x = a, *y = b;
    if (x->key < y->key) return 1;
    if (x->key > y->key) return -1;
    return x->idx - y->idx;   /* stable tiebreak for determinism */
}

/* Sort residency by affordability: rank households (here, individuals at spawn) by
 * wealth and homes by value, then seat the richest in the most desirable homes →
 * rich/poor quarters emerge instead of a random mix. Also stamps Building.value. */
static void residential_sort(World *w) {
    static RankD homes[MAX_BUILDINGS]; int nh = 0;
    for (int b = 0; b < w->n_buildings; b++) if (w->buildings[b].type == T_HOME) {
        w->buildings[b].value = home_value(w, w->buildings[b].x, w->buildings[b].y);
        homes[nh].key = w->buildings[b].value; homes[nh].idx = b; nh++;
    }
    if (nh == 0) return;
    qsort(homes, nh, sizeof(RankD), rankd_desc);   /* most desirable first */

    static RankD ags[MAX_AGENTS]; int na = 0;
    for (int i = 0; i < w->n_agents; i++) if (w->agents[i].alive) {
        ags[na].key = w->agents[i].needs.money; ags[na].idx = i; na++;
    }
    qsort(ags, na, sizeof(RankD), rankd_desc);      /* richest first */

    for (int b = 0; b < w->n_buildings; b++) if (w->buildings[b].type == T_HOME) w->buildings[b].n_residents = 0;
    int hi = 0;
    for (int k = 0; k < na; k++) {
        Agent *a = &w->agents[ags[k].idx];
        while (hi < nh) { int b = homes[hi].idx;
            int cap = w->buildings[b].capacity > 0 ? w->buildings[b].capacity : 4;
            if (w->buildings[b].n_residents < cap) break; hi++; }
        int b = (hi < nh) ? homes[hi].idx : homes[nh-1].idx;   /* overflow → least desirable */
        a->home_id = b;
        a->x = w->buildings[b].x; a->y = w->buildings[b].y;
        w->buildings[b].n_residents++;
    }
}

void economy_setup(World *w) {
    Rng *r = &w->rng;
    for (int i = 0; i < w->n_agents; i++)
        if (w->agents[i].alive) w->agents[i].occupation = occ_for(w, &w->agents[i]);

    if (get_neighborhoods()) residential_sort(w);   /* seat households by wealth into valued homes */

    /* Land ownership concentrates in a wealthy minority: the richest ~12% of the
       city become the landlord class, and every home is deeded to one of them.
       Most citizens are therefore tenants who pay rent to a landlord; a landlord
       lives rent-free in a home they own and collects from everyone else's. */
    int na = w->n_agents;
    int order[MAX_AGENTS]; int no = 0;
    for (int i = 0; i < na; i++) if (w->agents[i].alive) order[no++] = i;
    /* selection sort the top slice by money (descending) — plenty fast at this n */
    int n_landlords = no / 8; if (n_landlords < 3) n_landlords = 3; if (n_landlords > no) n_landlords = no;
    for (int s = 0; s < n_landlords; s++) {
        int best = s;
        for (int t = s + 1; t < no; t++)
            if (w->agents[order[t]].needs.money > w->agents[order[best]].needs.money) best = t;
        int tmp = order[s]; order[s] = order[best]; order[best] = tmp;
    }
    for (int b = 0; b < w->n_buildings; b++) {
        if (w->buildings[b].type != T_HOME) continue;
        /* prefer a landlord who already lives here, else deed it to a random landlord */
        int owner = -1;
        for (int s = 0; s < n_landlords; s++)
            if (w->agents[order[s]].home_id == b) { owner = w->agents[order[s]].id; break; }
        if (owner < 0) owner = w->agents[order[rng_int(r, n_landlords)]].id;
        w->buildings[b].owner_id = owner;
    }

    w->econ.goods_price = 1.0; w->econ.wage_mult = 1.0;
}

/* ── culture: religion, cultural group, language, schooling ───────────────────── */

const char *faith_name(unsigned char f) {
    static const char *n[FAITH_COUNT] = { "Secular","Orthodox","Reformed","Old Faith","Mystic" };
    return f < FAITH_COUNT ? n[f] : "?";
}
const char *culture_name(unsigned char c) {
    static const char *n[CUL_COUNT] = { "Harborfolk","Hill Clans","Old-town","Newcomers" };
    return c < CUL_COUNT ? n[c] : "?";
}
const char *language_name(unsigned char l) {
    static const char *n[LANG_COUNT] = { "Common","High Tongue","Coastal","Old Speech" };
    return l < LANG_COUNT ? n[l] : "?";
}

/* each culture has a native tongue and a leaning faith */
static unsigned char culture_tongue(unsigned char c) {
    switch (c) { case CUL_HARBOR: return LANG_COASTAL; case CUL_HILL: return LANG_HIGH;
                 case CUL_OLDTOWN: return LANG_OLD;    default: return LANG_COMMON; }
}
static unsigned char culture_faith(unsigned char c) {
    switch (c) { case CUL_HARBOR: return FAITH_REFORMED; case CUL_HILL: return FAITH_ORTHODOX;
                 case CUL_OLDTOWN: return FAITH_OLD;     default: return FAITH_MYSTIC; }
}

/* assign one agent's cultural attributes (shared by populate, spawn, birth fallback) */
static void culture_assign(World *w, Agent *a) {
    Rng *r = &w->rng;
    double roll = rng_double(r);
    a->culture = roll < 0.40 ? CUL_HARBOR : roll < 0.70 ? CUL_OLDTOWN
               : roll < 0.90 ? CUL_HILL : CUL_NEWCOMER;
    /* the Common tongue is the city's lingua franca; many speak it over their native one */
    a->language = rng_double(r) < 0.55 ? LANG_COMMON : culture_tongue(a->culture);
    /* the devout take their culture's faith; the rest are secular */
    a->faith = pers_faith(&a->pers) > 0.55 && rng_double(r) < 0.8
             ? culture_faith(a->culture) : FAITH_NONE;
    a->education = (float)clampd(rng_gauss(r, 0.5, 0.2), 0.0, 1.0);
    a->intellect = (float)clampd(rng_gauss(r, 0.5, 0.18), 0.05, 1.0);   /* innate research aptitude */
    a->craft = (float)clampd(rng_gauss(r, 0.35, 0.18), 0.0, 1.0);       /* some prior trade experience */
}

void culture_setup(World *w) {
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        culture_assign(w, a);
        /* cult members share the Mystic faith (the cult is their religion) */
        if (a->faction_id >= 0 && w->factions[a->faction_id].is_cult) a->faith = FAITH_MYSTIC;
    }
}

/* diffusion on day change: schooling of the young, religious conversion through
   friendship, and assimilation into the Common tongue. */
void culture_daily(World *w) {
    Rng *r = &w->rng;
    int n0 = w->n_agents;
    /* Writing (tech) speeds literacy; apprenticeship pulls adults toward the educated norm */
    double writing = w->sci.discovered[TECH_WRITING] ? w->sci.adoption[TECH_WRITING] : 0.0;
    double avgedu = 0; int na = 0;
    for (int i = 0; i < n0; i++) if (w->agents[i].alive && w->agents[i].age >= EDU_ADULT_AGE) { avgedu += w->agents[i].education; na++; }
    avgedu = na ? avgedu / na : 0.0;

    for (int i = 0; i < n0; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

        /* schooling: the young grow literate (per childhood-year, so the pace is
           independent of years-per-day; faster where there's writing) */
        if (a->age < EDU_ADULT_AGE && a->education < 1.0f)
            a->education = (float)clampd(a->education + EDU_PER_YEAR * get_years_per_day() * (1.0 + writing), 0, 1);
        /* apprenticeship: adults learn on the job toward the city's educated norm */
        else if (a->age >= EDU_ADULT_AGE && a->education < avgedu)
            a->education = (float)clampd(a->education + 0.03 * (avgedu - a->education) * a->intellect * (0.6 + writing), 0, 1);

        /* conversion: a searching soul takes up a devout friend's faith */
        if (a->needs.meaning < 0.4 && rng_double(r) < CONVERT_CHANCE) {
            for (int k = 0; k < a->rels.n; k++) {
                if (a->rels.rel[k].affinity < FRIENDSHIP_AFFINITY) continue;
                Agent *f = world_agent_by_id(w, a->rels.rel[k].other_id);
                if (!f || !f->alive || f->faith == FAITH_NONE || f->faith == a->faith) continue;
                a->faith = f->faith;
                a->needs.meaning = clampd(a->needs.meaning + 0.25, 0, 1);
                char t[96]; snprintf(t, sizeof(t), "%.26s converted to the %s faith", a->name, faith_name(a->faith));
                events_post(w, EV_FACTION, a->id, f->id, (int)a->x, (int)a->y, 0.35, t);
                break;
            }
        }

        /* assimilation: pick up the Common tongue over time (schooling speeds it) */
        if (a->language != LANG_COMMON && rng_double(r) < ASSIMILATE_CHANCE * (0.5 + a->education))
            a->language = LANG_COMMON;
    }
}

/* Old-age mortality (on day change): death risk climbs from AGE_MORTALITY, is
   near-certain by AGE_MAXLIFE. Scaled so most die in their 70s-80s. */
void lifecycle_daily(World *w) {
    double ypd = get_years_per_day();
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->age < AGE_MORTALITY) continue;
        double over = (double)(a->age - AGE_MORTALITY) / (double)(AGE_MAXLIFE - AGE_MORTALITY);
        if (over < 0) over = 0;
        double p_year = over * over;                 /* 0 at 60 -> 1 at 100 */
        double p_day = p_year * ypd;                 /* convert annual risk to this game-day */
        if (p_day > 0.9) p_day = 0.9;
        if (a->age >= AGE_MAXLIFE || rng_double(&w->rng) < p_day) {
            a->alive = 0; w->deaths++;
            if (a->spouse_id >= 0) { Agent *sp = world_agent_by_id(w, a->spouse_id);
                if (sp) sp->spouse_id = -1; a->spouse_id = -1; a->pregnant_ticks = 0; }
            char t[96]; snprintf(t, sizeof(t), "%.30s died of old age (%d)", a->name, a->age);
            events_post(w, EV_DEATH, a->id, -1, (int)a->x, (int)a->y, 0.7, t);
        }
    }
}

Agent *world_agent_at(World *w, int tx, int ty, double radius) {
    Agent *best = NULL; double bd = radius * radius;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        double d = (a->x - tx) * (a->x - tx) + (a->y - ty) * (a->y - ty);
        if (d <= bd) { bd = d; best = a; }
    }
    return best;
}

/* ── Per-agent helpers ───────────────────────────────────────────────────── */
/* crime seeking: head toward the right other agent so the trade/violence connects */
static Agent *seek_customer(World *w, Agent *d) {
    Agent *best = NULL; double bd = 1e18; int vis = get_vision();
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->id == d->id || o->is_police || o->arrested_ticks > 0) continue;
        if (o->crime_role == CR_DEALER || o->crime_role == CR_KINGPIN) continue;
        if (vis && !agent_can_see(w, d, (int)o->x, (int)o->y)) continue;   /* pursue a customer in sight */
        double dx = o->x - d->x, dy = o->y - d->y;
        double dd = dx*dx + dy*dy + (o->addiction > 0.1f ? 0.0 : 2000.0);  /* prefer addicts */
        if (dd < bd) { bd = dd; best = o; }
    }
    return best;
}
static Agent *seek_kingpin(World *w, Agent *d) {
    Agent *best = NULL; double bd = 1e18;
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->crime_role != CR_KINGPIN || o->drug_stock <= 0 || o->arrested_ticks > 0) continue;
        double dx = o->x - d->x, dy = o->y - d->y, dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; best = o; }
    }
    return best;
}
static Agent *seek_victim(World *w, Agent *a, int rival_faction_only) {
    Agent *best = NULL; double bd = 1e18; int vis = get_vision();
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id || o->is_police || o->arrested_ticks > 0) continue;
        if (rival_faction_only && !(o->faction_id >= 0 && o->faction_id != a->faction_id)) continue;
        if (vis && !agent_can_see(w, a, (int)o->x, (int)o->y)) continue;   /* stalk prey in sight */
        double dx = o->x - a->x, dy = o->y - a->y, dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; best = o; }
    }
    return best;
}

/* EV victim (crime-wealth mode): go where the money is. Pick the nearby mark that
 * maximises expected take ≈ wealth · P(success) / distance, where P(success) rises
 * with the perp's skill and falls in high-heat (well-watched) areas. No rng. */
static Agent *ev_victim(World *w, Agent *a) {
    Agent *best = NULL; double bestEV = -1; int vis = get_vision();
    double skill = a->crime_skill;
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id || o->is_police || o->arrested_ticks > 0) continue;
        if (vis && !agent_can_see(w, a, (int)o->x, (int)o->y)) continue;
        double dx = o->x - a->x, dy = o->y - a->y, dist = sqrt(dx*dx + dy*dy);
        if (dist > 16.0) continue;
        double risk = w->danger_[(int)o->x][(int)o->y] / 255.0;
        double psucc = clampd(0.4 + 0.5*skill - 0.3*risk, 0.1, 0.95);
        double ev = o->needs.money * psucc / (1.0 + 0.12*dist);
        if (ev > bestEV) { bestEV = ev; best = o; }
    }
    return best;
}
/* richest nearby home to burgle (crime-wealth mode): wealth discounted by distance. */
static int richest_home(World *w, Agent *a) {
    int best = -1; double bestScore = -1;
    for (int b = 0; b < w->n_buildings; b++) { Building *hb = &w->buildings[b];
        if (hb->type != T_HOME || hb->hh_size <= 0) continue;
        double dx = hb->x - a->x, dy = hb->y - a->y, dist = sqrt(dx*dx + dy*dy);
        if (dist > 30.0) continue;
        double score = hb->hh_wealth / (1.0 + 0.1*dist);
        if (score > bestScore) { bestScore = score; best = b; }
    }
    return best >= 0 ? best : building_nearest_reachable(w, (int)a->x, (int)a->y, T_HOME);
}

static void set_target(World *w, Agent *a) {
    int b = -1;
    switch (a->action) {
        case A_EAT:
            if (a->needs.hunger < NEED_CRITICAL) { a->tx = -1; a->ty = -1; return; }
            b = nearest_service((int)a->x, (int)a->y, SVC_SHOP); break;
        case A_SHOP:  b = nearest_service((int)a->x, (int)a->y, SVC_SHOP); break;
        case A_SLEEP:
            if (a->needs.energy < NEED_CRITICAL) { a->tx = -1; a->ty = -1; return; }  /* drop where you are */
            b = a->home_id; break;
        case A_GO_HOME: b = a->home_id; break;
        case A_WORK:  b = a->workplace_id; break;
        case A_DRINK: b = nearest_service((int)a->x, (int)a->y, SVC_BAR); break;
        case A_PRAY:  b = nearest_service((int)a->x, (int)a->y, SVC_CHURCH); break;
        case A_TREAT: b = nearest_service((int)a->x, (int)a->y, SVC_SHOP); break;  /* pharmacy/clinic */
        case A_CRIME: {
            Agent *tgt = NULL;
            if (a->crime_role == CR_KINGPIN) { a->tx = -1; a->ty = -1; return; }  /* stay put, deal wholesale */
            else if (a->crime_role == CR_DEALER) tgt = a->drug_stock > 0 ? seek_customer(w, a) : seek_kingpin(w, a);
            else if (a->crime_role == CR_KILLER) tgt = seek_victim(w, a, 0);
            else if (a->faction_id >= 0) { tgt = seek_victim(w, a, 1); if (!tgt) tgt = seek_victim(w, a, 0); }
            else tgt = get_crime_wealth() ? ev_victim(w, a) : seek_victim(w, a, 0);   /* go where the money is */
            if (tgt) { a->tx = (int)tgt->x; a->ty = (int)tgt->y; return; }
            if (a->crime_role == CR_CAREER)   /* go burgle: the richest nearby home in wealth mode */
                b = get_crime_wealth() ? richest_home(w, a) : building_nearest_reachable(w, (int)a->x, (int)a->y, T_HOME);
            break;
        }
        default: break;
    }
    if (b >= 0) { a->tx = w->buildings[b].x; a->ty = w->buildings[b].y; return; }
    /* wander / crime / flee / patrol / no building: random nearby tile */
    a->tx = (int)clampd(a->x + rng_int_incl(&w->rng, -10, 10), 0, WORLD_W - 1);
    a->ty = (int)clampd(a->y + rng_int_incl(&w->rng, -10, 10), 0, WORLD_H - 1);
}

static int arrived(const Agent *a) {
    return a->tx < 0 || ((int)a->x == a->tx && (int)a->y == a->ty);
}

static const char *choose_crime_kind(World *w, Agent *a, Agent *target) {
    Personality *p = &a->pers;
    Rng *r = &w->rng;

    /* serial killer: kill for the thrill (needs a victim nearby) */
    if (a->crime_role == CR_KILLER) return target ? "murder" : "burglary";

    /* drug trade */
    if (a->crime_role == CR_KINGPIN) return "trafficking";
    if (a->crime_role == CR_DEALER)  return a->drug_stock > 0 ? "dealing" : "trafficking";

    /* faction / rivalry violence */
    if (target) {
        if (a->faction_id >= 0 && target->faction_id >= 0 && target->faction_id != a->faction_id)
            return rng_double(r) < 0.5 ? "assault" : "vandalism";                 /* turf war */
        if (a->faction_id >= 0 && target->faction_id < 0 && rng_double(r) < 0.4)
            return "extortion";                                                   /* shakedown */
        Relation *rel = rel_get(&a->rels, target->id);
        if (rel && rel->affinity <= RIVALRY_AFFINITY &&
            (pers_has(p, TR_VENGEFUL) || pers_has(p, TR_CRUEL)) && rng_double(r) < 0.6)
            return "assault";                                                     /* settle a grudge */
        if ((pers_has(p, TR_CRUEL) || pers_has(p, TR_BRAVE)) && rng_double(r) < 0.25)
            return "assault";
    }

    /* career criminal: escalate to bigger scores as notoriety grows */
    if (a->crime_role == CR_CAREER) {
        int cc = a->crimes_committed;
        if (cc >= ESCALATE_T3 && rng_double(r) < 0.35) return "arson";
        if (cc >= ESCALATE_T2) return rng_double(r) < 0.6 ? "robbery" : "burglary";
        if (cc >= ESCALATE_T1) return rng_double(r) < 0.5 ? "burglary" : (target ? "robbery" : "theft");
        return target ? "robbery" : "burglary";
    }

    /* desperate citizen */
    if (a->needs.money < LOW_MONEY * 0.5 && target && rng_double(r) < 0.3) return "robbery";
    return target ? "theft" : "burglary";
}

static Agent *nearest_other(World *w, Agent *a, int radius) {
    Agent *best = NULL; int bd = radius * radius + 1; int vis = get_vision();
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id) continue;
        int d = ((int)o->x - (int)a->x) * ((int)o->x - (int)a->x) +
                ((int)o->y - (int)a->y) * ((int)o->y - (int)a->y);
        if (d <= bd) {
            if (vis && !agent_can_see(w, a, (int)o->x, (int)o->y)) continue;  /* only who you can see */
            bd = d; best = o;
        }
    }
    return best;
}

static void do_socialize(World *w, Agent *a) {
    Agent *o = nearest_other(w, a, 4);
    if (!o) return;
    double delta = rng_range(&w->rng, -0.15, 0.18);
    double compat = 1.0 - fabs(a->pers.a - o->pers.a);
    delta += (compat - 0.5) * 0.30;
    /* cultural homophily: a shared tongue/culture/faith eases a bond; a language
       barrier or sectarian difference strains it (communication shapes society) */
    if (a->language == o->language) delta += 0.08; else delta -= 0.10;
    if (a->culture == o->culture)   delta += 0.05;
    if (a->faith != FAITH_NONE && o->faith != FAITH_NONE)
        delta += (a->faith == o->faith) ? 0.04 : -0.03;
    rel_adjust(&a->rels, o->id, delta);
    rel_adjust(&o->rels, a->id, delta * 0.8);
    a->needs.social = clampd(a->needs.social + 0.2, 0, 1);
    o->needs.social = clampd(o->needs.social + 0.1, 0, 1);
    a->needs.belonging = clampd(a->needs.belonging + 0.05, 0, 1);

    Relation *ra = rel_get(&a->rels, o->id);
    if (ra && ra->affinity >= FRIENDSHIP_AFFINITY && ra->familiarity >= 0.4 && !ra->announced_friend) {
        ra->announced_friend = 1;
        Relation *ro = rel_get(&o->rels, a->id); if (ro) ro->announced_friend = 1;
        char t[96]; snprintf(t, sizeof(t), "%s and %s became friends", a->name, o->name);
        events_post(w, EV_FRIENDS, a->id, o->id, (int)a->x, (int)a->y, 0.25, t);
        mem_add(&a->mem, "Became friends", 0.5);
    } else if (delta > 0.2) {
        mem_add(&a->mem, "Had a good talk", 0.3);
    } else if (delta < -0.05) {
        mem_add(&a->mem, "Quarreled with someone", 0.5);
        /* announce a real falling-out only when affinity crosses into rivalry */
        if (ra && ra->affinity <= RIVALRY_AFFINITY && !ra->announced_rival) {
            ra->announced_rival = 1;
            Relation *ro = rel_get(&o->rels, a->id); if (ro) ro->announced_rival = 1;
            char t[96]; snprintf(t, sizeof(t), "%s and %s became rivals", a->name, o->name);
            events_post(w, EV_QUARREL, a->id, o->id, (int)a->x, (int)a->y, 0.3, t);
        }
    }
}

static void execute_action(World *w, Agent *a) {
    Needs *n = &a->needs;
    switch (a->action) {
        case A_EAT:
            if (n->money >= MEAL_PRICE) { n->money -= MEAL_PRICE; n->hunger = clampd(n->hunger + 0.6, 0, 1); }
            else if (n->hunger < NEED_CRITICAL) n->hunger = clampd(n->hunger + 0.3, 0, 1);
            break;
        case A_SLEEP:   n->energy = clampd(n->energy + 0.35, 0, 1); break;
        case A_GO_HOME: n->energy = clampd(n->energy + 0.08, 0, 1); break;
        case A_WORK: {  /* pay scales with the worker's craft + schooling (human capital) */
                        double pay = WAGE_PER_SHIFT * w->econ.wage_mult * worker_output(a) * occ_base_wage(a->occupation);
                        n->money += pay;
                        a->day_income += pay;     /* household net income (dashboard) */
                        w->wages_earned += pay;   /* cumulative legal income (dashboard) */
                        n->energy = clampd(n->energy - 0.1, 0, 1);
                        n->meaning = clampd(n->meaning + 0.05, 0, 1);
                        a->reputation = (float)clampd(a->reputation + 0.004, -1, 1);
                        /* learn the trade by doing — diminishing, faster for the bright */
                        if (a->craft < 1.0f)
                            a->craft = (float)clampd(a->craft + CRAFT_GAIN * (1.0 - a->craft) * (0.5 + a->intellect), 0, 1);
                        break; }
        case A_SOCIALIZE: do_socialize(w, a); break;
        case A_DRINK: { double price = DRINK_PRICE * w->econ.goods_price;
                        if (n->money >= price) { n->money -= price;
                            n->social = clampd(n->social + 0.3, 0, 1); do_socialize(w, a); } break; }
        case A_PRAY:    /* the faithful draw more meaning from worship than the secular */
                        n->meaning = clampd(n->meaning + (a->faith != FAITH_NONE ? 0.45 : 0.25), 0, 1);
                        n->safety = clampd(n->safety + 0.1, 0, 1);
                        if (a->faith != FAITH_NONE) n->belonging = clampd(n->belonging + 0.08, 0, 1);
                        a->reputation = (float)clampd(a->reputation + 0.004, -1, 1); break;
        case A_SHOP: {  double price = LUXURY_PRICE * w->econ.goods_price;
                        if (n->money >= price) { n->money -= price;
                            n->belonging = clampd(n->belonging + 0.2, 0, 1); } break; }
        case A_CRIME: {
            Agent *target = nearest_other(w, a, 6);
            crime_attempt(w, a, target, choose_crime_kind(w, a, target));
            break; }
        case A_FLEE:    n->safety = clampd(n->safety + 0.05, 0, 1); break;
        case A_TREAT: {
            double med = 1.0 + 0.6 * w->sci.adoption[TECH_MEDICINE];   /* better medicine heals more */
            if (n->money >= TREAT_COST) { n->money -= TREAT_COST; a->injury = (float)clampd(a->injury - 0.6*med, 0, 2); }
            else a->injury = (float)clampd(a->injury - 0.2*med, 0, 2);   /* self-care if you can't pay */
            n->safety = clampd(n->safety + 0.1, 0, 1);
            break; }
        default: break;
    }
}

/* one tile toward the target: follow the cached A*+JPS path, recomputing on
   retarget or when a long (truncated) path runs out; greedy fallback if unreachable. */
static int agent_next_step(const World *w, Agent *a, int *nx, int *ny) {
    int cx = (int)a->x, cy = (int)a->y;
    int retarget  = (a->path_tx != (short)a->tx || a->path_ty != (short)a->ty);
    int exhausted = (a->path_i >= a->path_len);
    if (retarget || (exhausted && a->path_len > 0 && (cx != a->tx || cy != a->ty))) {
        int n = find_path(w, cx, cy, a->tx, a->ty, a->path, PATH_MAX);
        a->path_len = (short)(n > 0 ? n : 0); a->path_i = 0;
        a->path_tx = (short)a->tx; a->path_ty = (short)a->ty;
    }
    static const int DX[4] = {1,-1,0,0}, DY[4] = {0,0,1,-1};
    if (a->path_i < a->path_len) {
        int d = a->path[a->path_i];
        int px = cx + DX[d], py = cy + DY[d];
        if (tile_walkable(w, px, py)) { a->path_i++; *nx = px; *ny = py; return 1; }
    }
    return path_step(w, cx, cy, a->tx, a->ty, nx, ny);   /* unreachable/edge -> greedy */
}

static void move_toward(const World *w, Agent *a, double dt) {
    if (a->tx < 0) return;
    a->move_progress += dt * AGENT_SPEED_TPS;
    while (a->move_progress >= 1.0 && !((int)a->x == a->tx && (int)a->y == a->ty)) {
        int nx, ny;
        if (agent_next_step(w, a, &nx, &ny) && (nx != (int)a->x || ny != (int)a->y)) {
            if (nx > a->x) a->facing = 'E'; else if (nx < a->x) a->facing = 'W';
            else if (ny > a->y) a->facing = 'S'; else if (ny < a->y) a->facing = 'N';
            a->x = nx; a->y = ny;
        } else break;   /* boxed in — stop burning move budget this tick */
        a->move_progress -= 1.0;
    }
}

static void set_mood_color(Agent *a) {
    const Needs *n = &a->needs;
    double wb = (n->hunger * 1.2 + n->energy + n->safety * 1.1 +
                 n->social * 0.9 + n->meaning + n->belonging) / 6.2;
    wb = clampd(wb, 0.0, 1.0);
    if (a->wanted) { a->r = 230; a->g = 70; a->b = 60; return; }        /* wanted = red */
    if (a->is_police) { a->r = 90; a->g = 150; a->b = 230; return; }     /* police = blue */
    a->r = (unsigned char)(230 * (1 - wb) + 120 * wb);
    a->g = (unsigned char)(90 * (1 - wb) + 220 * wb);
    a->b = (unsigned char)(80 * (1 - wb) + 130 * wb);
}

/* Interest heuristic: when is an agent worth an LLM consult? (decision_router) */
static int should_consult(World *w, Agent *a) {
    double score = 0.0;
    if (needs_is_critical(&a->needs)) score += 0.5;
    if (fabs(a->needs.social - 0.5) > 0.35) score += 0.2;
    for (int i = 0; i < a->rels.n; i++)
        if (a->rels.rel[i].affinity <= RIVALRY_AFFINITY) { score += 0.15; break; }
    if (a->faction_id != -1) score += 0.15;
    if (a->pers.o > 0.7) score += 0.10;
    double p = score < 0.8 ? score : 0.8;
    return rng_double(&w->rng) < p;
}

/* ── Tick ────────────────────────────────────────────────────────────────── */
void world_tick(World *w, double dt_seconds) {
    w->tick++;                       /* deterministic step clock (replay stamps events by it) */
    double game_hours = dt_seconds * TIME_SCALE / 3600.0;
    int prev_day = w->day;
    w->hour += game_hours;
    while (w->hour >= 24.0) { w->hour -= 24.0; w->day++; }
    int new_day = (w->day != prev_day);

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

        /* aging: advance toward the next birthday (even while jailed) */
        a->age_frac += (float)(game_hours * get_years_per_day() / 24.0);
        if (a->age_frac >= 1.0f) { int yrs = (int)a->age_frac; a->age += yrs; a->age_frac -= (float)yrs; }

        if (a->arrested_ticks > 0) {
            if (--a->arrested_ticks == 0) { a->jailed_for[0] = '\0'; a->sentence_total = 0; a->jail_gang = 0; }
            set_mood_color(a);
            continue;
        }

        needs_decay(&a->needs, game_hours);

        if (a->action_progress <= 0.0) {
            a->action = utility_best_action(a, w);
            a->action_progress = ACTION_SECONDS;
            a->acted = 0;
            set_target(w, a);
            /* Occasionally ask the LLM to override (non-blocking). */
            if (llm_enabled() && !a->llm_pending && should_consult(w, a)) {
                char prompt[768];
                llm_build_prompt(a, w, prompt, sizeof(prompt));
                if (llm_submit(a->id, prompt)) a->llm_pending = 1;
            }
        }
        move_toward(w, a, dt_seconds);
        if (arrived(a) && !a->acted) { execute_action(w, a); a->acted = 1; }
        a->action_progress -= dt_seconds;

        if (new_day) rel_decay_all(&a->rels, 1.0);

        set_mood_color(a);

        if (needs_is_dying(&a->needs)) {
            a->alive = 0; w->deaths++;
            if (a->spouse_id >= 0) { Agent *sp = world_agent_by_id(w, a->spouse_id);
                if (sp) sp->spouse_id = -1; a->spouse_id = -1; a->pregnant_ticks = 0; }
            char t[96];
            const char *reason = a->needs.hunger <= 0.01 ? "starvation" : "exhaustion";
            snprintf(t, sizeof(t), "%s died (%s)", a->name, reason);
            events_post(w, EV_DEATH, a->id, -1, (int)a->x, (int)a->y, 0.9, t);
        }
    }

    /* Apply any LLM decisions that have come back (deferred override). */
    int aid, act;
    while (llm_poll(&aid, &act)) {
        for (int i = 0; i < w->n_agents; i++) {
            if (w->agents[i].id != aid) continue;
            Agent *a = &w->agents[i];
            a->llm_pending = 0;
            if (a->alive && a->arrested_ticks == 0 && act >= 0 && act < A_COUNT) {
                a->action = (Action)act;
                a->acted = 0;
                set_target(w, a);
            }
            break;
        }
    }

    crime_tick(w);
    warfare_tick(w);

    if (new_day) { economy_daily(w); factions_daily(w); crime_daily(w); jail_tick(w); kinship_daily(w); law_daily(w); culture_daily(w); knowledge_daily(w); lifecycle_daily(w); danger_decay(w); households_daily(w); }
    metrics_sample_maybe(w);   /* samples on the configured cadence (default once/day) */
}

/* ── Save / load (binary; World is pointer-free POD) ─────────────────────── */
int world_save(const World *w, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t wr = fwrite(w, sizeof(World), 1, f);
    fclose(f);
    return wr == 1;
}
int world_load(World *w, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t rd = fread(w, sizeof(World), 1, f);
    fclose(f);
    if (rd == 1) { compute_components(w); compute_services(w); }   /* derived, not stored */
    return rd == 1;
}

static int cmp_double(const void *a, const void *b);   /* defined with the metrics code below */

/* Residential mobility (Phase 4): households whose per-capita wealth no longer matches
 * their home's value relocate — up when they've prospered, down when they've slipped —
 * to the best-matching home with room. Gentrification and decline over time. Draws
 * w->rng (fixed order) only on the neighborhoods path, so defaults stay byte-identical. */
static void residential_move(World *w) {
    double med = w->median_wealth > 0 ? w->median_wealth : 1.0;
    for (int b = 0; b < w->n_buildings; b++) {
        Building *hb = &w->buildings[b];
        if (hb->type != T_HOME || hb->hh_size <= 0) continue;
        double wt = (hb->hh_wealth / hb->hh_size) / (3.0 * med);
        if (wt > 1) wt = 1; if (wt < 0) wt = 0;
        double mismatch = wt - hb->value;
        if (fabs(mismatch) < 0.20) continue;              /* content where they are */
        if (rng_double(&w->rng) > 0.08) continue;          /* only a few move per day */
        int bestB = -1; double bestScore = fabs(mismatch); /* must beat staying put */
        for (int c = 0; c < w->n_buildings; c++) {
            Building *cb = &w->buildings[c];
            if (c == b || cb->type != T_HOME) continue;
            if (cb->capacity - cb->hh_size < hb->hh_size) continue;   /* room for the whole household */
            double score = fabs((double)cb->value - wt);
            if (score < bestScore) { bestScore = score; bestB = c; }
        }
        if (bestB < 0) continue;
        int moved = 0;
        for (int i = 0; i < w->n_agents; i++) { Agent *a = &w->agents[i];
            if (!a->alive || a->home_id != b) continue;
            a->home_id = bestB; a->x = w->buildings[bestB].x; a->y = w->buildings[bestB].y; moved++;
        }
        w->buildings[bestB].hh_size += moved; w->buildings[bestB].n_residents += moved;
        hb->hh_size -= moved; if (hb->n_residents >= moved) hb->n_residents -= moved;
        w->n_moves++;
    }
}

/* ── Households: recompute per-home aggregates (call on day change) ────────────
 * A household = living agents sharing a home_id. hh_wealth = Σ residents' money;
 * hh_income = Σ residents' net legitimate cash flow for the day (day_income), which
 * is then reset for the next day. Pure accounting over existing state. */
void households_daily(World *w) {
    for (int b = 0; b < w->n_buildings; b++)
        if (w->buildings[b].type == T_HOME) {
            w->buildings[b].hh_size = 0; w->buildings[b].hh_wealth = 0; w->buildings[b].hh_income = 0;
        }
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        if (a->home_id >= 0 && w->buildings[a->home_id].type == T_HOME) {
            Building *hb = &w->buildings[a->home_id];
            hb->hh_size++;
            hb->hh_wealth += a->needs.money;
            hb->hh_income += a->day_income;
        }
        a->day_income = 0;   /* reset the accumulator for the next day */
    }

    /* dynamic home value (Phase 4): desirability drifts toward a blend of location,
     * resident wealth and (minus) local crime heat → gentrification and decline. Only
     * after the economy has a median (skips initial seeding, so no churn at spawn). */
    if (get_neighborhoods() && w->median_wealth > 0) {
        double med = w->median_wealth;
        for (int b = 0; b < w->n_buildings; b++) {
            Building *hb = &w->buildings[b];
            if (hb->type != T_HOME) continue;
            double base = home_value(w, hb->x, hb->y);                 /* static locational worth */
            double wealth = hb->hh_size > 0 ? (hb->hh_wealth / hb->hh_size) / (3.0 * med) : 0.0;
            if (wealth > 1) wealth = 1;
            double heat = w->danger_[hb->x][hb->y] / 255.0;
            double target = 0.45*base + 0.45*wealth - 0.25*heat + 0.10;
            if (target < 0) target = 0; if (target > 1) target = 1;
            hb->value += (float)(0.12 * (target - hb->value));          /* EMA drift */
        }
        residential_move(w);   /* households relocate as their fortunes diverge from their home */
    }

    /* affluence field for the neighborhoods overlay: splat each home's wealth into a
     * disc, keeping the max, so rich/poor quarters read as smooth blobs on the map. */
    if (get_neighborhoods()) {
        memset(w->affluence_, 0, sizeof(w->affluence_));
        /* scale by the 85th percentile of per-capita household wealth, so one
         * rent-rich landlord doesn't crush the whole map to near-zero. */
        static double pc[MAX_BUILDINGS]; int np = 0;
        for (int b = 0; b < w->n_buildings; b++) {
            Building *hb = &w->buildings[b];
            if (hb->type == T_HOME && hb->hh_size > 0) pc[np++] = hb->hh_wealth / hb->hh_size;
        }
        if (np == 0) return;
        qsort(pc, np, sizeof(double), cmp_double);
        double scale = pc[(np * 85) / 100];
        if (scale < 1.0) scale = 1.0;
        const int R = 6;
        for (int b = 0; b < w->n_buildings; b++) {
            Building *hb = &w->buildings[b];
            if (hb->type != T_HOME || hb->hh_size <= 0) continue;
            double pcw = hb->hh_wealth / hb->hh_size;
            int base = (int)(255.0 * pcw / scale); if (base > 255) base = 255;
            for (int yy = hb->y - R; yy <= hb->y + R; yy++) for (int xx = hb->x - R; xx <= hb->x + R; xx++) {
                if (xx < 0 || yy < 0 || xx >= WORLD_W || yy >= WORLD_H) continue;
                int dd = abs(xx - hb->x) + abs(yy - hb->y);
                int v = base - base * dd / (2 * R);     /* linear falloff */
                if (v > w->affluence_[xx][yy]) w->affluence_[xx][yy] = (uint8_t)(v < 0 ? 0 : v);
            }
        }
    }
}

/* ── Balance metrics: one CSV row per game-day for the monitoring dashboard ────
 * Exports *all* daily aggregates (not a curated subset) — one row is tiny and you
 * can't chart a variable you never recorded. The header and the row are emitted
 * from the SAME loops over the enums (crime kinds, techs, faiths, languages), so
 * the columns and values can never drift. Per-agent data is deliberately excluded:
 * it is not a time series and would be megabytes per day. These are pure read-only
 * observers of World — they draw no w->rng numbers, so default runs stay
 * byte-identical whether or not export is enabled. */
static FILE *g_metrics = NULL;
static double g_last_sample = -1e18;   /* game-hours (day*24+hour) of the last row written */

/* Write ",<prefix><slug>" — a CSV-safe column name: lowercase, non-alphanumerics
 * (spaces, etc.) collapsed to '_'. Keeps headers machine-friendly ("Old Faith"
 * -> "faith_old_faith") regardless of the display names the name functions return. */
static void fput_slug(FILE *f, const char *prefix, const char *name) {
    fprintf(f, ",%s", prefix);
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
        fputc(c, f);
    }
}

void metrics_open(const char *path) {
    if (!path || !*path) return;
    if (g_metrics) { fclose(g_metrics); g_metrics = NULL; }
    g_metrics = fopen(path, "w");
    if (!g_metrics) return;
    g_last_sample = -1e18;    /* so the first tick emits a row immediately */
    /* fixed columns — t is fractional game-days (day + hour/24), the x-axis */
    fprintf(g_metrics,
        "t,day,hour,alive,children,youths,adults,elders,avg_age,"
        "couples,married,pregnant,born_alive,deaths,in_faction,avg_friends,max_friends,"
        "avg_money,goods_price,wage,indebted,total_debt,landlords,"
        "wages_earned,crime_income,"
        "crimes,wanted,jailed");
    /* per-crime-kind columns: count, then cumulative $ proceeds */
    for (int i = 0; i < CK_COUNT; i++) fput_slug(g_metrics, "crime_", crime_kind_name(i));
    for (int i = 0; i < CK_COUNT; i++) fput_slug(g_metrics, "take_", crime_kind_name(i));
    /* knowledge */
    fprintf(g_metrics, ",research,theories,techs,avg_edu,avg_craft,avg_intellect");
    for (int t = 0; t < TECH_COUNT; t++) fput_slug(g_metrics, "tech_", tech_name(t));
    /* culture */
    fprintf(g_metrics, ",religious,common_tongue");
    for (int i = 0; i < FAITH_COUNT; i++) fput_slug(g_metrics, "faith_", faith_name((unsigned char)i));
    for (int i = 0; i < LANG_COUNT; i++) fput_slug(g_metrics, "lang_", language_name((unsigned char)i));
    /* governance / factions */
    fprintf(g_metrics, ",factions,wars,war_casualties,crackdown");
    /* socioeconomics: wealth inequality + household income distribution + class tiers */
    fprintf(g_metrics, ",gini,hh_income_p25,hh_income_med,hh_income_p75,hh_income_mean");
    fprintf(g_metrics, ",loot_poor,loot_mid,loot_rich,moves,segregation,cr_poor,cr_mid,cr_rich");
    fprintf(g_metrics, ",loc_poor,loc_mid,loc_rich");
    for (int i = 0; i < 5; i++) fprintf(g_metrics, ",class%d", i);
    fprintf(g_metrics, "\n");
    fflush(g_metrics);
}

/* ascending qsort comparator for doubles (metrics quantiles) */
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void metrics_tick(World *w) {     /* called on day change; no-op unless a file is open */
    if (!g_metrics) return;
    int alive=0,ch=0,yo=0,ad=0,el=0,couples=0,preg=0,born=0,indebt=0,relig=0,common=0;
    int in_faction=0, landlords=0, total_friends=0, max_friends=0;
    long agesum=0; double money=0,debt=0,edu=0,craft=0,intel=0;
    int faith[FAITH_COUNT]={0}, lang[LANG_COUNT]={0};
    for (int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i];
        if(a->mother_id>=0) born++;            /* cumulative: born into the city */
        if(!a->alive) continue;
        alive++; agesum+=a->age; money+=a->needs.money; edu+=a->education; craft+=a->craft; intel+=a->intellect;
        switch(life_stage(a)){ case LS_CHILD:ch++;break; case LS_YOUTH:yo++;break; case LS_ELDER:el++;break; default:ad++; }
        if(a->spouse_id>=0) couples++;         /* counts both partners; halved below */
        if(a->pregnant_ticks>0) preg++;
        if(a->debt>0.5) indebt++;
        debt+=a->debt;
        if(a->faction_id!=-1) in_faction++;
        if(count_properties(w,a->id)>0) landlords++;
        int fr=0; for(int j=0;j<a->rels.n;j++) if(a->rels.rel[j].affinity>=FRIENDSHIP_AFFINITY) fr++;
        total_friends+=fr; if(fr>max_friends) max_friends=fr;
        if(a->faith!=FAITH_NONE) relig++;
        if(a->language==LANG_COMMON) common++;
        faith[a->faith]++; lang[a->language]++;
    }
    int married = couples;                      /* married agents (both partners) */
    int wars=0, war_cas=0;
    for(int i=0;i<w->n_factions;i++){ if(w->factions[i].war_with>i) wars++; war_cas+=w->factions[i].casualties; }
    int factions=0; for(int i=0;i<w->n_factions;i++) if(w->factions[i].active && w->factions[i].members) factions++;
    int techs=0; for(int k=0;k<TECH_COUNT;k++) techs+=w->sci.discovered[k];
    double crime_income=0; for(int k=0;k<CK_COUNT;k++) crime_income+=w->crime_take[k];

    /* ── socioeconomics: wealth Gini, household-income quartiles, class tiers ── */
    static double mv[MAX_AGENTS];        /* living agents' money, for the Gini */
    int nm=0; int cls[5]={0,0,0,0,0};
    for (int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
        mv[nm++]=a->needs.money; if(a->status<5) cls[a->status]++; }
    double gini=0;
    if (nm>0){ qsort(mv,nm,sizeof(double),cmp_double);
        double cum=0,wsum=0; for(int i=0;i<nm;i++){ cum+=(double)(i+1)*mv[i]; wsum+=mv[i]; }
        if (wsum>0) gini = (2.0*cum)/(nm*wsum) - (double)(nm+1)/nm; }
    static double hi[MAX_BUILDINGS];     /* occupied homes' net income, for quartiles */
    int nh=0; double hisum=0, pcwsum=0, pcwsq=0;
    for (int b=0;b<w->n_buildings;b++) if(w->buildings[b].type==T_HOME && w->buildings[b].hh_size>0){
        hi[nh++]=w->buildings[b].hh_income; hisum+=w->buildings[b].hh_income;
        double pc=w->buildings[b].hh_wealth/w->buildings[b].hh_size; pcwsum+=pc; pcwsq+=pc*pc; }
    double hp25=0,hmed=0,hp75=0,hmean=0;
    if (nh>0){ qsort(hi,nh,sizeof(double),cmp_double);
        hp25=hi[nh/4]; hmed=hi[nh/2]; hp75=hi[(nh*3)/4]; hmean=hisum/nh; }
    /* segregation = spread of per-home per-capita wealth (coefficient of variation):
       higher = wealth more clustered by neighborhood. */
    double seg=0;
    if (nh>0){ double m=pcwsum/nh, var=pcwsq/nh - m*m; if(var<0)var=0; if(m>0) seg=sqrt(var)/m; }

    fprintf(g_metrics, "%.4f,%d,%.2f,%d,%d,%d,%d,%d,%ld",
        w->day + w->hour / 24.0, w->day, w->hour, alive, ch, yo, ad, el, alive?agesum/alive:0);
    fprintf(g_metrics, ",%d,%d,%d,%d,%d,%d,%.2f,%d",
        couples/2, married, preg, born, w->deaths, in_faction,
        alive?(double)total_friends/alive:0.0, max_friends);
    fprintf(g_metrics, ",%.0f,%.2f,%.2f,%d,%.0f,%d",
        alive?money/alive:0.0, w->econ.goods_price, w->econ.wage_mult, indebt, debt, landlords);
    fprintf(g_metrics, ",%.0f,%.0f", w->wages_earned, crime_income);
    fprintf(g_metrics, ",%d,%d,%d", w->crimes, crime_wanted_count(w), crime_jailed_count(w));
    for (int i=0;i<CK_COUNT;i++) fprintf(g_metrics, ",%d", w->crime_kind[i]);
    for (int i=0;i<CK_COUNT;i++) fprintf(g_metrics, ",%.0f", w->crime_take[i]);
    fprintf(g_metrics, ",%.0f,%d,%d,%.1f,%.1f,%.1f",
        w->sci.research, w->sci.theories, techs,
        alive?edu/alive*100:0.0, alive?craft/alive*100:0.0, alive?intel/alive*100:0.0);
    for (int t=0;t<TECH_COUNT;t++) fprintf(g_metrics, ",%.0f", w->sci.adoption[t]*100);
    fprintf(g_metrics, ",%d,%d", relig, common);
    for (int i=0;i<FAITH_COUNT;i++) fprintf(g_metrics, ",%d", faith[i]);
    for (int i=0;i<LANG_COUNT;i++) fprintf(g_metrics, ",%d", lang[i]);
    fprintf(g_metrics, ",%d,%d,%d,%d", factions, wars, war_cas, w->crackdown_days>0?1:0);
    fprintf(g_metrics, ",%.4f,%.0f,%.0f,%.0f,%.0f", gini, hp25, hmed, hp75, hmean);
    fprintf(g_metrics, ",%.0f,%.0f,%.0f", w->loot_tier[0], w->loot_tier[1], w->loot_tier[2]);
    fprintf(g_metrics, ",%d,%.4f,%d,%d,%d", w->n_moves, seg, w->crimes_tier[0], w->crimes_tier[1], w->crimes_tier[2]);
    fprintf(g_metrics, ",%d,%d,%d", w->crimes_loc_tier[0], w->crimes_loc_tier[1], w->crimes_loc_tier[2]);
    for (int i=0;i<5;i++) fprintf(g_metrics, ",%d", cls[i]);
    fprintf(g_metrics, "\n");
    fflush(g_metrics);

    /* also stream a headline metric doc to OpenSearch (tagged with the session) */
    if (rec_active() && os_ingest_enabled()) {
        char d[640];
        snprintf(d, sizeof d,
            "[{\"doc_type\":\"metric\",\"session_id\":\"%s\",\"tick\":%llu,\"t\":%.4f,\"day\":%d,"
            "\"alive\":%d,\"deaths\":%d,\"crimes\":%d,\"avg_money\":%.0f,\"gini\":%.4f,"
            "\"hh_income_med\":%.0f,\"theories\":%d,\"techs\":%d,\"factions\":%d,\"wars\":%d}]",
            rec_session_id(), (unsigned long long)w->tick, w->day + w->hour/24.0, w->day,
            alive, w->deaths, w->crimes, alive?money/alive:0.0, gini, hmed,
            w->sci.theories, techs, factions, wars);
        os_ingest(d);
    }
}

/* Called every tick: write a row whenever the sampling cadence has elapsed. Default
 * cadence is 24 game-hours (one row per day); set smaller (e.g. 1) for intra-day. */
void metrics_sample_maybe(World *w) {
    if (!g_metrics) return;
    double now = w->day * 24.0 + w->hour;             /* elapsed game-hours */
    if (now - g_last_sample >= get_metrics_every() - 1e-9) {
        g_last_sample = now;
        metrics_tick(w);
    }
}

/* Sidecar manifest written beside the metrics CSV: records seed + days + every knob
 * so a session can be reproduced deterministically (--rerun). Plain KEY=VALUE lines. */
void metrics_write_manifest(const char *csv_path, unsigned int seed, int days) {
    if (!csv_path || !*csv_path) return;
    char mpath[1024];
    snprintf(mpath, sizeof mpath, "%s.meta", csv_path);
    FILE *f = fopen(mpath, "w");
    if (!f) return;
    fprintf(f, "seed=%u\n", seed);
    fprintf(f, "days=%d\n", days);
    fprintf(f, "years_per_day=%g\n", get_years_per_day());
    fprintf(f, "family_share=%g\n", get_family_share());
    fprintf(f, "kids_min=%d\n", get_family_kids_min());
    fprintf(f, "kids_max=%d\n", get_family_kids_max());
    fprintf(f, "pop_target=%d\n", get_pop_target());
    fprintf(f, "research_rate=%g\n", get_research_rate());
    fprintf(f, "production=%g\n", get_craft_bonus());
    fprintf(f, "child_cost=%g\n", get_child_cost());
    fprintf(f, "metrics_every=%g\n", get_metrics_every());
    fprintf(f, "occ_pay_spread=%g\n", get_occ_pay_spread());
    fprintf(f, "neighborhoods=%d\n", get_neighborhoods());
    fprintf(f, "crime_wealth=%d\n", get_crime_wealth());
    fprintf(f, "police_bias=%d\n", get_police_bias());
    fprintf(f, "vision=%d\n", get_vision());
    fprintf(f, "vision_radius=%d\n", get_vision_radius());
    fprintf(f, "hearing=%d\n", get_hearing());
    fprintf(f, "hearing_radius=%d\n", get_hearing_radius());
    fprintf(f, "noise_worldgen=%d\n", get_noise_worldgen());
    fclose(f);
}
