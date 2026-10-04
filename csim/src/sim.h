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
#define PATH_MAX 256            /* max cached path length per agent (re-chunks if longer) */
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
#define UPKEEP_FRACTION  0.15

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
#define CHILD_COST        3.0     /* daily upkeep a dependent child costs its parents (--child-cost) */
#define DEBT_CEILING      200.0   /* no more credit past this */
#define DAILY_INTEREST    0.03    /* interest accrued on outstanding debt per day */

/* Agent occupations (derived from workplace). */
enum { OCC_NONE, OCC_LABORER, OCC_SHOPKEEP, OCC_BARKEEP,
       OCC_CLERGY, OCC_OFFICER, OCC_COUNT };

/* ── Knowledge / technology / education ────────────────────────────────────── */
/* A short prerequisite chain: tech i needs `theory_req` city theories AND tech i-1,
   then an education/intellect-weighted discovery roll; once found it diffuses
   (adoption 0..1) and its benefit scales with adoption. */
enum { TECH_WRITING, TECH_TOOLING, TECH_MEDICINE, TECH_BANKING, TECH_PRINTING, TECH_CIVICS, TECH_COUNT };
#define RESEARCH_RATE    0.05   /* city research per unit of scholarship per day (default) */
#define THEORY_BASE      8.0    /* research for the 1st theory; cost rises with each */
#define THEORY_GROWTH    1.6    /* per-theory cost multiplier on the base */
#define DISCOVERY_BASE   0.25   /* daily discovery chance, scaled by top scholars' aptitude */
#define ADOPT_RATE       0.06   /* daily diffusion of a discovered tech toward full adoption */
#define CLERGY_SCHOLAR   2.0    /* clergy count double toward scholarship (monastic learning) */
#define CRAFT_BONUS      1.0    /* default: how much craft+education lift a worker's pay (--production) */
#define CRAFT_GAIN       0.004  /* craft learned per work shift (diminishing, x aptitude) */

/* ── Culture: religion, cultural group, language, education ─────────────────── */
enum { FAITH_NONE, FAITH_ORTHODOX, FAITH_REFORMED, FAITH_OLD, FAITH_MYSTIC, FAITH_COUNT };
enum { CUL_HARBOR, CUL_HILL, CUL_OLDTOWN, CUL_NEWCOMER, CUL_COUNT };
enum { LANG_COMMON, LANG_HIGH, LANG_COASTAL, LANG_OLD, LANG_COUNT };
#define CONVERT_CHANCE    0.08   /* daily chance a searching soul adopts a devout friend's faith */
#define ASSIMILATE_CHANCE 0.05   /* daily chance a minority-language agent picks up the Common tongue */
#define EDU_PER_YEAR      0.035   /* schooling gained per year of childhood (scales with aging pace) */
#define EDU_ADULT_AGE     18      /* schooling stops counting past this age */

/* ── Life cycle: aging & mortality ─────────────────────────────────────────── */
#define YEARS_PER_DAY   2.0    /* life-years per game-day (default; env CSIM_YEARS_PER_DAY) */
#define AGE_WORK        16     /* minimum age to work / commit crime / drink */
#define AGE_ADULT       18     /* legal adult */
#define AGE_CHILD_MAX   12     /* 0..12 = child */
#define AGE_ELDER       65     /* 65+ = elder */
#define AGE_MORTALITY   60     /* old-age death risk begins to climb here */
#define AGE_MAXLIFE     100    /* hard upper bound */
#define POP_TARGET      150    /* living-population the city immigrates toward */
#define FAMILY_SHARE    0.40   /* fraction of immigrant arrivals that are young families */
#define FAMILY_KIDS_MIN 1      /* kids per immigrant family (min) */
#define FAMILY_KIDS_MAX 3      /* kids per immigrant family (max) */
enum { LS_CHILD, LS_YOUTH, LS_ADULT, LS_ELDER };
#define IS_MINOR(a) ((a)->age < AGE_WORK)

/* ── Marriage / kinship / births ───────────────────────────────────────────── */
#define MARRY_MIN_AGE    20      /* minimum age to wed */
#define MARRY_AFFINITY   0.35    /* affinity with a partner needed to wed */
#define MARRY_FAMILIAR   0.3     /* familiarity needed to wed */
#define MARRY_CHANCE     0.38    /* daily chance a willing, eligible couple weds */
#define FERTILE_MAX_AGE  45      /* mothers stop conceiving past this */
#define CONCEIVE_CHANCE  0.38    /* daily chance a married fertile couple conceives */
#define GESTATION_DAYS   4       /* game-days of pregnancy before a birth */
#define MAX_CHILDREN     6       /* soft cap per mother */

/* ── Factions ────────────────────────────────────────────────────────────── */
#define FACTION_RADIUS 8

/* ── Agent field of view (perception) ──────────────────────────────────────── */
#define VISION_RADIUS  8    /* how far an agent sees down their line of sight (tiles) */
#define VISION_NEAR    2    /* 360-degree close-range awareness radius (tiles) */

/* ── Agent hearing (auditory perception) ───────────────────────────────────── */
#define HEARING_RADIUS 12   /* how far the loudest act carries (tiles); scaled by loudness */
#define HEARING_MUFFLE 2    /* a wall on the line costs this many tiles of hearing reach */

/* ── Combat, faction warfare & law (governance) ────────────────────────────── */
#define COMBAT_INJURY       0.5    /* base injury a won fight inflicts */
#define WAR_DECLARE_CHANCE  0.06   /* daily per-faction chance to start a war */
#define WAR_MIN_DAYS        2
#define WAR_MAX_DAYS        5
#define WAR_ENGAGE_RADIUS   8      /* how near an enemy soldier must be to be attacked */
#define WAR_ENGAGE_CHANCE   0.012  /* per-tick chance a soldier near an enemy strikes */
#define CRACKDOWN_THRESHOLD 45     /* crimes/day that provokes a police crackdown */
#define CRACKDOWN_DAYS      2      /* how long a crackdown lasts */
#define CRACKDOWN_BONUS     0.15   /* extra police catch chance during a crackdown */

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
    EV_MARRIAGE, EV_WAR,
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
    float age_frac;         /* 0..1 progress toward the next birthday (aging accumulator) */
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
    /* cached A*+JPS path (step directions 0=E,1=W,2=S,3=N), recomputed on retarget */
    unsigned char path[PATH_MAX];
    short path_len, path_i, path_tx, path_ty;

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
    double day_income;          /* net legitimate cash flow accumulated this day (transient) */

    /* ── culture ── */
    unsigned char faith;        /* FAITH_* religion followed */
    unsigned char culture;      /* CUL_* cultural group */
    unsigned char language;     /* LANG_* mother tongue */
    float education;            /* 0..1 learned schooling / literacy */
    float intellect;            /* 0..1 innate research aptitude (reasoning/observation) */
    float craft;                /* 0..1 occupational skill (grows by working; sets output/pay) */

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
    /* household aggregates (homes only; recomputed daily by households_daily) */
    int    hh_size;         /* living residents */
    double hh_wealth;       /* sum of residents' money */
    double hh_income;       /* residents' net legitimate income for the last day */
    float  value;           /* 0..1 home desirability (centrality/parks − crime); homes only */
} Building;

/* City-wide economy: markets, prevailing wages. */
typedef struct {
    double goods_price;     /* luxury/drink price multiplier */
    double wage_mult;       /* prevailing wage multiplier */
} Economy;

/* City-wide knowledge & technology: research accrues from educated scholars into
   theories; theories (+ the prior tech) unlock discoveries, which then diffuse. */
typedef struct {
    double research;                      /* progress toward the next theory */
    int    theories;                      /* city knowledge level */
    unsigned char discovered[TECH_COUNT]; /* 0/1 per tech */
    float  adoption[TECH_COUNT];          /* 0..1 how widely each is in use */
} Science;

typedef struct {
    int id, active;
    char name[32];
    int is_cult;            /* 0 gang, 1 cult */
    int leader_id;
    int members;
    double treasury;
    int war_with;           /* faction id this one is at war with (-1 none) */
    int war_days;           /* days of fighting left before a truce */
    int casualties;         /* members lost to warfare (lifetime) */
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
    uint8_t affluence_[WORLD_W][WORLD_H];/* 0..255 local household wealth (neighborhoods overlay) */

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
    uint64_t tick;  /* monotonic world_tick count — the deterministic clock for replay */
    uint64_t seed;  /* worldgen seed (for pure-function systems like weather) */
    struct { float temp, rain, fog; } weather;  /* 0..1 each; pure fn of (seed,time); --weather */
    Rng rng;

    /* stats for HUD / headless */
    int deaths, crimes;
    int crime_kind[CK_COUNT];   /* per-kind crime tally */
    double wages_earned;        /* cumulative legal income (wages paid to workers) */
    double crime_take[CK_COUNT];/* cumulative illegal proceeds, per crime kind */
    double median_wealth;       /* city median per-capita household wealth (set daily) */
    double loot_tier[3];        /* cumulative loot stolen from poor/mid/rich targets */
    int    crimes_tier[3];      /* count of money crimes against poor/mid/rich targets (by victim wealth) */
    int    crimes_loc_tier[3];  /* count of money crimes in poor/mid/rich NEIGHBOURHOODS (by location affluence) */
    int    n_moves;             /* cumulative household relocations (residential mobility) */
    int crimes_prev_day;        /* w->crimes snapshot at last day change (for crackdowns) */
    int crackdown_days;         /* police crackdown time remaining (law response) */

    Economy econ;               /* city-wide markets, wages */
    Science sci;                /* city-wide knowledge & technology */
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
void danger_decay(World *w);          /* fade the crime-heat (danger_) map; call daily */
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

/* ── Combat / warfare / law (governance) ───────────────────────────────────── */
/* Resolve one violent encounter; returns 1 if the defender was killed. Shared by
   street crime (assaults) and faction warfare. */
int  combat_attack(World *w, Agent *att, Agent *def, const char *context, double base_injury);
void warfare_tick(World *w);         /* warring factions' soldiers fight nearby enemies (per tick) */
void law_daily(World *w);            /* crackdowns when crime surges, on day change */
double police_pressure(const World *w);  /* extra police catch chance (0, or CRACKDOWN_BONUS) */

/* ── Field of view ─────────────────────────────────────────────────────────── */
void set_vision(int on);                 /* --vision / CSIM_VISION: directional sight + line-of-sight */
int  get_vision(void);
void set_vision_radius(int r);
int  get_vision_radius(void);
int  agent_can_see(const World *w, const Agent *viewer, int tx, int ty);  /* FOV + LOS */
/* ── Hearing ───────────────────────────────────────────────────────────────── */
void set_hearing(int on);                /* --hearing / CSIM_HEARING: loud acts heard around corners */
int  get_hearing(void);
void set_hearing_radius(int r);
int  get_hearing_radius(void);
int  agent_can_hear(const World *w, const Agent *l, int sx, int sy, double loudness);
/* ── Worldgen mode ─────────────────────────────────────────────────────────── */
void set_noise_worldgen(int on);         /* --noise-worldgen / CSIM_NOISE_WORLDGEN */
int  get_noise_worldgen(void);
const char *status_title(const Agent *a);   /* honorific from status/role/reputation */
void kinship_daily(World *w);               /* courtship -> marriage, pregnancy -> birth */
void lifecycle_daily(World *w);             /* old-age mortality, on day change */
void set_years_per_day(double y);           /* aging pace (CSIM_YEARS_PER_DAY / --years-per-day) */
double get_years_per_day(void);
void set_family_share(double f);            /* immigrant family share + kids-per-family (startup knobs) */
void set_family_kids(int lo, int hi);
double get_family_share(void);
int    get_family_kids_min(void);
int    get_family_kids_max(void);
void set_pop_target(int t);                 /* living-population target immigration aims for */
int  get_pop_target(void);
void   set_fixed_step(int on);              /* GUI: fixed-timestep (deterministic) vs variable dt */
int    get_fixed_step(void);
void   set_fixed_dt(double d);
double get_fixed_dt(void);
int   life_stage(const Agent *a);           /* LS_CHILD/YOUTH/ADULT/ELDER */
const char *life_stage_name(const Agent *a);
void economy_setup(World *w);               /* assign occupations + landlords */
int  count_properties(const World *w, int owner_id);  /* homes a landlord owns */
const char *occupation_name(unsigned char occ);
void culture_setup(World *w);               /* assign faith/culture/language/education */
void culture_daily(World *w);               /* conversion, assimilation, schooling (diffusion) */
void knowledge_daily(World *w);             /* research -> theories -> tech discovery + adoption */
const char *tech_name(int t);
void   set_research_rate(double r);         /* --research-rate / CSIM_RESEARCH_RATE */
double get_research_rate(void);
void   set_craft_bonus(double b);           /* --production / CSIM_PRODUCTION */
double get_craft_bonus(void);
void   set_child_cost(double c);            /* --child-cost / CSIM_CHILD_COST */
double get_child_cost(void);
double worker_output(const Agent *a);       /* pay/output factor from craft + education */

/* ── Socioeconomics: occupation pay tiers + households (dashboard diversity) ──── */
#define OCC_PAY_SPREAD 0.0                   /* default: flat pay (legacy); 1.0 = full tier spread */
double occ_base_wage(unsigned char occ);     /* occupation pay multiplier, scaled by the spread knob */
void   set_occ_pay_spread(double s);         /* --occ-pay-spread / CSIM_OCC_PAY_SPREAD */
double get_occ_pay_spread(void);
void   households_daily(World *w);            /* recompute per-home hh_size/hh_wealth/hh_income (day change) */
void   set_neighborhoods(int on);             /* --neighborhoods / CSIM_NEIGHBORHOODS: home value + sorting + overlay */
int    get_neighborhoods(void);
void   set_crime_wealth(int on);              /* --crime-wealth / CSIM_CRIME_WEALTH: wealth-scaled loot + EV targeting */
int    get_crime_wealth(void);

/* ── Weather (deterministic noise; --weather) ──────────────────────────────── */
#define WEATHER_PERIOD 5.0                    /* default game-days per weather cycle */
void   set_weather(int on);                   /* --weather / CSIM_WEATHER */
int    get_weather(void);
void   set_weather_period(double days);       /* --weather-period / CSIM_WEATHER_PERIOD */
double get_weather_period(void);
void   set_heat_cost(double f);               /* --heat-cost / CSIM_HEAT_COST (phase 2 scale) */
double get_heat_cost(void);
void   weather_update(World *w);              /* recompute w->weather from (seed,time); call each tick */
double weather_vision_mult(const World *w);   /* 1.0 clear .. ~0.4 thick fog (phase 2) */
double weather_hearing_mult(const World *w);  /* 1.0 clear .. ~0.4 heavy rain (phase 2) */
void   set_police_bias(int mode);             /* --police-bias: 0 crime(default), 1 money, 2 balanced */
int    get_police_bias(void);
double police_bias_bonus(const World *w, int x, int y);  /* extra catch chance at (x,y) from the bias */

/* ── Balance metrics export (dashboard): one CSV row per sample ─────────────── */
#define METRICS_EVERY_HOURS 24.0             /* default sampling cadence: once per game-day */
void metrics_open(const char *path);        /* CSIM_METRICS / --metrics; "" / NULL = off */
void metrics_tick(World *w);                 /* write one row now (no-op unless opened) */
void metrics_sample_maybe(World *w);         /* call every tick; samples on the cadence */
void   set_metrics_every(double hours);      /* --metrics-every / CSIM_METRICS_EVERY (game-hours) */
double get_metrics_every(void);
void metrics_write_manifest(const char *csv_path, unsigned int seed, int days);

const char *faith_name(unsigned char f);
const char *culture_name(unsigned char c);
const char *language_name(unsigned char l);

/* ── Factions ────────────────────────────────────────────────────────────── */
void factions_seed(World *w);
void factions_populate(World *w);    /* enlist crime-prone/faithful agents into gangs/cults */
void factions_daily(World *w);
int  factions_raise(World *w, int is_cult, int cx, int cy);  /* returns faction id or -1 */

/* ── Economy ─────────────────────────────────────────────────────────────── */
void economy_daily(World *w);     /* cost of living, wages settle */

/* ── Pathing (A*) ────────────────────────────────────────────────────────── */
int  tile_walkable(const World *w, int x, int y);
/* step one tile from (fx,fy) toward (tx,ty); writes next (*nx,*ny). 1 if moved. */
int  path_step(const World *w, int fx, int fy, int tx, int ty, int *nx, int *ny);
/* A*+JPS: fill `out` with up to `cap` step directions (0=E,1=W,2=S,3=N) from
   (sx,sy) to (tx,ty); returns path length, 0 if already there, -1 if unreachable. */
int  find_path(const World *w, int sx, int sy, int tx, int ty, unsigned char *out, int cap);

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
int  world_spawn_family(World *w);                   /* immigrate a couple + children; returns people added */

/* ── .env loader (python-dotenv-style; does not override existing env) ──────── */
void dotenv_load(const char *path);     /* load one file if it exists */
void dotenv_autoload(void);             /* try CSIM_ENV, ./.env, ../.env, ../../.env */

/* ── Save / load (JSON via cJSON if available; no-op stubs otherwise) ─────── */
int  world_save(const World *w, const char *path);   /* 1 ok, 0 fail */
int  world_load(World *w, const char *path);

#endif /* SIM_H */
