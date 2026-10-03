/* sim.c — Emergent City C core (ported from the Python sim). */
#include "sim.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Personality ─────────────────────────────────────────────────────────── */
int pers_has(const Personality *p, uint32_t trait) { return (p->traits & trait) != 0; }

void personality_random(Personality *p, Rng *r) {
    p->o = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->c = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->e = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->a = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->n = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    /* sample 2..4 distinct behavioral traits */
    p->traits = 0;
    int k = rng_int_incl(r, 2, 4);
    for (int i = 0; i < k; i++)
        p->traits |= (1u << rng_int(r, TRAIT_COUNT));
}

double pers_work_ethic(const Personality *p) {
    double base = p->c;
    if (pers_has(p, TR_LAZY)) base -= 0.3;
    if (pers_has(p, TR_AMBITIOUS) || pers_has(p, TR_DISCIPLINED)) base += 0.25;
    return clampd(base, 0.0, 1.0);
}

double pers_social_drive(const Personality *p) {
    double base = p->e;
    if (pers_has(p, TR_AWKWARD)) base -= 0.2;
    if (pers_has(p, TR_CHARISMATIC)) base += 0.2;
    return clampd(base, 0.0, 1.0);
}

double pers_crime_propensity(const Personality *p) {
    double base = (1.0 - p->a) * 0.5 + p->n * 0.3;
    if (pers_has(p, TR_CRUEL)) base += 0.25;
    if (pers_has(p, TR_MANIPULATIVE)) base += 0.15;
    if (pers_has(p, TR_VENGEFUL)) base += 0.10;
    if (pers_has(p, TR_KIND)) base -= 0.30;
    if (pers_has(p, TR_RELIGIOUS)) base -= 0.15;
    return clampd(base, 0.0, 1.0);
}

double pers_faith(const Personality *p) {
    if (pers_has(p, TR_RELIGIOUS)) return 0.85;
    if (pers_has(p, TR_SKEPTICAL) || pers_has(p, TR_CYNICAL)) return 0.10;
    return 0.4 + (p->o - 0.5) * 0.2;
}

/* ── Needs ───────────────────────────────────────────────────────────────── */
void needs_decay(Needs *n, double hours) {
    n->hunger    = clampd(n->hunger    - HUNGER_DECAY    * hours, 0.0, 1.0);
    n->energy    = clampd(n->energy    - ENERGY_DECAY    * hours, 0.0, 1.0);
    /* safety drifts toward 0.5 (as in Python) */
    n->safety    = clampd(n->safety + (0.5 - n->safety) * 0.02 * hours, 0.0, 1.0);
    n->social    = clampd(n->social    - SOCIAL_DECAY    * hours, 0.0, 1.0);
    n->meaning   = clampd(n->meaning   - MEANING_DECAY   * hours, 0.0, 1.0);
    n->belonging = clampd(n->belonging - BELONGING_DECAY * hours, 0.0, 1.0);
}

int needs_is_critical(const Needs *n) {
    return n->hunger < NEED_CRITICAL || n->energy < NEED_CRITICAL ||
           n->safety < NEED_CRITICAL || n->social < NEED_CRITICAL ||
           n->meaning < NEED_CRITICAL || n->belonging < NEED_CRITICAL;
}
int needs_is_dying(const Needs *n) {
    return n->hunger <= 0.01 || n->energy <= 0.01;
}

/* ── Utility AI (ported from utility_ai.py) ──────────────────────────────── */
static double sq(double x) { return x * x; }

Action utility_best_action(const Agent *a, const World *w) {
    const Needs *n = &a->needs;
    const Personality *p = &a->pers;
    int hour = (int)w->hour;
    int night = world_is_night(w);
    double s[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) s[i] = 0.0;

    /* eat */
    double eat = sq(1.0 - n->hunger) * 2.0;
    if (n->hunger < NEED_CRITICAL) eat += 3.0;
    else if (n->money < MEAL_PRICE * 0.5) eat *= 0.4;
    s[A_EAT] = eat;

    /* sleep */
    double sleep = sq(1.0 - n->energy) * 2.5;
    if (night) sleep *= 1.5;
    if (hour < 6 || hour > 23) sleep *= 1.3;
    if (n->energy < NEED_CRITICAL) sleep += 2.0;
    s[A_SLEEP] = sleep;

    /* work (everyone has a workplace in this POC) */
    double work = 0.0;
    if (hour >= 8 && hour <= 18) {
        work = 0.8 + pers_work_ethic(p) * 0.6;
        if (n->money < LOW_MONEY) work += 0.6;
    }
    s[A_WORK] = work;

    /* socialize */
    double social = (1.0 - n->social) * (0.7 + pers_social_drive(p) * 0.6);
    if (night && hour > 21) social *= 0.6;
    s[A_SOCIALIZE] = social;

    /* drink */
    double drink = 0.0;
    if ((hour >= 18 && hour <= 24) || hour < 2) {
        drink = (1.0 - n->social) * 0.6 + p->e * 0.4;
        if (pers_has(p, TR_ALCOHOLIC)) drink += 0.6;
        if (n->money < DRINK_PRICE) drink *= 0.2;
    }
    s[A_DRINK] = drink;

    /* pray */
    double pray = 0.0;
    if ((1.0 - n->meaning) > 0.4 || n->safety < 0.4)
        pray = (1.0 - n->meaning) * pers_faith(p) * 1.2;
    s[A_PRAY] = pray;

    /* shop */
    double shop = 0.0;
    if (n->money > LUXURY_PRICE && n->belonging < 0.7)
        shop = 0.35 + (0.7 - n->belonging) * 0.5;
    s[A_SHOP] = shop;

    /* go home */
    double home = 0.0;
    if (night) home = 0.7;
    if (n->energy < 0.3) home += 0.5;
    s[A_GO_HOME] = home;

    /* commit crime */
    double crime = 0.0;
    double prop = pers_crime_propensity(p);
    if (n->money < LOW_MONEY && n->hunger < 0.5)
        crime = prop * 1.2 * (1.0 - n->money / (LOW_MONEY > 1.0 ? LOW_MONEY : 1.0));
    if (n->safety > 0.7) crime *= 0.7;
    s[A_CRIME] = crime;

    /* flee */
    double flee = 0.0;
    if (n->safety < 0.3) flee = (0.3 - n->safety) * 4.0;
    s[A_FLEE] = flee;

    /* wander fallback */
    s[A_WANDER] = 0.05;

    Action best = A_WANDER; double bestv = -1.0;
    for (int i = 0; i < A_COUNT; i++)
        if (s[i] > bestv) { bestv = s[i]; best = (Action)i; }
    return best;
}

const char *action_name(Action a) {
    static const char *names[A_COUNT] = {
        "eat", "sleep", "work", "socialize", "drink", "pray", "shop",
        "go_home", "crime", "flee", "wander"
    };
    return (a >= 0 && a < A_COUNT) ? names[a] : "?";
}
const char *tile_name(TileType t) {
    static const char *names[T_TYPE_COUNT] = {
        "grass", "road", "home", "shop", "work", "bar", "church"
    };
    return (t >= 0 && t < T_TYPE_COUNT) ? names[t] : "?";
}

/* ── World generation ────────────────────────────────────────────────────── */
void world_init(World *w, uint64_t seed) {
    memset(w, 0, sizeof(*w));
    rng_seed(&w->rng, seed, 0xCAFEu);
    Rng *r = &w->rng;
    for (int x = 0; x < WORLD_W; x++) {
        for (int y = 0; y < WORLD_H; y++) {
            /* Manhattan-ish grid of roads with building/grass blocks between. */
            if (x % 12 == 0 || y % 9 == 0) {
                w->tile[x][y] = T_ROAD;
            } else {
                double roll = rng_double(r);
                TileType t = T_GRASS;
                if (roll < 0.22) t = T_HOME;
                else if (roll < 0.30) t = T_SHOP;
                else if (roll < 0.37) t = T_WORK;
                else if (roll < 0.40) t = T_BAR;
                else if (roll < 0.42) t = T_CHURCH;
                w->tile[x][y] = (uint8_t)t;
            }
        }
    }
    w->hour = 8.0;
    w->day = 1;
}

/* nearest tile of a given type (cheap scan; fine for POC scale) */
static int nearest_tile(const World *w, int fx, int fy, TileType want, int *ox, int *oy) {
    int best = -1, bx = -1, by = -1;
    for (int x = 0; x < WORLD_W; x++)
        for (int y = 0; y < WORLD_H; y++)
            if (w->tile[x][y] == want) {
                int d = abs(x - fx) + abs(y - fy);
                if (best < 0 || d < best) { best = d; bx = x; by = y; }
            }
    if (best < 0) return 0;
    *ox = bx; *oy = by; return 1;
}

void world_populate(World *w, int n) {
    if (n > MAX_AGENTS) n = MAX_AGENTS;
    Rng *r = &w->rng;
    for (int i = 0; i < n; i++) {
        Agent *a = &w->agents[w->n_agents];
        memset(a, 0, sizeof(*a));
        a->id = w->n_agents;
        a->alive = 1;
        /* home = a random HOME tile */
        int hx = rng_int(r, WORLD_W), hy = rng_int(r, WORLD_H), tries = 0;
        while (w->tile[hx][hy] != T_HOME && tries < 200) {
            hx = rng_int(r, WORLD_W); hy = rng_int(r, WORLD_H); tries++;
        }
        a->home_x = hx; a->home_y = hy;
        a->x = hx; a->y = hy; a->tx = -1; a->ty = -1;
        a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
        a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
        a->needs.money = clampd(rng_gauss(r, START_MONEY_MEAN, START_MONEY_SD), 0.0, 1e9);
        personality_random(&a->pers, r);
        a->action = A_WANDER;
        a->action_progress = 0.0;
        w->n_agents++;
    }
}

int world_is_night(const World *w) {
    int h = (int)w->hour;
    return h < DAYTIME_START || h >= NIGHT_START;
}

/* pick a target tile for the chosen action */
static void set_target(World *w, Agent *a) {
    int ox, oy;
    switch (a->action) {
        case A_EAT:
        case A_SHOP:
            /* Starving: eat in place (no target) rather than walk to a shop. */
            if (a->action == A_EAT && a->needs.hunger < NEED_CRITICAL) { a->tx = -1; a->ty = -1; return; }
            if (nearest_tile(w, (int)a->x, (int)a->y, T_SHOP, &ox, &oy)) { a->tx = ox; a->ty = oy; return; }
            break;
        case A_SLEEP: case A_GO_HOME:
            a->tx = a->home_x; a->ty = a->home_y; return;
        case A_WORK:
            if (nearest_tile(w, (int)a->x, (int)a->y, T_WORK, &ox, &oy)) { a->tx = ox; a->ty = oy; return; }
            break;
        case A_DRINK:
            if (nearest_tile(w, (int)a->x, (int)a->y, T_BAR, &ox, &oy)) { a->tx = ox; a->ty = oy; return; }
            break;
        case A_PRAY:
            if (nearest_tile(w, (int)a->x, (int)a->y, T_CHURCH, &ox, &oy)) { a->tx = ox; a->ty = oy; return; }
            break;
        default: break;
    }
    /* wander / crime / flee / no building: random nearby target */
    a->tx = (int)clampd(a->x + rng_int_incl(&w->rng, -10, 10), 0, WORLD_W - 1);
    a->ty = (int)clampd(a->y + rng_int_incl(&w->rng, -10, 10), 0, WORLD_H - 1);
}

static int arrived(const Agent *a) {
    return a->tx < 0 || ((int)a->x == a->tx && (int)a->y == a->ty);
}

/* on-arrival action effect */
static void execute_action(World *w, Agent *a) {
    Needs *n = &a->needs;
    switch (a->action) {
        case A_EAT:
            if (n->money >= MEAL_PRICE) { n->money -= MEAL_PRICE; n->hunger = clampd(n->hunger + 0.6, 0, 1); }
            else if (n->hunger < NEED_CRITICAL) n->hunger = clampd(n->hunger + 0.3, 0, 1); /* charity */
            break;
        case A_SLEEP:
            n->energy = clampd(n->energy + 0.35, 0, 1);
            break;
        case A_GO_HOME:
            n->energy = clampd(n->energy + 0.08, 0, 1);
            break;
        case A_WORK:
            n->money += WAGE_PER_SHIFT; n->energy = clampd(n->energy - 0.1, 0, 1);
            n->meaning = clampd(n->meaning + 0.05, 0, 1);
            break;
        case A_SOCIALIZE:
            n->social = clampd(n->social + 0.2, 0, 1); n->belonging = clampd(n->belonging + 0.05, 0, 1);
            break;
        case A_DRINK:
            if (n->money >= DRINK_PRICE) { n->money -= DRINK_PRICE; n->social = clampd(n->social + 0.3, 0, 1); }
            break;
        case A_PRAY:
            n->meaning = clampd(n->meaning + 0.4, 0, 1); n->safety = clampd(n->safety + 0.1, 0, 1);
            break;
        case A_SHOP:
            if (n->money >= LUXURY_PRICE) { n->money -= LUXURY_PRICE; n->belonging = clampd(n->belonging + 0.2, 0, 1); }
            break;
        case A_CRIME:
            n->money += rng_range(&w->rng, 5.0, 40.0); w->crimes++;
            break;
        case A_FLEE:
            n->safety = clampd(n->safety + 0.05, 0, 1);
            break;
        default: break;
    }
}

static void move_toward(Agent *a, double dt) {
    if (a->tx < 0) return;
    a->move_progress += dt * AGENT_SPEED_TPS;
    while (a->move_progress >= 1.0 && !((int)a->x == a->tx && (int)a->y == a->ty)) {
        if ((int)a->x < a->tx) a->x += 1; else if ((int)a->x > a->tx) a->x -= 1;
        else if ((int)a->y < a->ty) a->y += 1; else if ((int)a->y > a->ty) a->y -= 1;
        a->move_progress -= 1.0;
    }
}

/* color agents by overall wellbeing: red (low) -> green (high) */
static void set_mood_color(Agent *a) {
    const Needs *n = &a->needs;
    double wb = (n->hunger * 1.2 + n->energy + n->safety * 1.1 +
                 n->social * 0.9 + n->meaning + n->belonging) / 6.2;
    wb = clampd(wb, 0.0, 1.0);
    a->r = (unsigned char)(230 * (1.0 - wb) + 120 * wb);
    a->g = (unsigned char)(90 * (1.0 - wb) + 220 * wb);
    a->b = (unsigned char)(80 * (1.0 - wb) + 130 * wb);
}

void world_tick(World *w, double dt_seconds) {
    double game_hours = dt_seconds * TIME_SCALE / 3600.0;
    int prev_day = w->day;
    w->hour += game_hours;
    while (w->hour >= 24.0) { w->hour -= 24.0; w->day++; }

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

        needs_decay(&a->needs, game_hours);

        if (a->action_progress <= 0.0) {
            a->action = utility_best_action(a, w);
            a->action_progress = ACTION_SECONDS;
            a->acted = 0;
            set_target(w, a);
        }
        move_toward(a, dt_seconds);
        if (arrived(a) && !a->acted) {
            execute_action(w, a);      /* once per decision, on arrival */
            a->acted = 1;
        }
        a->action_progress -= dt_seconds;

        set_mood_color(a);

        if (needs_is_dying(&a->needs)) { a->alive = 0; w->deaths++; }

        /* daily cost of living */
        if (w->day != prev_day) {
            double cost = RENT_PER_DAY + a->needs.money * UPKEEP_FRACTION;
            a->needs.money = clampd(a->needs.money - cost, 0.0, 1e9);
        }
    }
}
