/* world.c — world generation, population, tick orchestration, save/load. */
#include "sim.h"
#include "llm.h"
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

void world_init(World *w, uint64_t seed) {
    memset(w, 0, sizeof(*w));
    rng_seed(&w->rng, seed, 0xCAFEu);
    Rng *r = &w->rng;
    const double cx = WORLD_W * 0.5, cy = WORLD_H * 0.5;
    const double maxd = sqrt(cx*cx + cy*cy);

    /* 1. Base layer: grass, plus a meandering river down the east side. */
    for (int y = 0; y < WORLD_H; y++) {
        double rc = WORLD_W*0.72 + 10.0*sin(y*0.10) + 5.0*sin(y*0.31);
        for (int x = 0; x < WORLD_W; x++)
            w->tile[x][y] = (fabs(x - rc) < 1.6) ? T_WATER : T_GRASS;
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

    /* 3. A handful of parks (whole-ish blocks of greenery). */
    for (int p = 0; p < 6; p++) {
        int px = 3 + rng_int(r, WORLD_W - 11), py = 3 + rng_int(r, WORLD_H - 9);
        int pw = 4 + rng_int(r, 4), ph = 3 + rng_int(r, 3);
        for (int x = px; x < px+pw && x < WORLD_W; x++)
            for (int y = py; y < py+ph && y < WORLD_H; y++)
                if (w->tile[x][y] == T_GRASS) w->tile[x][y] = T_PARK;
    }

    /* 4. Fill lots with buildings on a downtown-to-fringe density gradient. */
    for (int x = 0; x < WORLD_W; x++)
        for (int y = 0; y < WORLD_H; y++) {
            if (w->tile[x][y] != T_GRASS) continue;
            if (w->n_buildings >= MAX_BUILDINGS) goto built;
            double dx = x - cx, dy = y - cy, d = sqrt(dx*dx + dy*dy) / maxd;
            if (rng_double(r) > district_fill(d)) continue;   /* leave as yard/grass */
            TileType t = district_building(r, d);
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
        a->sex = (unsigned char)rng_int(r, 2);
        a->spouse_id = a->mother_id = a->father_id = -1;
        a->n_children = 0; a->pregnant_ticks = 0;
        w->n_agents++;
    }
    assign_crime_roles(w);   /* career criminals, dealers, kingpins, users, a rare killer */
    factions_populate(w);    /* enlist members into gangs/cults so factions are real actors */
    economy_setup(w);        /* occupations, landlords/tenants */
    culture_setup(w);        /* faith, cultural group, language, schooling */
}

Agent *world_agent_by_id(World *w, int id) {
    for (int i = 0; i < w->n_agents; i++) if (w->agents[i].id == id) return &w->agents[i];
    return NULL;
}

/* Spawn + register one new citizen at (tx,ty) (God Mode). Returns id or -1. */
int world_spawn_agent(World *w, int tx, int ty) {
    if (w->n_agents >= MAX_AGENTS) return -1;
    Rng *r = &w->rng;
    Agent *a = &w->agents[w->n_agents];
    memset(a, 0, sizeof(*a));
    a->id = w->next_id++;
    a->alive = 1;
    snprintf(a->name, sizeof(a->name), "%s %s", FIRST_NAMES[rng_int(r, 40)], LAST_NAMES[rng_int(r, 28)]);
    a->age = (int)clampd(rng_gauss(r, 35, 14), 16, 90);
    a->home_id = random_building(w, T_HOME);
    a->x = (tx >= 0 && tx < WORLD_W) ? tx : rng_int(r, WORLD_W);
    a->y = (ty >= 0 && ty < WORLD_H) ? ty : rng_int(r, WORLD_H);
    a->tx = -1; a->ty = -1;
    double wr = rng_double(r);
    TileType wtype = wr < 0.6 ? T_WORK : wr < 0.85 ? T_SHOP : wr < 0.95 ? T_BAR : T_CHURCH;
    a->workplace_id = random_building(w, wtype);
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
    w->n_agents++;
    char t[96]; snprintf(t, sizeof(t), "%s appeared in the city", a->name);
    events_post(w, EV_BIRTH, a->id, -1, a->x, a->y, 0.4, t);
    return a->id;
}

/* ── kinship & births ───────────────────────────────────────────────────────
   The city marries fond, familiar, eligible couples and raises children from
   them.  Called once per game-day from world_tick. */

/* Append a newborn carrying the father's surname and the family's home. */
static int spawn_child(World *w, Agent *mum, Agent *dad) {
    if (w->n_agents >= MAX_AGENTS) return -1;
    Rng *r = &w->rng;
    Agent *a = &w->agents[w->n_agents];
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
    a->needs.hunger = a->needs.energy = a->needs.safety = 1.0;
    a->needs.social = a->needs.meaning = a->needs.belonging = 1.0;
    a->needs.money = 0;
    personality_random(&a->pers, r);
    a->action = A_WANDER;
    w->n_agents++;
    char t[112]; snprintf(t, sizeof(t), "%.20s and %.20s had a child, %.24s",
                          mum->name, dad->name, a->name);
    events_post(w, EV_BIRTH, a->id, mum->id, a->x, a->y, 0.7, t);
    return a->id;
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

void economy_setup(World *w) {
    Rng *r = &w->rng;
    for (int i = 0; i < w->n_agents; i++)
        if (w->agents[i].alive) w->agents[i].occupation = occ_for(w, &w->agents[i]);

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
    for (int i = 0; i < n0; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

        /* schooling: the young grow literate */
        if (a->age < EDU_ADULT_AGE && a->education < 1.0f)
            a->education = (float)clampd(a->education + EDU_YOUTH_GAIN, 0, 1);

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
    Agent *best = NULL; double bd = 1e18;
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->id == d->id || o->is_police || o->arrested_ticks > 0) continue;
        if (o->crime_role == CR_DEALER || o->crime_role == CR_KINGPIN) continue;
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
    Agent *best = NULL; double bd = 1e18;
    for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id || o->is_police || o->arrested_ticks > 0) continue;
        if (rival_faction_only && !(o->faction_id >= 0 && o->faction_id != a->faction_id)) continue;
        double dx = o->x - a->x, dy = o->y - a->y, dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; best = o; }
    }
    return best;
}

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
        case A_TREAT: b = building_nearest(w, (int)a->x, (int)a->y, T_SHOP); break;  /* pharmacy/clinic */
        case A_CRIME: {
            Agent *tgt = NULL;
            if (a->crime_role == CR_KINGPIN) { a->tx = -1; a->ty = -1; return; }  /* stay put, deal wholesale */
            else if (a->crime_role == CR_DEALER) tgt = a->drug_stock > 0 ? seek_customer(w, a) : seek_kingpin(w, a);
            else if (a->crime_role == CR_KILLER) tgt = seek_victim(w, a, 0);
            else if (a->faction_id >= 0) { tgt = seek_victim(w, a, 1); if (!tgt) tgt = seek_victim(w, a, 0); }
            else tgt = seek_victim(w, a, 0);
            if (tgt) { a->tx = (int)tgt->x; a->ty = (int)tgt->y; return; }
            if (a->crime_role == CR_CAREER) b = building_nearest(w, (int)a->x, (int)a->y, T_HOME);  /* go burgle */
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
        case A_WORK:    /* the educated command better pay (human capital) */
                        n->money += WAGE_PER_SHIFT * w->econ.wage_mult * (0.7 + a->education * 0.6);
                        n->energy = clampd(n->energy - 0.1, 0, 1);
                        n->meaning = clampd(n->meaning + 0.05, 0, 1);
                        a->reputation = (float)clampd(a->reputation + 0.004, -1, 1);
                        break;
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
        case A_TREAT:
            if (n->money >= TREAT_COST) { n->money -= TREAT_COST; a->injury = (float)clampd(a->injury - 0.6, 0, 2); }
            else a->injury = (float)clampd(a->injury - 0.2, 0, 2);   /* self-care if you can't pay */
            n->safety = clampd(n->safety + 0.1, 0, 1);
            break;
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
    double game_hours = dt_seconds * TIME_SCALE / 3600.0;
    int prev_day = w->day;
    w->hour += game_hours;
    while (w->hour >= 24.0) { w->hour -= 24.0; w->day++; }
    int new_day = (w->day != prev_day);

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;

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

    if (new_day) { economy_daily(w); factions_daily(w); crime_daily(w); jail_tick(w); kinship_daily(w); law_daily(w); culture_daily(w); danger_decay(w); }
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
