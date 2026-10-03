/* systems.c — crime (+wanted/jail/severity), factions, economy. */
#include "sim.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Crime severity ──────────────────────────────────────────────────────── */
static double crime_severity(const char *kind) {
    if (!strcmp(kind, "robbery")) return 1.8;
    if (!strcmp(kind, "assault")) return 2.4;
    if (!strcmp(kind, "riot"))    return 2.6;
    return 1.0;  /* theft */
}

static void mark_wanted(World *w, Agent *a, const char *kind) {
    int heat = (int)(WANTED_DURATION * crime_severity(kind));
    if (a->wanted) { if (heat > a->wanted_ticks) { a->wanted_ticks = heat; strncpy(a->wanted_for, kind, 15); } return; }
    a->wanted = 1;
    strncpy(a->wanted_for, kind, 15); a->wanted_for[15] = '\0';
    a->wanted_ticks = heat;
    char t[96]; snprintf(t, sizeof(t), "%s is now wanted for %s", a->name, kind);
    events_post(w, EV_WANTED, a->id, -1, (int)a->x, (int)a->y, 0.5, t);
}
static void clear_wanted(Agent *a) { a->wanted = 0; a->wanted_ticks = 0; a->wanted_for[0] = '\0'; }
static void jail(Agent *a, const char *kind) {
    int sentence = (int)(ARREST_DURATION * crime_severity(kind));
    a->arrested_ticks = sentence; a->sentence_total = sentence;
    strncpy(a->jailed_for, kind, 15); a->jailed_for[15] = '\0';
    clear_wanted(a);
}

void crime_attempt(World *w, Agent *perp, Agent *target, const char *kind) {
    int x = (int)perp->x, y = (int)perp->y;
    int civ_witnesses = 0, police_near = 0;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == perp->id) continue;
        if (abs((int)o->x - x) <= WITNESS_RADIUS && abs((int)o->y - y) <= WITNESS_RADIUS) {
            if (o->is_police) police_near = 1; else civ_witnesses++;
        }
    }
    double chance = 0.65 - 0.10 * civ_witnesses - (police_near ? 0.55 : 0.0)
                    + pers_crime_propensity(&perp->pers) * 0.15;
    chance = clampd(chance, 0.05, 0.95);
    int success = rng_double(&w->rng) < chance;
    double importance = (!strcmp(kind, "theft")) ? 0.55 : 0.85;
    char vs[48]; vs[0] = '\0';
    if (target) snprintf(vs, sizeof(vs), " against %s", target->name);
    char t[96];

    if (success) {
        double loot = rng_range(&w->rng, 5.0, 40.0);
        perp->needs.money += loot;
        if (target) { target->needs.money = clampd(target->needs.money - loot, 0, 1e9);
                      target->needs.safety = clampd(target->needs.safety - 0.4, 0, 1); }
        w->crimes++;
        snprintf(t, sizeof(t), "%s committed %s%s ($%.0f)", perp->name, kind, vs, loot);
        events_post(w, EV_CRIME, perp->id, target ? target->id : -1, x, y, importance, t);
        if (civ_witnesses) mark_wanted(w, perp, kind);
    } else if (police_near) {
        perp->needs.safety = clampd(perp->needs.safety - 0.3, 0, 1);
        jail(perp, kind);
        snprintf(t, sizeof(t), "%s was caught in the act and arrested for %s%s", perp->name, kind, vs);
        events_post(w, EV_ARREST, perp->id, target ? target->id : -1, x, y, importance + 0.15, t);
    } else {
        perp->needs.safety = clampd(perp->needs.safety - 0.3, 0, 1);
        snprintf(t, sizeof(t), "%s botched %s%s and fled", perp->name, kind, vs);
        events_post(w, EV_CRIME_FAILED, perp->id, target ? target->id : -1, x, y, importance + 0.1, t);
        if (civ_witnesses) mark_wanted(w, perp, kind);
    }
}

/* Police hunt wanted agents; uncaught heat cools off ("lying low"). */
void crime_tick(World *w) {
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || !a->wanted || a->arrested_ticks > 0) continue;
        Agent *cop = NULL;
        for (int j = 0; j < w->n_agents; j++) {
            Agent *p = &w->agents[j];
            if (!p->alive || !p->is_police || p->arrested_ticks > 0 || p->id == a->id) continue;
            if (abs((int)p->x - (int)a->x) <= POLICE_ARREST_RADIUS &&
                abs((int)p->y - (int)a->y) <= POLICE_ARREST_RADIUS) { cop = p; break; }
        }
        if (cop && rng_double(&w->rng) < POLICE_ARREST_CHANCE) {
            char kind[16]; strncpy(kind, a->wanted_for[0] ? a->wanted_for : "a crime", 15); kind[15] = '\0';
            jail(a, kind);
            char t[96];
            snprintf(t, sizeof(t), "%s was tracked down and arrested by %s (wanted for %s)",
                     a->name, cop->name, kind);
            events_post(w, EV_ARREST, a->id, cop->id, (int)a->x, (int)a->y, 0.7, t);
            continue;
        }
        if (--a->wanted_ticks <= 0) {
            char t[96];
            snprintf(t, sizeof(t), "%s lay low and is no longer wanted", a->name);
            events_post(w, EV_LAID_LOW, a->id, -1, (int)a->x, (int)a->y, 0.3, t);
            clear_wanted(a);
        }
    }
}

int crime_jailed_count(const World *w) {
    int c = 0;
    for (int i = 0; i < w->n_agents; i++) if (w->agents[i].alive && w->agents[i].arrested_ticks > 0) c++;
    return c;
}
int crime_wanted_count(const World *w) {
    int c = 0;
    for (int i = 0; i < w->n_agents; i++) if (w->agents[i].alive && w->agents[i].wanted) c++;
    return c;
}

/* ── Factions ────────────────────────────────────────────────────────────── */
static const char *GANG_NAMES[] = {"Scarlet Hounds","Black Bay","The Needles",
    "Northern Wolves","Factory Shadows","The Twenty-Thirds"};
static const char *CULT_NAMES[] = {"Children of the Dawn","Silence of the Moon",
    "Order of the Ninth Hour","Circle of Ash","Sisters of the Water"};

static Faction *faction_create(World *w, int is_cult, const char *name,
                               unsigned char r, unsigned char g, unsigned char b) {
    if (w->n_factions >= MAX_FACTIONS) return NULL;
    Faction *f = &w->factions[w->n_factions];
    f->id = w->n_factions; f->active = 1; f->is_cult = is_cult;
    strncpy(f->name, name, sizeof(f->name) - 1); f->name[sizeof(f->name) - 1] = '\0';
    f->leader_id = -1; f->members = 0; f->treasury = 0;
    f->r = r; f->g = g; f->b = b;
    w->n_factions++;
    return f;
}

void factions_seed(World *w) {
    unsigned char gc[][3] = {{200,50,50},{40,40,40},{170,80,200},{100,100,220},{160,110,50},{220,180,60}};
    unsigned char cc[][3] = {{230,200,70},{180,180,240},{120,60,160},{90,90,90},{60,160,200}};
    for (int i = 0; i < 6; i++) faction_create(w, 0, GANG_NAMES[i], gc[i][0], gc[i][1], gc[i][2]);
    for (int i = 0; i < 5; i++) faction_create(w, 1, CULT_NAMES[i], cc[i][0], cc[i][1], cc[i][2]);
}

void factions_daily(World *w) {
    for (int i = 0; i < w->n_factions; i++) {
        Faction *f = &w->factions[i];
        if (!f->active) continue;
        f->treasury += f->is_cult ? f->members * 3 : f->members * 5 - 10;
        /* recruit a desperate/faithful unaffiliated citizen */
        if (rng_double(&w->rng) < 0.25 && w->n_agents > 0) {
            Agent *c = &w->agents[rng_int(&w->rng, w->n_agents)];
            if (!c->alive || c->faction_id != -1) continue;
            int join = f->is_cult
                ? (pers_faith(&c->pers) > 0.7 && c->needs.meaning < 0.4)
                : (pers_crime_propensity(&c->pers) > 0.5 && c->needs.belonging < 0.4);
            if (join) {
                c->faction_id = f->id; f->members++;
                if (f->leader_id == -1) f->leader_id = c->id;
                char t[96]; snprintf(t, sizeof(t), "%s joined %s", c->name, f->name);
                events_post(w, EV_FACTION, c->id, -1, (int)c->x, (int)c->y, 0.45, t);
            }
        }
    }
}

int factions_raise(World *w, int is_cult, int cx, int cy) {
    /* recruit nearby unaffiliated agents into a fresh faction */
    const char **names = is_cult ? CULT_NAMES : GANG_NAMES;
    int nnames = is_cult ? 5 : 6;
    int idx = rng_int(&w->rng, nnames);
    unsigned char r = (unsigned char)rng_int_incl(&w->rng, 60, 230);
    unsigned char g = (unsigned char)rng_int_incl(&w->rng, 60, 230);
    unsigned char b = (unsigned char)rng_int_incl(&w->rng, 60, 230);
    Faction *f = faction_create(w, is_cult, names[idx], r, g, b);
    if (!f) return -1;
    int recruited = 0;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->faction_id != -1) continue;
        if (abs((int)a->x - cx) <= FACTION_RADIUS && abs((int)a->y - cy) <= FACTION_RADIUS) {
            a->faction_id = f->id; f->members++;
            if (f->leader_id == -1) f->leader_id = a->id;
            a->needs.belonging = clampd(a->needs.belonging + 0.3, 0, 1);
            recruited++;
        }
    }
    if (recruited < 2) { f->active = 0; w->n_factions--; return -1; }  /* undo weak faction */
    char t[96];
    snprintf(t, sizeof(t), "%s '%s' formed with %d members",
             is_cult ? "Cult" : "Gang", f->name, recruited);
    events_post(w, EV_FACTION, f->leader_id, -1, cx, cy, 0.8, t);
    return f->id;
}

/* ── Economy ─────────────────────────────────────────────────────────────── */
void economy_daily(World *w) {
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        double cost = RENT_PER_DAY + a->needs.money * UPKEEP_FRACTION;
        a->needs.money = clampd(a->needs.money - cost, 0.0, 1e9);
        /* ambient: becoming destitute (can't afford a meal) */
        if (a->needs.money < MEAL_PRICE && !a->broke_flagged) {
            a->broke_flagged = 1;
            char t[96]; snprintf(t, sizeof(t), "%s is destitute", a->name);
            events_post(w, EV_HARDSHIP, a->id, -1, (int)a->x, (int)a->y, 0.4, t);
        } else if (a->needs.money >= LOW_MONEY) {
            a->broke_flagged = 0;
        }
    }
}
