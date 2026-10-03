/* sim.h — Emergent City C port: core data + simulation API (no graphics deps).
 *
 * This is a proof-of-concept port of the Python sim's data-oriented core:
 * Needs, Personality, UtilityAI and a minimal World/agent/time loop. It compiles
 * with plain C (libm only); the renderer (main.c, raylib) is a thin layer on top.
 */
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include "rng.h"

/* ── World ───────────────────────────────────────────────────────────────── */
#define WORLD_W 120
#define WORLD_H 90
#define MAX_AGENTS 400

/* ── Needs decay per in-game hour (from config.py) ───────────────────────── */
#define HUNGER_DECAY     0.045
#define ENERGY_DECAY     0.035
#define SOCIAL_DECAY     0.020
#define MEANING_DECAY    0.015
#define BELONGING_DECAY  0.012
#define NEED_CRITICAL    0.20
#define NEED_LOW         0.40

/* ── Time ────────────────────────────────────────────────────────────────── */
#define SECONDS_PER_HOUR 30.0
#define TIME_SCALE       (3600.0 / SECONDS_PER_HOUR)   /* in-game sec / real sec */
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

/* ── Personality ─────────────────────────────────────────────────────────── */
#define BIG5_MEAN  0.5
#define BIG5_SD    0.18

#define AGENT_SPEED_TPS 2.0   /* tiles per real second at 1x */
#define ACTION_SECONDS  6.0

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
    T_GRASS, T_ROAD, T_HOME, T_SHOP, T_WORK, T_BAR, T_CHURCH, T_TYPE_COUNT
} TileType;

typedef enum {
    A_EAT, A_SLEEP, A_WORK, A_SOCIALIZE, A_DRINK, A_PRAY, A_SHOP,
    A_GO_HOME, A_CRIME, A_FLEE, A_WANDER, A_COUNT
} Action;

typedef struct {
    double hunger, energy, safety, social, meaning, belonging, money;
} Needs;

typedef struct {
    double o, c, e, a, n;   /* Big Five */
    uint32_t traits;        /* TR_* bitmask */
} Personality;

typedef struct {
    int id, alive;
    double x, y;            /* tile position (float for smooth-ish move) */
    int tx, ty;            /* current target tile (-1 if none) */
    int home_x, home_y;
    Needs needs;
    Personality pers;
    Action action;
    double action_progress; /* seconds until re-decide */
    int acted;             /* action effect already applied this decision */
    double move_progress;   /* accumulates toward one-tile steps */
    unsigned char r, g, b;  /* render color (mood) */
} Agent;

typedef struct {
    uint8_t tile[WORLD_W][WORLD_H];   /* TileType */
    Agent agents[MAX_AGENTS];
    int n_agents;
    double hour;    /* 0..24 */
    int day;
    Rng rng;
    /* lightweight stats for the HUD / headless report */
    int deaths;
    int crimes;
} World;

/* Personality */
void   personality_random(Personality *p, Rng *r);
double pers_work_ethic(const Personality *p);
double pers_social_drive(const Personality *p);
double pers_crime_propensity(const Personality *p);
double pers_faith(const Personality *p);
int    pers_has(const Personality *p, uint32_t trait);

/* Needs */
void needs_decay(Needs *n, double hours);
int  needs_is_critical(const Needs *n);
int  needs_is_dying(const Needs *n);

/* Utility AI */
Action      utility_best_action(const Agent *a, const World *w);
const char *action_name(Action a);
const char *tile_name(TileType t);

/* World */
void world_init(World *w, uint64_t seed);
void world_populate(World *w, int n);
void world_tick(World *w, double dt_seconds);
int  world_is_night(const World *w);

#endif /* SIM_H */
