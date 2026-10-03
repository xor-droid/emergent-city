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
static double crime_severity(const char *k) {
    if (!strcmp(k, "vandalism"))   return 0.8;
    if (!strcmp(k, "theft"))       return 1.0;
    if (!strcmp(k, "burglary"))    return 1.4;
    if (!strcmp(k, "dealing"))     return 1.5;
    if (!strcmp(k, "extortion"))   return 1.6;
    if (!strcmp(k, "robbery"))     return 1.8;
    if (!strcmp(k, "trafficking")) return 2.2;
    if (!strcmp(k, "assault"))     return 2.4;
    if (!strcmp(k, "riot"))        return 2.6;
    if (!strcmp(k, "arson"))       return 2.8;
    if (!strcmp(k, "murder"))      return 3.6;
    return 1.0;
}

const char *crime_role_name(int r) {
    switch (r) {
        case CR_CAREER:  return "career criminal";
        case CR_DEALER:  return "street dealer";
        case CR_KINGPIN: return "kingpin";
        case CR_KILLER:  return "serial killer";
        default:         return "citizen";
    }
}

static const char *CK_NAMES[CK_COUNT] = {
    "theft","burglary","robbery","extortion","vandalism","arson",
    "assault","riot","dealing","trafficking","murder"
};
const char *crime_kind_name(int i) { return (i >= 0 && i < CK_COUNT) ? CK_NAMES[i] : "?"; }
static int ck_index(const char *k) {
    for (int i = 0; i < CK_COUNT; i++) if (!strcmp(k, CK_NAMES[i])) return i;
    return CK_THEFT;
}
/* record one committed crime of this kind */
static void tally(World *w, const char *kind) { w->crimes++; w->crime_kind[ck_index(kind)]++; }

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
/* fraction of the lie-low window still remaining (1=just wanted, 0=about to cool off) */
double crime_cooldown_frac(const Agent *a) {
    if (!a->wanted) return 0;
    double full = WANTED_DURATION * crime_severity(a->wanted_for[0] ? a->wanted_for : "theft");
    return full > 0 ? (double)a->wanted_ticks / full : 0;
}
static void jail(Agent *a, const char *kind) {
    int sentence = (int)(ARREST_DURATION * crime_severity(kind));
    a->arrested_ticks = sentence; a->sentence_total = sentence;
    strncpy(a->jailed_for, kind, 15); a->jailed_for[15] = '\0';
    clear_wanted(a);
}

static void witnesses_at(World *w, Agent *perp, int *civ, int *pol) {
    int x = (int)perp->x, y = (int)perp->y; *civ = 0; *pol = 0;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == perp->id) continue;
        if (abs((int)o->x - x) <= WITNESS_RADIUS && abs((int)o->y - y) <= WITNESS_RADIUS) {
            if (o->is_police) (*pol)++; else (*civ)++;
        }
    }
}

/* nearest plausible drug customer (prefer existing users) */
static Agent *find_user_near(World *w, Agent *d, int radius) {
    Agent *bestA = NULL, *bestAny = NULL; double bdA = 1e18, bdAny = 1e18;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == d->id || o->is_police || o->arrested_ticks > 0) continue;
        if (o->crime_role == CR_DEALER || o->crime_role == CR_KINGPIN) continue;
        double dx = o->x - d->x, dy = o->y - d->y, dd = dx*dx + dy*dy;
        if (dd > (double)radius * radius) continue;
        if (o->addiction > 0.1 && dd < bdA) { bdA = dd; bestA = o; }
        if (dd < bdAny) { bdAny = dd; bestAny = o; }
    }
    return bestA ? bestA : bestAny;
}
static Agent *nearest_kingpin(World *w, Agent *d) {
    Agent *best = NULL; double bd = 1e18;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->crime_role != CR_KINGPIN || o->drug_stock <= 0 || o->arrested_ticks > 0) continue;
        double dx = o->x - d->x, dy = o->y - d->y, dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; best = o; }
    }
    return best;
}
/* another dealer whose customer or turf this sale poaches */
static Agent *turf_conflict(World *w, Agent *d, Agent *cust) {
    if (cust->dealer_id >= 0 && cust->dealer_id != d->id) {
        Agent *a = world_agent_by_id(w, cust->dealer_id);
        if (a && a->alive && (a->crime_role == CR_DEALER || a->crime_role == CR_KINGPIN)) return a;
    }
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->id == d->id || a->crime_role != CR_DEALER) continue;
        if (abs((int)cust->x - a->turf_x) <= TURF_RADIUS && abs((int)cust->y - a->turf_y) <= TURF_RADIUS) return a;
    }
    return NULL;
}
/* wronged dealer A retaliates against offender B (faction turf war or personal) */
static void retaliate(World *w, Agent *A, Agent *B) {
    rel_adjust(&A->rels, B->id, -0.6); rel_adjust(&B->rels, A->id, -0.5);
    char t[96];
    if (A->faction_id >= 0 && B->faction_id >= 0 && A->faction_id != B->faction_id) {
        snprintf(t, sizeof(t), "Turf war: %.28s muscles into %.28s territory",
                 w->factions[B->faction_id].name, w->factions[A->faction_id].name);
        events_post(w, EV_QUARREL, A->id, B->id, (int)A->x, (int)A->y, 0.7, t);
        for (int i = 0; i < w->n_agents; i++) {   /* a soldier of A's gang hits B if near */
            Agent *s = &w->agents[i];
            if (!s->alive || s->faction_id != A->faction_id || s->id == B->id || s->arrested_ticks > 0) continue;
            if (abs((int)s->x - (int)B->x) <= RETALIATE_RADIUS && abs((int)s->y - (int)B->y) <= RETALIATE_RADIUS) {
                crime_attempt(w, s, B, "assault"); return;
            }
        }
    } else {
        snprintf(t, sizeof(t), "%s moves in on %s's turf", B->name, A->name);
        events_post(w, EV_QUARREL, A->id, B->id, (int)A->x, (int)A->y, 0.5, t);
    }
    if (abs((int)A->x - (int)B->x) <= RETALIATE_RADIUS && abs((int)A->y - (int)B->y) <= RETALIATE_RADIUS)
        crime_attempt(w, A, B, "assault");
}

static void do_deal(World *w, Agent *d) {
    if (d->drug_stock <= 0) return;
    Agent *cust = find_user_near(w, d, SELL_RADIUS);
    if (!cust) return;
    int civ, pol; witnesses_at(w, d, &civ, &pol);
    if (pol && rng_double(&w->rng) < 0.5 - d->crime_skill * 0.3) {
        jail(d, "dealing");
        char t[96]; snprintf(t, sizeof(t), "%s was busted dealing", d->name);
        events_post(w, EV_ARREST, d->id, cust->id, (int)d->x, (int)d->y, 0.8, t);
        return;
    }
    double price = DRUG_STREET_PRICE * (0.7 + cust->addiction * 0.8);
    if (price > cust->needs.money) price = cust->needs.money;
    if (price < 1.0) return;   /* broke customer */
    Agent *rival = turf_conflict(w, d, cust);
    cust->needs.money -= price; d->needs.money += price; d->drug_stock--;
    cust->addiction = (float)clampd(cust->addiction + 0.07, 0, 1);
    cust->needs.social = clampd(cust->needs.social + 0.15, 0, 1);  /* the high */
    cust->dealer_id = d->id;
    d->crimes_committed++; d->reputation = (float)clampd(d->reputation - 0.03, -1, 1); tally(w, "dealing");
    char t[96]; snprintf(t, sizeof(t), "%s sold drugs to %s ($%.0f)", d->name, cust->name, price);
    events_post(w, EV_CRIME, d->id, cust->id, (int)d->x, (int)d->y, 0.45, t);
    if (civ && rng_double(&w->rng) < 0.3) mark_wanted(w, d, "dealing");
    if (rival && rival->id != d->id) retaliate(w, rival, d);
}

static void do_traffic(World *w, Agent *p) {
    char t[96];
    if (p->crime_role == CR_KINGPIN) {
        if (p->drug_stock >= DRUG_BATCH) return;   /* only import when the stash runs low */
        int civ, pol; witnesses_at(w, p, &civ, &pol); (void)civ;
        if (pol && rng_double(&w->rng) < 0.35 - p->crime_skill * 0.25) {
            jail(p, "trafficking");
            snprintf(t, sizeof(t), "%s was caught trafficking a shipment", p->name);
            events_post(w, EV_ARREST, p->id, -1, (int)p->x, (int)p->y, 0.9, t);
            return;
        }
        p->drug_stock += DRUG_BATCH; p->crimes_committed++; tally(w, "trafficking");
        snprintf(t, sizeof(t), "%s brought in a drug shipment (%d units)", p->name, DRUG_BATCH);
        events_post(w, EV_CRIME, p->id, -1, (int)p->x, (int)p->y, 0.5, t);
    } else {  /* dealer buys wholesale */
        Agent *kp = nearest_kingpin(w, p);
        if (!kp) return;
        int units = DRUG_BATCH; if (kp->drug_stock < units) units = kp->drug_stock;
        double cost = units * DRUG_WHOLESALE;
        if (cost > p->needs.money) { units = (int)(p->needs.money / DRUG_WHOLESALE); cost = units * DRUG_WHOLESALE; }
        if (units <= 0) return;
        p->needs.money -= cost; kp->needs.money += cost; kp->drug_stock -= units; p->drug_stock += units;
        snprintf(t, sizeof(t), "%.24s bought %d units wholesale from %.24s", p->name, units, kp->name);
        events_post(w, EV_CRIME, p->id, kp->id, (int)p->x, (int)p->y, 0.4, t);
    }
}

static void do_murder(World *w, Agent *k, Agent *victim) {
    if (!victim || !victim->alive) return;
    int civ, pol; witnesses_at(w, k, &civ, &pol);
    double chance = clampd(0.6 + k->crime_skill * 0.3 - civ * 0.15 - (pol ? 0.5 : 0), 0.03, 0.97);
    char t[96];
    if (pol && rng_double(&w->rng) > chance) {
        jail(k, "murder");
        snprintf(t, sizeof(t), "%s was caught attempting murder and arrested", k->name);
        events_post(w, EV_ARREST, k->id, victim->id, (int)k->x, (int)k->y, 1.0, t);
        return;
    }
    if (rng_double(&w->rng) < chance) {
        victim->alive = 0; w->deaths++; tally(w, "murder"); k->crimes_committed++;
        k->reputation = (float)clampd(k->reputation - 0.15, -1, 1);
        k->needs.meaning = clampd(k->needs.meaning + 0.3, 0, 1);  /* the thrill */
        k->needs.safety  = clampd(k->needs.safety  + 0.2, 0, 1);
        for (int i = 0; i < w->n_agents; i++) {   /* terror nearby */
            Agent *o = &w->agents[i]; if (!o->alive || o->id == k->id) continue;
            if (abs((int)o->x - (int)k->x) <= WITNESS_RADIUS*2 && abs((int)o->y - (int)k->y) <= WITNESS_RADIUS*2)
                o->needs.safety = clampd(o->needs.safety - 0.4, 0, 1);
        }
        snprintf(t, sizeof(t), "%s was murdered — a serial killer stalks the city", victim->name);
        events_post(w, EV_DEATH, victim->id, k->id, (int)victim->x, (int)victim->y, 1.0, t);
        if (civ) mark_wanted(w, k, "murder");
    } else {
        victim->needs.safety = clampd(victim->needs.safety - 0.5, 0, 1);
        snprintf(t, sizeof(t), "%s survived an attack by an unknown assailant", victim->name);
        events_post(w, EV_CRIME_FAILED, k->id, victim->id, (int)k->x, (int)k->y, 0.8, t);
        if (civ) mark_wanted(w, k, "murder");
    }
}

/* add injury to a victim; may be fatal (shared by street assaults + jail brawls) */
static void apply_injury(World *w, Agent *v, double amt, const char *cause, Agent *by) {
    if (!v || !v->alive) return;
    v->injury = (float)clampd(v->injury + amt, 0, 2);
    if (v->injury >= INJURY_FATAL) {
        v->alive = 0; w->deaths++;
        char t[96];
        if (by) snprintf(t, sizeof(t), "%.26s died of injuries from %.26s (%s)", v->name, by->name, cause);
        else    snprintf(t, sizeof(t), "%.30s died of their injuries (%s)", v->name, cause);
        events_post(w, EV_DEATH, v->id, by ? by->id : -1, (int)v->x, (int)v->y, 0.9, t);
    }
}

void crime_attempt(World *w, Agent *perp, Agent *target, const char *kind) {
    if (!strcmp(kind, "dealing"))     { do_deal(w, perp);          return; }
    if (!strcmp(kind, "trafficking")) { do_traffic(w, perp);       return; }
    if (!strcmp(kind, "murder"))      { do_murder(w, perp, target); return; }

    int x = (int)perp->x, y = (int)perp->y;
    int civ, pol; witnesses_at(w, perp, &civ, &pol);
    double skill = perp->crime_skill;
    double witw = (!strcmp(kind, "burglary")) ? 0.03 : 0.10;   /* burglary is indoors */
    double chance = clampd(0.62 - witw * civ - (pol ? 0.55 : 0.0)
                           + pers_crime_propensity(&perp->pers) * 0.12 + skill * 0.25, 0.05, 0.96);
    int success = rng_double(&w->rng) < chance;
    double importance = (!strcmp(kind,"theft")||!strcmp(kind,"vandalism")) ? 0.5 : 0.85;
    char vs[48]; vs[0] = '\0'; if (target) snprintf(vs, sizeof(vs), " against %s", target->name);
    char t[96];

    if (success) {
        double loot = 0;
        if      (!strcmp(kind, "theft"))     loot = rng_range(&w->rng, 5, 25);
        else if (!strcmp(kind, "burglary"))  loot = rng_range(&w->rng, 25, 90);
        else if (!strcmp(kind, "robbery"))   loot = rng_range(&w->rng, 15, 55);
        else if (!strcmp(kind, "extortion")) loot = rng_range(&w->rng, 15, 50);
        else if (!strcmp(kind, "riot"))      loot = rng_range(&w->rng, 5, 40);
        perp->needs.money += loot;
        if (target) {
            if (loot > 0) target->needs.money = clampd(target->needs.money - loot, 0, 1e9);
            target->needs.safety = clampd(target->needs.safety - (!strcmp(kind,"assault")?0.5:0.3), 0, 1);
            if (!strcmp(kind, "assault")) apply_injury(w, target, ASSAULT_INJURY, "assault", perp);
        } else if (loot > 0) {
            /* (D) no direct target (e.g. burglary) — take it from the nearest resident
             * so stolen money is transferred, not injected into the economy */
            Agent *v = NULL; int bd = 0;
            for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
                if (!o->alive || o->id == perp->id || o->needs.money < loot) continue;
                int d = abs((int)o->x - x) + abs((int)o->y - y);
                if (d <= WITNESS_RADIUS*4 && (!v || d < bd)) { v = o; bd = d; }
            }
            if (v) v->needs.money = clampd(v->needs.money - loot, 0, 1e9);
        }
        if (!strcmp(kind, "extortion") && perp->faction_id >= 0)
            w->factions[perp->faction_id].treasury += loot * 0.5;
        if (!strcmp(kind, "arson") || !strcmp(kind, "vandalism")) {
            for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
                if (!o->alive || o->id == perp->id) continue;
                if (abs((int)o->x - x) <= WITNESS_RADIUS && abs((int)o->y - y) <= WITNESS_RADIUS)
                    o->needs.safety = clampd(o->needs.safety - (!strcmp(kind,"arson")?0.4:0.15), 0, 1);
            }
        }
        perp->crimes_committed++;
        perp->reputation = (float)clampd(perp->reputation - 0.03 * crime_severity(kind), -1, 1);
        if ((perp->crime_role == CR_CAREER || perp->crime_role == CR_DEALER) && perp->crime_skill < 0.95)
            perp->crime_skill += 0.02f;
        tally(w, kind);
        if (loot > 0) snprintf(t, sizeof(t), "%s committed %s%s ($%.0f)", perp->name, kind, vs, loot);
        else          snprintf(t, sizeof(t), "%s committed %s%s", perp->name, kind, vs);
        events_post(w, EV_CRIME, perp->id, target ? target->id : -1, x, y, importance, t);
        if (civ) mark_wanted(w, perp, kind);
    } else if (pol) {
        perp->needs.safety = clampd(perp->needs.safety - 0.3, 0, 1);
        jail(perp, kind);
        snprintf(t, sizeof(t), "%s was caught and arrested for %s%s", perp->name, kind, vs);
        events_post(w, EV_ARREST, perp->id, target ? target->id : -1, x, y, importance + 0.15, t);
    } else {
        perp->needs.safety = clampd(perp->needs.safety - 0.3, 0, 1);
        snprintf(t, sizeof(t), "%s botched %s%s and fled", perp->name, kind, vs);
        events_post(w, EV_CRIME_FAILED, perp->id, target ? target->id : -1, x, y, importance + 0.1, t);
        if (civ) mark_wanted(w, perp, kind);
    }
}

/* Assign crime roles after population: users, career criminals, dealers,
 * kingpins, and (rarely) a latent serial killer. */
void assign_crime_roles(World *w) {
    int kingpins = 0, dealers = 0;
    int maxKing = 1 + w->n_agents / 90;
    int maxDeal = 2 + w->n_agents / 40;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        a->dealer_id = -1; a->status = 2;   /* default tier: Citizen */
        if (rng_double(&w->rng) < KILLER_CHANCE) { a->crime_role = CR_KILLER;
            a->crime_skill = 0.4f + (float)rng_double(&w->rng) * 0.4f; continue; }
        if (a->is_police) continue;
        if (rng_double(&w->rng) < USER_FRACTION) a->addiction = (float)rng_range(&w->rng, 0.25, 0.75);
        double prop = pers_crime_propensity(&a->pers);
        if (prop > 0.50 && rng_double(&w->rng) < 0.22) {
            a->crime_skill = 0.25f + (float)prop * 0.4f;
            if (kingpins < maxKing && rng_double(&w->rng) < 0.25) {
                a->crime_role = CR_KINGPIN; a->drug_stock = DRUG_BATCH * 2; kingpins++;
            } else if (dealers < maxDeal) {
                a->crime_role = CR_DEALER; a->drug_stock = DRUG_BATCH;
                a->turf_x = (int)a->x; a->turf_y = (int)a->y; dealers++;
            } else {
                a->crime_role = CR_CAREER;
            }
        }
    }
    /* guarantee a police presence so crime has a counterforce (station placement
     * is RNG-dependent and can yield none) */
    int police = 0;
    for (int i = 0; i < w->n_agents; i++) police += w->agents[i].is_police;
    int want_police = 3 + w->n_agents / 40;
    for (int i = 0; i < w->n_agents && police < want_police; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->is_police || a->crime_role != CR_CITIZEN) continue;
        a->is_police = 1; police++;
    }

    int nc=0,nd=0,nk=0,nkill=0,nu=0;
    for (int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i];
        if(a->crime_role==CR_CAREER)nc++; else if(a->crime_role==CR_DEALER)nd++;
        else if(a->crime_role==CR_KINGPIN)nk++; else if(a->crime_role==CR_KILLER)nkill++;
        if(a->addiction>0.0f)nu++; }
    fprintf(stderr,"[roles] career=%d dealer=%d kingpin=%d killer=%d  users=%d  police=%d\n",
            nc,nd,nk,nkill,nu,police);
}

/* Daily: (B) role mobility/emergence + (C) a trickle of newcomers. */
void crime_daily(World *w) {
    int dealers = 0, kingpins = 0;
    for (int i = 0; i < w->n_agents; i++) {
        if (w->agents[i].crime_role == CR_DEALER) dealers++;
        else if (w->agents[i].crime_role == CR_KINGPIN) kingpins++;
    }
    int maxDeal = 2 + w->n_agents / 40, maxKing = 1 + w->n_agents / 90;

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->is_police || a->arrested_ticks > 0) continue;
        char t[96];
        if (a->crime_role == CR_CITIZEN && a->crimes_committed >= 5) {
            a->crime_role = CR_CAREER; if (a->crime_skill < 0.3f) a->crime_skill = 0.3f;
            snprintf(t, sizeof(t), "%s has turned to a life of crime", a->name);
            events_post(w, EV_FACTION, a->id, -1, (int)a->x, (int)a->y, 0.5, t);
        } else if (a->crime_role == CR_CAREER && a->crimes_committed >= 25 && kingpins < maxKing && rng_double(&w->rng) < 0.3) {
            a->crime_role = CR_KINGPIN; a->drug_stock += DRUG_BATCH * 2; kingpins++;
            snprintf(t, sizeof(t), "%s rose to run a drug operation", a->name);
            events_post(w, EV_FACTION, a->id, -1, (int)a->x, (int)a->y, 0.75, t);
        } else if (a->crime_role == CR_CAREER && a->crimes_committed >= 15 && dealers < maxDeal && rng_double(&w->rng) < 0.3) {
            a->crime_role = CR_DEALER; a->turf_x = (int)a->x; a->turf_y = (int)a->y; a->drug_stock += DRUG_BATCH; dealers++;
            snprintf(t, sizeof(t), "%s started dealing on the corner", a->name);
            events_post(w, EV_FACTION, a->id, -1, (int)a->x, (int)a->y, 0.55, t);
        }
    }

    if (w->n_agents < 200 && rng_double(&w->rng) < 0.4) {   /* soft population cap */
        int id = world_spawn_agent(w, -1, -1);
        if (id >= 0) {
            Agent *a = world_agent_by_id(w, id);
            if (a) {
                a->dealer_id = -1; a->status = 2;
                if (rng_double(&w->rng) < KILLER_CHANCE * 4) { a->crime_role = CR_KILLER; a->crime_skill = 0.4f; }
                else {
                    if (rng_double(&w->rng) < USER_FRACTION) a->addiction = (float)rng_range(&w->rng, 0.25, 0.75);
                    double prop = pers_crime_propensity(&a->pers);
                    if (prop > 0.5 && rng_double(&w->rng) < 0.22) { a->crime_role = CR_CAREER; a->crime_skill = 0.25f + (float)prop * 0.4f; }
                }
            }
        }
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

/* ── Jail: gangs form behind bars and settle scores ─────────────────────────── */
static const char *JAIL_GANG_NAMES[JAIL_GANGS] = { "the Yard Kings", "Cellblock Crew", "the Lifers" };
const char *jail_gang_name(int g) { return (g >= 1 && g <= JAIL_GANGS) ? JAIL_GANG_NAMES[g-1] : "-"; }

/* honorific derived from role, reputation and social tier */
const char *status_title(const Agent *a) {
    if (a->is_police) return "Officer";
    if (a->crime_role == CR_KINGPIN) return "Kingpin";
    if (a->reputation < -0.4f) return "Notorious";
    switch (a->status) {
        case 0:  return "Destitute";
        case 1:  return "Struggling";
        case 3:  return "Esteemed";
        case 4:  return "Magnate";
        default: return "Citizen";
    }
}

void jail_tick(World *w) {
    /* recruit inmates with a criminal bent into a jail gang */
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->arrested_ticks <= 0 || a->jail_gang) continue;
        double bent = pers_crime_propensity(&a->pers) + (a->crime_role != CR_CITIZEN ? 0.4 : 0.0);
        if (bent > 0.5 && rng_double(&w->rng) < 0.6) a->jail_gang = (unsigned char)(1 + rng_int(&w->rng, JAIL_GANGS));
    }
    /* brawls: a gang member shanks/beats a rival-gang (or any) inmate */
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->arrested_ticks <= 0 || !a->jail_gang) continue;
        if (rng_double(&w->rng) > JAIL_VIOLENCE_P) continue;
        Agent *vic = NULL;
        for (int pass = 0; pass < 2 && !vic; pass++)
            for (int j = 0; j < w->n_agents; j++) {
                Agent *o = &w->agents[j];
                if (!o->alive || o->arrested_ticks <= 0 || o->id == a->id) continue;
                if (pass == 0 ? (o->jail_gang && o->jail_gang != a->jail_gang) : (o->jail_gang != a->jail_gang)) { vic = o; break; }
            }
        if (!vic) continue;
        a->crimes_committed++;
        int lethal = vic->injury > 0.5 || rng_double(&w->rng) < 0.18 + a->crime_skill * 0.2;
        if (lethal) {
            apply_injury(w, vic, 1.0, "a prison brawl", a);   /* fatal; posts the death */
        } else {
            apply_injury(w, vic, JAIL_BEATING, "a jail beating", a);
            if (vic->alive) {
                char t[96]; snprintf(t, sizeof(t), "%.22s (%s) beat %.22s in a jail brawl",
                                     a->name, jail_gang_name(a->jail_gang), vic->name);
                events_post(w, EV_CRIME, a->id, vic->id, (int)vic->x, (int)vic->y, 0.5, t);
            }
        }
    }
}

/* ── Economy ─────────────────────────────────────────────────────────────── */
void economy_daily(World *w) {
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        double cost = RENT_PER_DAY + a->needs.money * UPKEEP_FRACTION;
        a->needs.money = clampd(a->needs.money - cost, 0.0, 1e9);
        if (a->addiction > 0.0f) a->addiction = (float)clampd(a->addiction - 0.015, 0, 1);  /* habit fades without use */
        if (a->injury > 0.0f) a->injury = (float)clampd(a->injury - 0.08, 0, 2);            /* wounds slowly heal */
        a->reputation = (float)clampd(a->reputation + (a->reputation > 0 ? -0.01 : 0.01), -1, 1);  /* drift to neutral */
        /* recompute social tier from wealth + reputation + role */
        int tier = 2;
        if (a->needs.money > 400) tier++;
        if (a->needs.money > 1000) tier++;
        if (a->needs.money < LOW_MONEY) tier--;
        if (a->reputation > 0.3f) tier++;
        if (a->reputation < -0.3f) tier--;
        if (a->crime_role == CR_KINGPIN) tier++;
        if (tier < 0) tier = 0; if (tier > 4) tier = 4;
        a->status = (unsigned char)tier;
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
