/* world.c — world generation, population, tick orchestration, save/load. */
#include "sim.h"
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
static void place_building(World *w, int x, int y, TileType t, int cap) {
    if (w->n_buildings >= MAX_BUILDINGS) return;
    Building *b = &w->buildings[w->n_buildings];
    b->id = w->n_buildings; b->type = t; b->x = x; b->y = y; b->w = 1; b->h = 1;
    b->capacity = cap; b->n_residents = 0; b->n_workers = 0;
    w->tile[x][y] = (uint8_t)t;
    w->n_buildings++;
}

void world_init(World *w, uint64_t seed) {
    memset(w, 0, sizeof(*w));
    rng_seed(&w->rng, seed, 0xCAFEu);
    Rng *r = &w->rng;
    for (int x = 0; x < WORLD_W; x++)
        for (int y = 0; y < WORLD_H; y++)
            w->tile[x][y] = (x % 12 == 0 || y % 9 == 0) ? T_ROAD : T_GRASS;

    /* scatter buildings on non-road tiles */
    for (int x = 0; x < WORLD_W; x++) {
        for (int y = 0; y < WORLD_H; y++) {
            if (w->tile[x][y] != T_GRASS) continue;
            if (w->n_buildings >= MAX_BUILDINGS) break;
            /* sparse placement spread across the whole map (~4% of grass) */
            double roll = rng_double(r);
            if (roll < 0.020)       place_building(w, x, y, T_HOME, 4);
            else if (roll < 0.028)  place_building(w, x, y, T_SHOP, 8);
            else if (roll < 0.034)  place_building(w, x, y, T_WORK, 10);
            else if (roll < 0.037)  place_building(w, x, y, T_BAR, 6);
            else if (roll < 0.039)  place_building(w, x, y, T_CHURCH, 20);
            else if (roll < 0.040)  place_building(w, x, y, T_POLICE, 6);
        }
    }
    w->hour = 8.0; w->day = 1;
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
        /* workplace: weighted so police are a small minority (~3%) */
        double wr = rng_double(r);
        TileType wtype = wr < 0.55 ? T_WORK : wr < 0.80 ? T_SHOP
                       : wr < 0.90 ? T_BAR  : wr < 0.97 ? T_CHURCH : T_POLICE;
        a->workplace_id = random_building(w, wtype);
        if (a->workplace_id < 0) a->workplace_id = random_building(w, T_WORK);
        if (a->workplace_id >= 0 && w->buildings[a->workplace_id].type == T_POLICE)
            a->is_police = 1;
        a->faction_id = -1;
        a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
        a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
        a->needs.money = clampd(rng_gauss(r, START_MONEY_MEAN, START_MONEY_SD), 0, 1e9);
        personality_random(&a->pers, r);
        a->action = A_WANDER; a->faction_id = -1;
        a->wanted_for[0] = a->jailed_for[0] = '\0';
        w->n_agents++;
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
static void set_target(World *w, Agent *a) {
    int b = -1;
    switch (a->action) {
        case A_EAT:
            if (a->needs.hunger < NEED_CRITICAL) { a->tx = -1; a->ty = -1; return; }
            b = building_nearest(w, (int)a->x, (int)a->y, T_SHOP); break;
        case A_SHOP:  b = building_nearest(w, (int)a->x, (int)a->y, T_SHOP); break;
        case A_SLEEP: case A_GO_HOME: b = a->home_id; break;
        case A_WORK:  b = a->workplace_id; break;
        case A_DRINK: b = building_nearest(w, (int)a->x, (int)a->y, T_BAR); break;
        case A_PRAY:  b = building_nearest(w, (int)a->x, (int)a->y, T_CHURCH); break;
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
    if (!target) return "theft";
    Personality *p = &a->pers;
    if ((pers_has(p, TR_CRUEL) || pers_has(p, TR_VENGEFUL) || pers_has(p, TR_BRAVE))
        && rng_double(&w->rng) < 0.5) return "assault";
    if (a->needs.money < LOW_MONEY * 0.5 && rng_double(&w->rng) < 0.4) return "robbery";
    return "theft";
}

static Agent *nearest_other(World *w, Agent *a, int radius) {
    Agent *best = NULL; int bd = radius * radius + 1;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id) continue;
        int d = ((int)o->x - (int)a->x) * ((int)o->x - (int)a->x) +
                ((int)o->y - (int)a->y) * ((int)o->y - (int)a->y);
        if (d <= bd) { bd = d; best = o; }
    }
    return best;
}

static void do_socialize(World *w, Agent *a) {
    Agent *o = nearest_other(w, a, 4);
    if (!o) return;
    double delta = rng_range(&w->rng, -0.15, 0.18);
    double compat = 1.0 - fabs(a->pers.a - o->pers.a);
    delta += (compat - 0.5) * 0.30;
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
        case A_WORK:    n->money += WAGE_PER_SHIFT; n->energy = clampd(n->energy - 0.1, 0, 1);
                        n->meaning = clampd(n->meaning + 0.05, 0, 1); break;
        case A_SOCIALIZE: do_socialize(w, a); break;
        case A_DRINK:   if (n->money >= DRINK_PRICE) { n->money -= DRINK_PRICE;
                            n->social = clampd(n->social + 0.3, 0, 1); do_socialize(w, a); } break;
        case A_PRAY:    n->meaning = clampd(n->meaning + 0.4, 0, 1); n->safety = clampd(n->safety + 0.1, 0, 1); break;
        case A_SHOP:    if (n->money >= LUXURY_PRICE) { n->money -= LUXURY_PRICE;
                            n->belonging = clampd(n->belonging + 0.2, 0, 1); } break;
        case A_CRIME: {
            Agent *target = nearest_other(w, a, 3);
            crime_attempt(w, a, target, choose_crime_kind(w, a, target));
            break; }
        case A_FLEE:    n->safety = clampd(n->safety + 0.05, 0, 1); break;
        default: break;
    }
}

static void move_toward(const World *w, Agent *a, double dt) {
    if (a->tx < 0) return;
    a->move_progress += dt * AGENT_SPEED_TPS;
    while (a->move_progress >= 1.0 && !((int)a->x == a->tx && (int)a->y == a->ty)) {
        int nx, ny;
        if (path_step(w, (int)a->x, (int)a->y, a->tx, a->ty, &nx, &ny)) {
            if (nx > a->x) a->facing = 'E'; else if (nx < a->x) a->facing = 'W';
            else if (ny > a->y) a->facing = 'S'; else if (ny < a->y) a->facing = 'N';
            a->x = nx; a->y = ny;
        }
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

/* ── Tick ────────────────────────────────────────────────────────────────── */
void world_tick(World *w, double dt_seconds) {
    double game_hours = dt_seconds * TIME_SCALE / 3600.0;
    int prev_day = w->day;
    w->hour += game_hours;
    while (w->hour >= 24.0) { w->hour -= 24.0; w->day++; }
    int new_day = (w->day != prev_day);

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

        if (a->arrested_ticks > 0) {
            if (--a->arrested_ticks == 0) { a->jailed_for[0] = '\0'; a->sentence_total = 0; }
            set_mood_color(a);
            continue;
        }

        needs_decay(&a->needs, game_hours);

        if (a->action_progress <= 0.0) {
            a->action = utility_best_action(a, w);
            a->action_progress = ACTION_SECONDS;
            a->acted = 0;
            set_target(w, a);
        }
        move_toward(w, a, dt_seconds);
        if (arrived(a) && !a->acted) { execute_action(w, a); a->acted = 1; }
        a->action_progress -= dt_seconds;

        if (new_day) rel_decay_all(&a->rels, 1.0);

        set_mood_color(a);

        if (needs_is_dying(&a->needs)) {
            a->alive = 0; w->deaths++;
            char t[96];
            const char *reason = a->needs.hunger <= 0.01 ? "starvation" : "exhaustion";
            snprintf(t, sizeof(t), "%s died (%s)", a->name, reason);
            events_post(w, EV_DEATH, a->id, -1, (int)a->x, (int)a->y, 0.9, t);
        }
    }

    crime_tick(w);

    if (new_day) { economy_daily(w); factions_daily(w); }
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
    return rd == 1;
}
