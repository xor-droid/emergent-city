/* sim.c — core leaf systems: personality, needs, relationships, memory,
 * utility AI, events, pathing, buildings, name tables. */
#include "sim.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static double sq(double x) { return x * x; }

/* ── Personality ─────────────────────────────────────────────────────────── */
int pers_has(const Personality *p, uint32_t trait) { return (p->traits & trait) != 0; }

void personality_random(Personality *p, Rng *r) {
    p->o = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->c = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->e = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->a = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->n = clampd(rng_gauss(r, BIG5_MEAN, BIG5_SD), 0.05, 0.95);
    p->traits = 0;
    int k = rng_int_incl(r, 2, 4);
    for (int i = 0; i < k; i++) p->traits |= (1u << rng_int(r, TRAIT_COUNT));
}
double pers_work_ethic(const Personality *p) {
    double b = p->c;
    if (pers_has(p, TR_LAZY)) b -= 0.3;
    if (pers_has(p, TR_AMBITIOUS) || pers_has(p, TR_DISCIPLINED)) b += 0.25;
    return clampd(b, 0.0, 1.0);
}
double pers_social_drive(const Personality *p) {
    double b = p->e;
    if (pers_has(p, TR_AWKWARD)) b -= 0.2;
    if (pers_has(p, TR_CHARISMATIC)) b += 0.2;
    return clampd(b, 0.0, 1.0);
}
double pers_crime_propensity(const Personality *p) {
    double b = (1.0 - p->a) * 0.5 + p->n * 0.3;
    if (pers_has(p, TR_CRUEL)) b += 0.25;
    if (pers_has(p, TR_MANIPULATIVE)) b += 0.15;
    if (pers_has(p, TR_VENGEFUL)) b += 0.10;
    if (pers_has(p, TR_KIND)) b -= 0.30;
    if (pers_has(p, TR_RELIGIOUS)) b -= 0.15;
    return clampd(b, 0.0, 1.0);
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
int needs_is_dying(const Needs *n) { return n->hunger <= 0.01 || n->energy <= 0.01; }

/* ── Relationships ───────────────────────────────────────────────────────── */
Relation *rel_get(Relationships *rs, int other_id) {
    for (int i = 0; i < rs->n; i++) if (rs->rel[i].other_id == other_id) return &rs->rel[i];
    return NULL;
}
Relation *rel_touch(Relationships *rs, int other_id) {
    Relation *r = rel_get(rs, other_id);
    if (r) return r;
    if (rs->n < MAX_RELATIONS) {
        r = &rs->rel[rs->n++];
    } else {
        /* evict the weakest (lowest |affinity|) */
        int worst = 0;
        for (int i = 1; i < rs->n; i++)
            if (fabs(rs->rel[i].affinity) < fabs(rs->rel[worst].affinity)) worst = i;
        r = &rs->rel[worst];
    }
    r->other_id = other_id; r->affinity = 0.0; r->familiarity = 0.0; r->announced_friend = 0;
    return r;
}
void rel_adjust(Relationships *rs, int other_id, double d_aff) {
    Relation *r = rel_touch(rs, other_id);
    r->affinity = clampd(r->affinity + d_aff, -1.0, 1.0);
    r->familiarity = clampd(r->familiarity + INTERACT_FAMILIARITY, 0.0, 1.0);
}
void rel_decay_all(Relationships *rs, double days) {
    for (int i = 0; i < rs->n; i++) {
        double a = rs->rel[i].affinity;
        double d = RELATIONSHIP_DECAY * days;
        if (a > 0) a = a - d < 0 ? 0 : a - d;
        else if (a < 0) a = a + d > 0 ? 0 : a + d;
        rs->rel[i].affinity = a;
    }
}
int rel_is_friend(const Relationships *rs, int other_id) {
    for (int i = 0; i < rs->n; i++)
        if (rs->rel[i].other_id == other_id)
            return rs->rel[i].affinity >= FRIENDSHIP_AFFINITY;
    return 0;
}

/* ── Memory (ring) ───────────────────────────────────────────────────────── */
void mem_add(MemoryBook *m, const char *text, double importance) {
    Memory *slot = &m->items[m->head];
    strncpy(slot->text, text, sizeof(slot->text) - 1);
    slot->text[sizeof(slot->text) - 1] = '\0';
    slot->importance = importance;
    m->head = (m->head + 1) % AGENT_MEMORY;
    if (m->n < AGENT_MEMORY) m->n++;
}

/* ── Utility AI ──────────────────────────────────────────────────────────── */
Action utility_best_action(const Agent *a, const World *w) {
    const Needs *n = &a->needs;
    const Personality *p = &a->pers;
    int hour = (int)w->hour;
    int night = world_is_night(w);
    double s[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) s[i] = 0.0;

    double eat = sq(1.0 - n->hunger) * 2.0;
    if (n->hunger < NEED_CRITICAL) eat += 3.0;
    else if (n->money < MEAL_PRICE * 0.5) eat *= 0.4;
    s[A_EAT] = eat;

    double sleep = sq(1.0 - n->energy) * 2.5;
    if (night) sleep *= 1.5;
    if (hour < 6 || hour > 23) sleep *= 1.3;
    if (n->energy < NEED_CRITICAL) sleep += 2.0;
    s[A_SLEEP] = sleep;

    double work = 0.0;
    if (hour >= 8 && hour <= 18 && a->workplace_id != -1) {
        work = 0.8 + pers_work_ethic(p) * 0.6;
        if (n->money < LOW_MONEY) work += 0.6;
    }
    s[A_WORK] = work;

    double social = (1.0 - n->social) * (0.7 + pers_social_drive(p) * 0.6);
    if (night && hour > 21) social *= 0.6;
    s[A_SOCIALIZE] = social;

    double drink = 0.0;
    if ((hour >= 18 && hour <= 24) || hour < 2) {
        drink = (1.0 - n->social) * 0.6 + p->e * 0.4;
        if (pers_has(p, TR_ALCOHOLIC)) drink += 0.6;
        if (n->money < DRINK_PRICE) drink *= 0.2;
    }
    s[A_DRINK] = drink;

    double pray = 0.0;
    if ((1.0 - n->meaning) > 0.4 || n->safety < 0.4)
        pray = (1.0 - n->meaning) * pers_faith(p) * 1.2;
    s[A_PRAY] = pray;

    double shop = 0.0;
    if (n->money > LUXURY_PRICE && n->belonging < 0.7)
        shop = 0.35 + (0.7 - n->belonging) * 0.5;
    s[A_SHOP] = shop;

    double home = 0.0;
    if (night) home = 0.7;
    if (n->energy < 0.3) home += 0.5;
    s[A_GO_HOME] = home;

    double crime = 0.0;
    double prop = pers_crime_propensity(p);
    if (n->money < LOW_MONEY && n->hunger < 0.5)
        crime = prop * 1.2 * (1.0 - n->money / (LOW_MONEY > 1.0 ? LOW_MONEY : 1.0));
    if (a->faction_id != -1 && night) crime += 0.3 * prop;
    if (n->safety > 0.7) crime *= 0.7;
    if (a->wanted) crime *= WANTED_CRIME_SUPPRESSION;
    s[A_CRIME] = crime;

    double patrol = 0.0;
    if (a->is_police && hour >= 6 && hour <= 22) patrol = 1.5;
    s[A_PATROL] = patrol;

    double flee = 0.0;
    if (n->safety < 0.3) flee = (0.3 - n->safety) * 4.0;
    s[A_FLEE] = flee;

    s[A_WANDER] = 0.05;

    Action best = A_WANDER; double bestv = -1.0;
    for (int i = 0; i < A_COUNT; i++) if (s[i] > bestv) { bestv = s[i]; best = (Action)i; }
    return best;
}

const char *action_name(Action a) {
    static const char *names[A_COUNT] = {
        "eat","sleep","work","socialize","drink","pray","shop",
        "go_home","crime","flee","patrol","wander"
    };
    return (a >= 0 && a < A_COUNT) ? names[a] : "?";
}
const char *tile_name(TileType t) {
    static const char *names[T_TYPE_COUNT] = {
        "grass","road","home","shop","work","bar","church","police"
    };
    return (t >= 0 && t < T_TYPE_COUNT) ? names[t] : "?";
}

/* ── Events (ring buffer) ────────────────────────────────────────────────── */
void events_post(World *w, EventKind k, int actor, int target,
                 int x, int y, double importance, const char *text) {
    WorldEvent *e = &w->events[w->ev_head];
    e->kind = k; e->actor_id = actor; e->target_id = target;
    e->x = x; e->y = y; e->importance = importance;
    strncpy(e->text, text, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    w->ev_head = (w->ev_head + 1) % EVENT_RING;
    if (w->ev_count < EVENT_RING) w->ev_count++;
}
const WorldEvent *events_recent(const World *w, int i) {
    if (i < 0 || i >= w->ev_count) return NULL;
    int idx = (w->ev_head - 1 - i + EVENT_RING * 2) % EVENT_RING;
    return &w->events[idx];
}
const char *event_kind_name(EventKind k) {
    static const char *names[EV_KIND_COUNT] = {
        "crime","crime_failed","arrest","wanted","laid_low",
        "death","birth","friends","quarrel","faction","hardship"
    };
    return (k >= 0 && k < EV_KIND_COUNT) ? names[k] : "?";
}

/* ── Pathing (greedy step with sidestep) ─────────────────────────────────── */
int tile_walkable(const World *w, int x, int y) {
    (void)w;
    return x >= 0 && x < WORLD_W && y >= 0 && y < WORLD_H;  /* all in-bounds walkable */
}
int path_step(const World *w, int fx, int fy, int tx, int ty, int *nx, int *ny) {
    (void)w;
    if (fx == tx && fy == ty) { *nx = fx; *ny = fy; return 0; }
    int dx = (tx > fx) - (tx < fx);
    int dy = (ty > fy) - (ty < fy);
    /* prefer the larger axis first for a diagonal-ish walk */
    if (abs(tx - fx) >= abs(ty - fy) && dx) { *nx = fx + dx; *ny = fy; }
    else if (dy) { *nx = fx; *ny = fy + dy; }
    else { *nx = fx + dx; *ny = fy; }
    return 1;
}

/* ── Buildings ───────────────────────────────────────────────────────────── */
Building *building_get(World *w, int id) {
    if (id < 0 || id >= w->n_buildings) return NULL;
    return &w->buildings[id];
}
int building_nearest(const World *w, int fx, int fy, TileType type) {
    int best = -1, bestd = 0;
    for (int i = 0; i < w->n_buildings; i++) {
        if (w->buildings[i].type != type) continue;
        int d = abs(w->buildings[i].x - fx) + abs(w->buildings[i].y - fy);
        if (best < 0 || d < bestd) { best = i; bestd = d; }
    }
    return best;
}

/* ── .env loader (does not override already-set env vars) ─────────────────── */
static void strip(char *s) {
    char *p = s; while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r' || s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = '\0';
}
void dotenv_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        strip(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        strip(key); strip(val);
        /* drop surrounding quotes */
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') || (val[0] == '\'' && val[vl-1] == '\''))) {
            val[vl-1] = '\0'; val++;
        }
        if (key[0]) setenv(key, val, 0);  /* 0 = don't overwrite existing */
    }
    fclose(f);
}
void dotenv_autoload(void) {
    const char *explicit_path = getenv("CSIM_ENV");
    if (explicit_path && explicit_path[0]) { dotenv_load(explicit_path); return; }
    /* search upward: run from build/, csim/, or the project root */
    const char *cands[] = {".env", "../.env", "../../.env", "../../../.env"};
    for (int i = 0; i < 4; i++) {
        FILE *f = fopen(cands[i], "r");
        if (f) { fclose(f); dotenv_load(cands[i]); return; }
    }
}
