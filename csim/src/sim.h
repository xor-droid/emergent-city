/* sim.h — Emergent City C port: full data model + simulation API (no graphics deps).
 *
 * Ports the Python sim's data-oriented core to C: Needs, Personality, UtilityAI,
 * Relationships, Memory, Crime+Wanted+Jail, Factions, Economy, Events, A*
 * pathfinding, and the World/agent/time loop. Compiles with C + libm; renderers
 * (ui.c over the gfx.h backends) is a thin layer on top.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include "rng.h"

/* ── World ───────────────────────────────────────────────────────────────── */
#define WORLD_W 120
#define WORLD_H 90
#define MAX_AGENTS 400
#define MAX_BUILDINGS 4096   /* dense districted worldgen registers many buildings */
#define MAX_FACTIONS 32
#define EVENT_RING 256          /* recent world events kept for the feed */
#define MAX_RELATIONS 40        /* per-agent relationship ledger (LRU-ish) */
#define AGENT_MEMORY 8          /* per-agent recent memories */

/* ── Needs decay per in-game hour ────────────────────────────────────────── */
#define HUNGER_DECAY     0.045
#define ENERGY_DECAY     0.035
#define SOCIAL_DECAY     0.020
#define MEANING_DECAY    0.015
#define BELONGING_DECAY  0.012
#define NEED_CRITICAL    0.20
#define NEED_LOW         0.40

/* ── Time ────────────────────────────────────────────────────────────────── */
#define SECONDS_PER_HOUR 30.0
#define TIME_SCALE       (3600.0 / SECONDS_PER_HOUR)
#define DAYTIME_START    6
#define NIGHT_START      20

/* ── Economy ─────────────────────────────────────────────────────────────── */
#define MEAL_PRICE       5.0
#define DRINK_PRICE      8.0
#define LUXURY_PRICE     30.0
#define WAGE_PER_SHIFT   15.0
#define LOW_MONEY        20.0
#define START_MONEY_MEAN 100.0
#define START_MONEY_SD   50.0
#define RENT_PER_DAY     5.0
#define UPKEEP_FRACTION  0.35

/* ── Personality / movement ──────────────────────────────────────────────── */
#define BIG5_MEAN  0.5
#define BIG5_SD    0.18
#define AGENT_SPEED_TPS 2.0
#define ACTION_SECONDS  6.0

/* ── Relationships ───────────────────────────────────────────────────────── */
#define FRIENDSHIP_AFFINITY 0.40
#define RIVALRY_AFFINITY    (-0.30)
#define RELATIONSHIP_DECAY  0.010
#define INTERACT_FAMILIARITY 0.05

/* ── Crime / wanted / jail ───────────────────────────────────────────────── */
#define WITNESS_RADIUS       5
#define ARREST_DURATION      600     /* jail sentence base (ticks) */
#define WANTED_DURATION      2400    /* lie-low window (ticks) */
#define POLICE_ARREST_RADIUS 2
#define POLICE_ARREST_CHANCE 0.12
#define WANTED_CRIME_SUPPRESSION 0.25

/* ── Crime roles & the drug trade ────────────────────────────────────────── */
enum { CR_CITIZEN, CR_CAREER, CR_DEALER, CR_KINGPIN, CR_KILLER };  /* Agent.crime_role */
#define KILLER_CHANCE 0.006   /* tiny chance a spawned agent is a latent serial killer */
/* crime kinds, for the per-kind tally (World.crime_kind) */
enum { CK_THEFT, CK_BURGLARY, CK_ROBBERY, CK_EXTORTION, CK_VANDALISM, CK_ARSON,
       CK_ASSAULT, CK_RIOT, CK_DEALING, CK_TRAFFICKING, CK_MURDER, CK_COUNT };

/* ── Injury / treatment / jail violence ──────────────────────────────────── */
#define ASSAULT_INJURY   0.35    /* injury added by a street assault */
#define JAIL_BEATING     0.45    /* harsher inside */
#define INJURY_FATAL     1.0     /* injury >= this -> dies of wounds */
#define TREAT_COST       20.0    /* paying to get patched up */
#define JAIL_GANGS       3       /* number of distinct jail gangs */
#define JAIL_VIOLENCE_P  0.45    /* chance a jail-gang member picks a fight (per game day) */
#define CAREER_FRACTION   0.07    /* of high-propensity agents who turn pro */
#define USER_FRACTION     0.12    /* share of citizens who become drug users */
#define DRUG_STREET_PRICE 14.0    /* dealer -> user, per unit (scaled by addiction) */
#define DRUG_WHOLESALE    6.0     /* kingpin -> dealer, per unit */
#define DRUG_BATCH        8       /* units per import / wholesale buy */
#define SELL_RADIUS       6       /* how near a user must be for a street sale */
#define TURF_RADIUS       16      /* dealer territory radius */
#define RETALIATE_RADIUS  10      /* immediate retaliation range */
/* career-criminal escalation thresholds (crimes_committed) */
#define ESCALATE_T1 4
#define ESCALATE_T2 10
#define ESCALATE_T3 22

/* ── Economy: markets, land, credit ────────────────────────────────────────── */
#define RENT_TO_LANDLORD  6.0     /* daily rent a tenant pays their landlord */
#define LOAN_AMOUNT       25.0    /* emergency micro-loan when destitute */
#define DEBT_CEILING      200.0   /* no more credit past this */
#define DAILY_INTEREST    0.03    /* interest accrued on outstanding debt per day */

/* Agent occupations (derived from workplace). */
enum { OCC_NONE, OCC_LABORER, OCC_SHOPKEEP, OCC_BARKEEP,
       OCC_CLERGY, OCC_OFFICER, OCC_COUNT };

/* ── Marriage / kinship / births ───────────────────────────────────────────── */
#define MARRY_MIN_AGE    20      /* minimum age to wed */
#define MARRY_AFFINITY   0.45    /* affinity with a partner needed to wed */
#define MARRY_FAMILIAR   0.4     /* familiarity needed to wed */
#define MARRY_CHANCE     0.22    /* daily chance a willing, eligible couple weds */
#define FERTILE_MAX_AGE  45      /* mothers stop conceiving past this */
#define CONCEIVE_CHANCE  0.16    /* daily chance a married fertile couple conceives */
#define GESTATION_DAYS   4       /* game-days of pregnancy before a birth */
#define MAX_CHILDREN     6       /* soft cap per mother */

/* ── Factions ────────────────────────────────────────────────────────────── */
#define FACTION_RADIUS 8

/* Behaviorally-relevant unique traits, as a bitmask. */
enum {
    TR_CRUEL = 1 << 0, TR_MANIPULATIVE = 1 << 1, TR_VENGEFUL = 1 << 2,
    TR_KIND = 1 << 3, TR_RELIGIOUS = 1 << 4, TR_AWKWARD = 1 << 5,
    TR_CHARISMATIC = 1 << 6, TR_LAZY = 1 << 7, TR_AMBITIOUS = 1 << 8,
    TR_DISCIPLINED = 1 << 9, TR_SKEPTICAL = 1 << 10, TR_CYNICAL = 1 << 11,
    TR_ALCOHOLIC = 1 << 12, TR_BRAVE = 1 << 13,
    TRAIT_COUNT = 14
};

typedef enum {
    T_GRASS, T_ROAD, T_HOME, T_SHOP, T_WORK, T_BAR, T_CHURCH, T_POLICE,
    T_PARK, T_WATER, T_TYPE_COUNT
} TileType;

typedef enum {
    A_EAT, A_SLEEP, A_WORK, A_SOCIALIZE, A_DRINK, A_PRAY, A_SHOP,
    A_GO_HOME, A_CRIME, A_FLEE, A_PATROL, A_TREAT, A_WANDER, A_COUNT
} Action;

/* World event kinds (drive the event feed). */
typedef enum {
    EV_CRIME, EV_CRIME_FAILED, EV_ARREST, EV_WANTED, EV_LAID_LOW,
    EV_DEATH, EV_BIRTH, EV_FRIENDS, EV_QUARREL, EV_FACTION, EV_HARDSHIP,
    EV_MARRIAGE,
    EV_KIND_COUNT
} EventKind;

typedef struct {
    double hunger, energy, safety, social, meaning, belonging, money;
} Needs;

typedef struct {
    double o, c, e, a, n;   /* Big Five */
    uint32_t traits;        /* TR_* bitmask */
} Personality;

/* One remembered other agent. */
typedef struct {
    int other_id;
    double affinity;        /* -1..1 */
    double familiarity;     /* 0..1 */
    int announced_friend;   /* already announced a friendship with them */
    int announced_rival;    /* already announced a rivalry with them */
} Relation;

typedef struct {
    Relation rel[MAX_RELATIONS];
    int n;
} Relationships;

typedef struct {
    char text[48];
    double importance;
} Memory;

typedef struct {
    Memory items[AGENT_MEMORY];
    int head, n;            /* ring buffer */
} MemoryBook;

typedef struct {
    int id, alive;
    char name[32];
    int age;
    double x, y;
    int tx, ty;
    int home_id;            /* building index, or -1 */
    int workplace_id;       /* building index, or -1 */
    Needs needs;
    Personality pers;
    Relationships rels;
    MemoryBook mem;
    Action action;
    double action_progress;
    int acted;
    double move_progress;
    char facing;

    int is_police;
    int faction_id;         /* -1 if none */
    int llm_pending;        /* an LLM decision request is in flight */
    int broke_flagged;      /* destitute milestone already announced */

    /* ── crime roles & the drug trade ── */
    unsigned char crime_role;   /* CR_* */
    int   crimes_committed;     /* successful crimes (notoriety + escalation tier) */
    float crime_skill;          /* 0..1 competence: better success, harder to catch */
    float addiction;            /* 0..1 drug dependence (users) */
    int   drug_stock;           /* dealer/kingpin inventory, units */
    int   dealer_id;            /* a user's regular dealer (-1 none) */
    int   turf_x, turf_y;       /* a dealer's territory centre */
    float injury;               /* 0=healthy .. 1=fatal (from assaults) */
    unsigned char jail_gang;    /* 0=none, else jail-gang id (while incarcerated) */
    float reputation;           /* -1 disreputable .. +1 esteemed (civic standing) */
    unsigned char status;       /* 0..4 social tier, recomputed daily */

    /* ── kinship & family ── */
    unsigned char sex;          /* 0=female, 1=male */
    int   spouse_id;            /* -1 none */
    int   mother_id, father_id; /* -1 unknown */
    int   n_children;
    int   pregnant_ticks;       /* >0 = gestating (days remaining); females only */

    /* ── economy ── */
    unsigned char occupation;   /* OCC_* derived from workplace */
    double debt;                /* outstanding credit (accrues daily interest) */

    int wanted;
    int wanted_ticks;
    char wanted_for[16];
    int arrested_ticks;     /* jail time remaining (frozen while >0) */
    int sentence_total;
    char jailed_for[16];

    char last_thought[64];
    char last_dialogue[80];

    unsigned char r, g, b;  /* render color (mood) */
} Agent;

typedef struct {
    int id;
    TileType type;
    int x, y, w, h;
    int capacity;
    int n_residents;        /* for homes */
    int n_workers;          /* for workplaces */
    int owner_id;           /* landlord who owns this building (-1 none) */
} Building;

/* City-wide economy: markets, prevailing wages. */
typedef struct {
    double goods_price;     /* luxury/drink price multiplier */
    double wage_mult;       /* prevailing wage multiplier */
} Economy;

typedef struct {
    int id, active;
    char name[32];
    int is_cult;            /* 0 gang, 1 cult */
    int leader_id;
    int members;
    double treasury;
    unsigned char r, g, b;
} Faction;

typedef struct {
    EventKind kind;
    int actor_id, target_id;
    int x, y;
    double importance;
    char text[96];
} WorldEvent;

typedef struct {
    uint8_t tile[WORLD_W][WORLD_H];     /* TileType */
    uint8_t danger_[WORLD_W][WORLD_H];  /* 0..255 danger (crime heat) */

    Agent agents[MAX_AGENTS];
    int n_agents;
    int next_id;

    Building buildings[MAX_BUILDINGS];
    int n_buildings;

    Faction factions[MAX_FACTIONS];
    int n_factions;

    WorldEvent events[EVENT_RING];      /* ring buffer */
    int ev_head, ev_count;

    double hour;    /* 0..24 */
    int day;
    Rng rng;

    /* stats for HUD / headless */
    int deaths, crimes;
    int crime_kind[CK_COUNT];   /* per-kind crime tally */

    Economy econ;               /* city-wide markets, food, wages */
} World;

/* ── Personality ─────────────────────────────────────────────────────────── */
void   personality_random(Personality *p, Rng *r);
double pers_work_ethic(const Personality *p);
double pers_social_drive(const Personality *p);
double pers_crime_propensity(const Personality *p);
double pers_faith(const Personality *p);
int    pers_has(const Personality *p, uint32_t trait);

/* ── Needs ───────────────────────────────────────────────────────────────── */
void needs_decay(Needs *n, double hours);
int  needs_is_critical(const Needs *n);
int  needs_is_dying(const Needs *n);

/* ── Relationships ───────────────────────────────────────────────────────── */
Relation *rel_get(Relationships *rs, int other_id);        /* NULL if unknown */
Relation *rel_touch(Relationships *rs, int other_id);      /* get-or-create */
void      rel_adjust(Relationships *rs, int other_id, double d_aff);
void      rel_decay_all(Relationships *rs, double days);
int       rel_is_friend(const Relationships *rs, int other_id);

/* ── Memory ──────────────────────────────────────────────────────────────── */
void mem_add(MemoryBook *m, const char *text, double importance);

/* ── Utility AI ──────────────────────────────────────────────────────────── */
Action      utility_best_action(const Agent *a, const World *w);
const char *action_name(Action a);
const char *tile_name(TileType t);

/* ── Events ──────────────────────────────────────────────────────────────── */
void             events_post(World *w, EventKind k, int actor, int target,
                             int x, int y, double importance, const char *text);
const WorldEvent *events_recent(const World *w, int i); /* i=0 newest; NULL past end */
const char       *event_kind_name(EventKind k);

/* ── Crime / wanted / jail ───────────────────────────────────────────────── */
void crime_attempt(World *w, Agent *perp, Agent *target, const char *kind);
void crime_tick(World *w);        /* police hunting + lie-low cooldown */
int  crime_jailed_count(const World *w);
int  crime_wanted_count(const World *w);
double crime_cooldown_frac(const Agent *a);
const char *crime_role_name(int role);
const char *crime_kind_name(int i);
void assign_crime_roles(World *w);   /* post-populate: pick career/dealer/kingpin/killer/users */
void crime_daily(World *w);          /* role mobility (emergence) + immigration, on day change */
void jail_tick(World *w);            /* jail gangs + shankings, on day change */
const char *jail_gang_name(int g);
const char *status_title(const Agent *a);   /* honorific from status/role/reputation */
void kinship_daily(World *w);               /* courtship -> marriage, pregnancy -> birth */
void economy_setup(World *w);               /* assign occupations + landlords, seed the larder */
int  count_properties(const World *w, int owner_id);  /* homes a landlord owns */
const char *occupation_name(unsigned char occ);

/* ── Factions ────────────────────────────────────────────────────────────── */
void factions_seed(World *w);
void factions_daily(World *w);
int  factions_raise(World *w, int is_cult, int cx, int cy);  /* returns faction id or -1 */

/* ── Economy ─────────────────────────────────────────────────────────────── */
void economy_daily(World *w);     /* cost of living, wages settle */

/* ── Pathing (A*) ────────────────────────────────────────────────────────── */
int  tile_walkable(const World *w, int x, int y);
/* step one tile from (fx,fy) toward (tx,ty); writes next (*nx,*ny). 1 if moved. */
int  path_step(const World *w, int fx, int fy, int tx, int ty, int *nx, int *ny);

/* ── Buildings ───────────────────────────────────────────────────────────── */
int  building_nearest(const World *w, int fx, int fy, TileType type);
Building *building_get(World *w, int id);

/* ── World ───────────────────────────────────────────────────────────────── */
void world_init(World *w, uint64_t seed);
void world_populate(World *w, int n);
void world_tick(World *w, double dt_seconds);
int  world_is_night(const World *w);
int  world_alive(const World *w);
Agent *world_agent_at(World *w, int tx, int ty, double radius);
Agent *world_agent_by_id(World *w, int id);
int  world_spawn_agent(World *w, int tx, int ty);   /* returns new agent id, or -1 */

/* ── .env loader (python-dotenv-style; does not override existing env) ──────── */
void dotenv_load(const char *path);     /* load one file if it exists */
void dotenv_autoload(void);             /* try CSIM_ENV, ./.env, ../.env, ../../.env */

/* ── Save / load (JSON via cJSON if available; no-op stubs otherwise) ─────── */
int  world_save(const World *w, const char *path);   /* 1 ok, 0 fail */
int  world_load(World *w, const char *path);

#endif /* SIM_H */
