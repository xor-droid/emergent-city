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
    /* sentencing tiers: base scales with the offence; repeat offenders serve longer
       (habitual-offender law), capped at roughly triple for a hardened rap sheet */
    double repeat = 1.0 + (a->crimes_committed < 20 ? a->crimes_committed : 20) * 0.1;
    int sentence = (int)(ARREST_DURATION * crime_severity(kind) * repeat);
    a->arrested_ticks = sentence; a->sentence_total = sentence;
    strncpy(a->jailed_for, kind, 15); a->jailed_for[15] = '\0';
    clear_wanted(a);
}

/* extra police effectiveness while a crackdown is in force (law responding to disorder) */
double police_pressure(const World *w) {
    return (w->crackdown_days > 0 ? CRACKDOWN_BONUS : 0.0)
         + 0.1 * w->sci.adoption[TECH_CIVICS];   /* Civics (tech) = better law enforcement */
}

/* Bresenham line of sight; buildings between the two points block it. */
static int line_of_sight(const World *w, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, x = x0, y = y0;
    while (!(x == x1 && y == y1)) {
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        if (x == x1 && y == y1) break;                /* reached the target: it's visible */
        if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) return 0;
        { TileType tt = (TileType)w->tile[x][y];
          if (tt >= T_HOME && tt <= T_POLICE) return 0; }       /* an opaque building blocks sight */
    }
    return 1;
}

/* Can `viewer` see tile (tx,ty)? 360deg within VISION_NEAR, a ~120deg forward cone
   out to the vision radius, and line-of-sight the whole way. */
int agent_can_see(const World *w, const Agent *viewer, int tx, int ty) {
    int dx = tx - (int)viewer->x, dy = ty - (int)viewer->y;
    int d2 = dx * dx + dy * dy;
    if (d2 == 0) return 1;
    int R = get_vision_radius();
    if (d2 > R * R) return 0;                          /* out of range */
    if (d2 > VISION_NEAR * VISION_NEAR) {              /* beyond the close ring -> must be in the cone */
        int fx = 0, fy = 0;
        switch (viewer->facing) { case 'N': fy = -1; break; case 'S': fy = 1; break;
                                  case 'E': fx = 1; break;  case 'W': fx = -1; break; default: break; }
        if (fx || fy) {                                /* has a facing: forward 120deg cone */
            int fwd = dx * fx + dy * fy;
            if (fwd <= 0) return 0;
            if (fwd * fwd * 4 < d2) return 0;
        }                                              /* facing==0 (idle): vigilant 360 within range */
    }
    return line_of_sight(w, (int)viewer->x, (int)viewer->y, tx, ty);
}

/* count opaque buildings on the line between two points (sound muffles per wall) */
static int walls_between(const World *w, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, x = x0, y = y0, n = 0;
    while (!(x == x1 && y == y1)) {
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
        if (x == x1 && y == y1) break;
        if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) break;
        { TileType tt = (TileType)w->tile[x][y]; if (tt >= T_HOME && tt <= T_POLICE) n++; }
    }
    return n;
}

/* Can `l` hear an act of the given loudness at (sx,sy)? Omnidirectional (no cone);
   reach = hearing radius x loudness; walls muffle (cost HEARING_MUFFLE each) but
   do not fully block — sound carries around corners. */
int agent_can_hear(const World *w, const Agent *l, int sx, int sy, double loudness) {
    if (loudness <= 0.0) return 0;
    double R = get_hearing_radius() * loudness;
    int dx = sx - (int)l->x, dy = sy - (int)l->y;
    double d = sqrt((double)(dx * dx + dy * dy));
    if (d > R) return 0;
    return d + walls_between(w, (int)l->x, (int)l->y, sx, sy) * (double)HEARING_MUFFLE <= R;
}

/* How loud is this act? Violence carries; stealth crime is near-silent. A serial
   killer muffles their kills (stealth scales with skill). 0..~0.9. */
double crime_loudness(const Agent *perp, const char *kind) {
    double v;
    if      (!strcmp(kind, "arson")     || !strcmp(kind, "riot"))        v = 0.9;
    else if (!strcmp(kind, "murder"))                                    v = 0.8;
    else if (!strcmp(kind, "assault")   || !strcmp(kind, "the war"))     v = 0.7;
    else if (!strcmp(kind, "robbery"))                                   v = 0.5;
    else if (!strcmp(kind, "extortion"))                                 v = 0.4;
    else if (!strcmp(kind, "vandalism"))                                 v = 0.2;
    else                                                                 v = 0.1; /* theft/burglary/dealing/trafficking */
    if (perp && perp->crime_role == CR_KILLER &&
        (!strcmp(kind, "murder") || !strcmp(kind, "assault")))
        v *= (1.0 - 0.6 * perp->crime_skill);   /* a skilled killer strikes quietly */
    return v;
}

static void witnesses_at(World *w, Agent *perp, double loudness, int *civ, int *pol) {
    int x = (int)perp->x, y = (int)perp->y; *civ = 0; *pol = 0;
    int vis = get_vision(), hear = get_hearing();
    int reach = WITNESS_RADIUS;
    if (hear) { int hr = (int)(get_hearing_radius() * loudness + 0.999); if (hr > reach) reach = hr; }
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == perp->id) continue;
        int saw = 0, heard = 0;
        if (abs((int)o->x - x) <= WITNESS_RADIUS && abs((int)o->y - y) <= WITNESS_RADIUS)
            saw = (!vis) || agent_can_see(w, o, x, y);         /* within sight range */
        if (!saw && hear) heard = agent_can_hear(w, o, x, y, loudness);  /* or heard the noise */
        if (!saw && !heard) continue;
        if (o->is_police) (*pol)++; else (*civ)++;
    }
}

/* nearest plausible drug customer (prefer existing users) */
static Agent *find_user_near(World *w, Agent *d, int radius) {
    Agent *bestA = NULL, *bestAny = NULL; double bdA = 1e18, bdAny = 1e18; int vis = get_vision();
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == d->id || o->is_police || o->arrested_ticks > 0) continue;
        if (o->crime_role == CR_DEALER || o->crime_role == CR_KINGPIN) continue;
        double dx = o->x - d->x, dy = o->y - d->y, dd = dx*dx + dy*dy;
        if (dd > (double)radius * radius) continue;
        if (vis && !agent_can_see(w, d, (int)o->x, (int)o->y)) continue;   /* deal with a buyer in sight */
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
    int civ, pol; witnesses_at(w, d, crime_loudness(d, "dealing"), &civ, &pol);
    if (pol && rng_double(&w->rng) < 0.5 - d->crime_skill * 0.3 + police_pressure(w)) {
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
    w->crime_take[CK_DEALING] += price;   /* cumulative illegal proceeds (dashboard) */
    cust->addiction = (float)clampd(cust->addiction + 0.07, 0, 1);
    cust->needs.social = clampd(cust->needs.social + 0.15, 0, 1);  /* the high */
    cust->dealer_id = d->id;
    d->crimes_committed++; d->reputation = (float)clampd(d->reputation - 0.03, -1, 1); tally(w, "dealing");
    /* street corner sales to users are NOT posted to the feed (they'd drown it) —
       only the wholesale supply chain (below) and busts/wanted notices surface. */
    if (civ && rng_double(&w->rng) < 0.3) mark_wanted(w, d, "dealing");
    if (rival && rival->id != d->id) retaliate(w, rival, d);
}

static void do_traffic(World *w, Agent *p) {
    char t[96];
    if (p->crime_role == CR_KINGPIN) {
        if (p->drug_stock >= DRUG_BATCH) return;   /* only import when the stash runs low */
        int civ, pol; witnesses_at(w, p, crime_loudness(p, "trafficking"), &civ, &pol); (void)civ;
        if (pol && rng_double(&w->rng) < 0.35 - p->crime_skill * 0.25 + police_pressure(w)) {
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
        w->crime_take[CK_TRAFFICKING] += cost;   /* cumulative illegal proceeds (dashboard) */
        snprintf(t, sizeof(t), "%.20s bought %d units wholesale from %.20s ($%.0f)", p->name, units, kp->name, cost);
        events_post(w, EV_CRIME, p->id, kp->id, (int)p->x, (int)p->y, 0.4, t);
    }
}

static void do_murder(World *w, Agent *k, Agent *victim) {
    if (!victim || !victim->alive) return;
    int civ, pol; witnesses_at(w, k, crime_loudness(k, "murder"), &civ, &pol);
    double chance = clampd(0.6 + k->crime_skill * 0.3 - civ * 0.15 - (pol ? 0.5 + police_pressure(w) : 0), 0.03, 0.97);
    char t[96];
    if (pol && rng_double(&w->rng) > chance) {
        jail(k, "murder");
        snprintf(t, sizeof(t), "%s was caught attempting murder and arrested", k->name);
        events_post(w, EV_ARREST, k->id, victim->id, (int)k->x, (int)k->y, 1.0, t);
        return;
    }
    if (rng_double(&w->rng) < chance) {
        victim->alive = 0; w->deaths++; tally(w, "murder"); k->crimes_committed++;
        if (victim->spouse_id >= 0) { Agent *sp = world_agent_by_id(w, victim->spouse_id);
            if (sp) sp->spouse_id = -1; victim->spouse_id = -1; victim->pregnant_ticks = 0; }
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
    amt *= (1.0 - 0.3 * w->sci.adoption[TECH_MEDICINE]);   /* Medicine (tech) softens wounds */
    v->injury = (float)clampd(v->injury + amt, 0, 2);
    if (v->injury >= INJURY_FATAL) {
        v->alive = 0; w->deaths++;
        if (v->spouse_id >= 0) { Agent *sp = world_agent_by_id(w, v->spouse_id);
            if (sp) sp->spouse_id = -1; v->spouse_id = -1; v->pregnant_ticks = 0; }
        char t[96];
        if (by) snprintf(t, sizeof(t), "%.26s died of injuries from %.26s (%s)", v->name, by->name, cause);
        else    snprintf(t, sizeof(t), "%.30s died of their injuries (%s)", v->name, cause);
        events_post(w, EV_DEATH, v->id, by ? by->id : -1, (int)v->x, (int)v->y, 0.9, t);
    }
}

/* count faction-mates of `a` within radius of (x,y) — "backup" in a fight */
static int allies_near(World *w, const Agent *a, int x, int y, int radius) {
    if (a->faction_id < 0) return 0;
    int c = 0;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *o = &w->agents[i];
        if (!o->alive || o->id == a->id || o->faction_id != a->faction_id || o->arrested_ticks > 0) continue;
        if (abs((int)o->x - x) <= radius && abs((int)o->y - y) <= radius) c++;
    }
    return c;
}

/* The shared combat primitive. Resolves one strike from `att` on `def` using
   fighting prowess (skill, bravery, numbers) vs. the defender's. On a win the
   defender is wounded (possibly fatally); a brave defender may wound back.
   Used by street assaults and by faction warfare alike. Returns 1 if def died. */
int combat_attack(World *w, Agent *att, Agent *def, const char *context, double base_injury) {
    if (!att || !def || !att->alive || !def->alive) return 0;
    double ax = att->x, ay = att->y;
    double atk = 0.5 + att->crime_skill * 0.6
               + (pers_has(&att->pers, TR_BRAVE) ? 0.2 : 0.0)
               + pers_crime_propensity(&att->pers) * 0.3
               + allies_near(w, att, (int)ax, (int)ay, WAR_ENGAGE_RADIUS) * 0.08
               - att->injury * 0.4;
    double dfn = 0.4 + def->crime_skill * 0.5
               + (pers_has(&def->pers, TR_BRAVE) ? 0.25 : 0.0)
               + allies_near(w, def, (int)def->x, (int)def->y, WAR_ENGAGE_RADIUS) * 0.08
               - def->injury * 0.4;
    double pwin = clampd(0.5 + (atk - dfn) * 0.4, 0.1, 0.92);
    rel_adjust(&att->rels, def->id, -0.4); rel_adjust(&def->rels, att->id, -0.5);
    att->reputation = (float)clampd(att->reputation - 0.02, -1, 1);

    if (rng_double(&w->rng) < pwin) {
        double amt = base_injury * (0.7 + att->crime_skill * 0.6);
        apply_injury(w, def, amt, context, att);
        def->needs.safety = clampd(def->needs.safety - 0.4, 0, 1);
        return !def->alive;
    }
    /* the blow is turned — a game defender strikes back */
    def->needs.safety = clampd(def->needs.safety - 0.2, 0, 1);
    if (pers_has(&def->pers, TR_BRAVE) || def->faction_id >= 0)
        apply_injury(w, att, base_injury * 0.5, context, def);
    return 0;
}

/* size the take for a property crime. Legacy (crime-wealth off): flat ranges that
 * draw from w->rng exactly as before. Wealth mode: a fraction of the target's money
 * (the nearest-richest resident when there's no explicit victim), clamped per kind —
 * so a mansion is a real score and a tenement nets pennies. Draws no rng. */
static double crime_loot(World *w, Agent *perp, Agent *target, const char *kind) {
    if (!get_crime_wealth()) {
        if      (!strcmp(kind, "theft"))     return rng_range(&w->rng, 5, 25);
        else if (!strcmp(kind, "burglary"))  return rng_range(&w->rng, 25, 90);
        else if (!strcmp(kind, "robbery"))   return rng_range(&w->rng, 15, 55);
        else if (!strcmp(kind, "extortion")) return rng_range(&w->rng, 15, 50);
        else if (!strcmp(kind, "riot"))      return rng_range(&w->rng, 5, 40);
        return 0.0;
    }
    double tw;
    if (target) tw = target->needs.money;
    else {   /* burglary / untargeted: size by the richest resident on the block */
        double bw = 0; int x = (int)perp->x, y = (int)perp->y;
        for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
            if (!o->alive || o->id == perp->id || o->is_police) continue;
            if (abs((int)o->x - x) + abs((int)o->y - y) > WITNESS_RADIUS*4) continue;
            if (o->needs.money > bw) bw = o->needs.money;
        }
        tw = bw;
    }
    double frac, lo, hi;
    if      (!strcmp(kind, "theft"))     { frac = 0.15; lo = 3; hi = 60;  }
    else if (!strcmp(kind, "burglary"))  { frac = 0.30; lo = 5; hi = 300; }
    else if (!strcmp(kind, "robbery"))   { frac = 0.35; lo = 5; hi = 200; }
    else if (!strcmp(kind, "extortion")) { frac = 0.25; lo = 5; hi = 150; }
    else if (!strcmp(kind, "riot"))      { frac = 0.10; lo = 3; hi = 60;  }
    else return 0.0;
    if (tw <= 0) return 0.0;
    double loot = frac * tw;
    if (loot < lo) loot = lo; if (loot > hi) loot = hi; if (loot > tw) loot = tw;
    return loot;
}
/* tally loot against the victim's wealth tier (poor/mid/rich vs the city median) —
 * for the dashboard; runs regardless of the crime-wealth flag. */
static void crime_tier_add(World *w, double victim_wealth, double loot) {
    double med = w->median_wealth > 0 ? w->median_wealth : 1.0;
    int tier = (victim_wealth < 0.5 * med) ? 0 : (victim_wealth > 2.0 * med) ? 2 : 1;
    w->loot_tier[tier] += loot;
    w->crimes_tier[tier]++;
}

/* Police patrol bias (Phase 4 policy knob): extra catch chance at (x,y). Mode 0
 * (default/legacy) returns 0 → behaviour unchanged. Mode 1 (money) concentrates
 * policing in affluent areas (safer rich blocks, crime displaced to poor ones);
 * mode 2 (balanced) splits between wealth and existing crime heat. */
double police_bias_bonus(const World *w, int x, int y) {
    int mode = get_police_bias();
    if (mode == 0) return 0.0;
    double val = get_neighborhoods() ? w->affluence_[x][y] / 255.0 : 0.5;
    double money_term = 0.25 * (val - 0.5) * 2.0;      /* −0.25 (poor) .. +0.25 (rich) */
    if (mode == 1) return money_term;
    double heat = w->danger_[x][y] / 255.0;            /* mode 2: balanced */
    return 0.5 * money_term + 0.5 * (0.25 * heat);
}

void crime_attempt(World *w, Agent *perp, Agent *target, const char *kind) {
    if (!strcmp(kind, "dealing"))     { do_deal(w, perp);          return; }
    if (!strcmp(kind, "trafficking")) { do_traffic(w, perp);       return; }
    if (!strcmp(kind, "murder"))      { do_murder(w, perp, target); return; }

    int x = (int)perp->x, y = (int)perp->y;
    int civ, pol; witnesses_at(w, perp, crime_loudness(perp, kind), &civ, &pol);
    double skill = perp->crime_skill;
    double witw = (!strcmp(kind, "burglary")) ? 0.03 : 0.10;   /* burglary is indoors */
    double chance = clampd(0.62 - witw * civ - (pol ? 0.55 + police_pressure(w) : 0.0)
                           - police_bias_bonus(w, x, y)   /* where police concentrate (money/crime bias) */
                           + pers_crime_propensity(&perp->pers) * 0.12 + skill * 0.25, 0.05, 0.96);
    int success = rng_double(&w->rng) < chance;
    double importance = (!strcmp(kind,"theft")||!strcmp(kind,"vandalism")) ? 0.5 : 0.85;
    char vs[48]; vs[0] = '\0'; if (target) snprintf(vs, sizeof(vs), " against %s", target->name);
    char t[96];

    if (success) {
        double loot = crime_loot(w, perp, target, kind);
        perp->needs.money += loot;
        if (loot > 0) w->crime_take[ck_index(kind)] += loot;   /* cumulative illegal proceeds (dashboard) */
        if (target) {
            double vw = target->needs.money;                   /* victim wealth before the take */
            if (loot > 0) { target->needs.money = clampd(target->needs.money - loot, 0, 1e9);
                            crime_tier_add(w, vw, loot); }
            target->needs.safety = clampd(target->needs.safety - (!strcmp(kind,"assault")?0.5:0.3), 0, 1);
            if (!strcmp(kind, "assault")) combat_attack(w, perp, target, "assault", ASSAULT_INJURY);
        } else if (loot > 0) {
            /* (D) no direct target (e.g. burglary) — take it from a nearby resident so
             * stolen money is transferred, not injected. Wealth mode robs the richest on
             * the block; legacy robs the nearest who can cover the (flat) loot. */
            Agent *v = NULL;
            if (get_crime_wealth()) {
                double bw = -1;
                for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
                    if (!o->alive || o->id == perp->id || o->is_police) continue;
                    if (abs((int)o->x - x) + abs((int)o->y - y) <= WITNESS_RADIUS*4 && o->needs.money > bw) { bw = o->needs.money; v = o; }
                }
            } else {
                int bd = 0;
                for (int i = 0; i < w->n_agents; i++) { Agent *o = &w->agents[i];
                    if (!o->alive || o->id == perp->id || o->needs.money < loot) continue;
                    int d = abs((int)o->x - x) + abs((int)o->y - y);
                    if (d <= WITNESS_RADIUS*4 && (!v || d < bd)) { v = o; bd = d; }
                }
            }
            if (v) { double vw = v->needs.money; v->needs.money = clampd(v->needs.money - loot, 0, 1e9);
                     crime_tier_add(w, vw, loot); }
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

/* Enlist members into the seeded factions so gangs/cults are real actors (and can
   wage war). Crime-prone citizens gravitate to gangs; the devout to cults. */
void factions_populate(World *w) {
    Rng *r = &w->rng;
    int gangs[MAX_FACTIONS], cults[MAX_FACTIONS], ng = 0, ncu = 0;
    for (int i = 0; i < w->n_factions; i++) {
        if (!w->factions[i].active) continue;
        if (w->factions[i].is_cult) cults[ncu++] = i; else gangs[ng++] = i;
    }
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->is_police || a->faction_id >= 0) continue;
        int crook = (a->crime_role == CR_CAREER || a->crime_role == CR_DEALER || a->crime_role == CR_KINGPIN);
        double prop = pers_crime_propensity(&a->pers);
        int fid = -1;
        if (ng > 0 && (crook ? rng_double(r) < 0.75 : (prop > 0.5 && rng_double(r) < 0.30)))
            fid = gangs[rng_int(r, ng)];
        else if (ncu > 0 && pers_faith(&a->pers) > 0.6 && rng_double(r) < 0.40)
            fid = cults[rng_int(r, ncu)];
        if (fid < 0) continue;
        Faction *f = &w->factions[fid];
        a->faction_id = fid; f->members++;
        a->needs.belonging = clampd(a->needs.belonging + 0.2, 0, 1);
        if (f->leader_id == -1) f->leader_id = a->id;
    }
    /* turf: give each gang member a territory centred on their home-ish spot */
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (a->alive && a->faction_id >= 0 && !w->factions[a->faction_id].is_cult
            && a->turf_x == 0 && a->turf_y == 0) { a->turf_x = (int)a->x; a->turf_y = (int)a->y; }
    }
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

    /* Immigration: newcomers arrive to keep a living city near its target size as
       old age and crime thin the ranks. Based on the ALIVE count (dead slots are
       reclaimed), so a high-turnover run doesn't quietly depopulate. */
    int alive = world_alive(w);
    int pop_target = get_pop_target();
    if (alive < pop_target) {
        int gap = pop_target - alive;
        int want = 3 + gap / 6;         /* people to add today (steady trickle + shortfall) */
        int added = 0, guard = 0;
        while (added < want && guard++ < want + 6) {
            if (rng_double(&w->rng) < get_family_share()) {   /* some arrivals are young families with kids */
                added += world_spawn_family(w);
                continue;
            }
            int id = world_spawn_agent(w, -1, -1);
            if (id < 0) break;
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
            added++;
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
                abs((int)p->y - (int)a->y) <= POLICE_ARREST_RADIUS) {
                if (get_vision() && !agent_can_see(w, p, (int)a->x, (int)a->y)) continue;  /* must spot them */
                cop = p; break;
            }
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
    f->war_with = -1; f->war_days = 0; f->casualties = 0;
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

void factions_war_daily(World *w);   /* defined just below */

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
    factions_war_daily(w);
}

/* count a faction's surviving, free members */
static int faction_strength(World *w, int fid) {
    if (fid < 0) return 0;
    int c = 0;
    for (int i = 0; i < w->n_agents; i++)
        if (w->agents[i].alive && w->agents[i].faction_id == fid) c++;
    return c;
}

/* Declare wars between rival factions and wind down ones that have run their
   course. Warfare is strictly faction-vs-faction — this is a city, not a nation. */
void factions_war_daily(World *w) {
    char t[112];
    for (int i = 0; i < w->n_factions; i++) {
        Faction *f = &w->factions[i];
        if (!f->active) continue;

        /* a war in progress winds down by the day, or ends if a side is spent */
        if (f->war_with >= 0) {
            Faction *e = &w->factions[f->war_with];
            if (--f->war_days <= 0 || !e->active || faction_strength(w, f->id) < 2 || faction_strength(w, e->id) < 2) {
                if (f->id < f->war_with) {   /* announce the truce once, from the lower id */
                    snprintf(t, sizeof(t), "Truce: %.24s and %.24s end their war", f->name, e->name);
                    events_post(w, EV_WAR, f->leader_id, e->leader_id, 0, 0, 0.7, t);
                }
                f->war_with = -1; f->war_days = 0;
                e->war_with = -1; e->war_days = 0;
            }
            continue;
        }

        /* otherwise a strong faction may pick a fight with a rival faction */
        if (faction_strength(w, f->id) < 3) continue;
        if (rng_double(&w->rng) >= WAR_DECLARE_CHANCE) continue;
        int tries = 4, tgt = -1;
        while (tries-- > 0) {
            int j = rng_int(&w->rng, w->n_factions);
            Faction *g = &w->factions[j];
            if (j == i || !g->active || g->war_with >= 0 || faction_strength(w, j) < 3) continue;
            /* gangs feud over turf; cults clash with gangs over the city's soul */
            tgt = j; break;
        }
        if (tgt < 0) continue;
        Faction *g = &w->factions[tgt];
        int days = rng_int_incl(&w->rng, WAR_MIN_DAYS, WAR_MAX_DAYS);
        f->war_with = tgt; f->war_days = days;
        g->war_with = i;   g->war_days = days;
        snprintf(t, sizeof(t), "WAR: %.24s declares war on %.24s", f->name, g->name);
        events_post(w, EV_WAR, f->leader_id, g->leader_id, 0, 0, 0.9, t);
    }
}

/* Soldiers of warring factions attack nearby enemies, every tick. The actual
   violence runs through the shared combat primitive, same as street crime. */
void warfare_tick(World *w) {
    int any = 0;
    for (int i = 0; i < w->n_factions; i++) if (w->factions[i].active && w->factions[i].war_with >= 0) { any = 1; break; }
    if (!any) return;

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->faction_id < 0 || a->arrested_ticks > 0) continue;
        Faction *f = &w->factions[a->faction_id];
        if (!f->active || f->war_with < 0) continue;
        if (rng_double(&w->rng) >= WAR_ENGAGE_CHANCE) continue;

        /* find the nearest enemy-faction fighter in range */
        Agent *enemy = NULL; int bd = WAR_ENGAGE_RADIUS * 2 + 1;
        for (int k = 0; k < w->n_agents; k++) {
            Agent *o = &w->agents[k];
            if (!o->alive || o->faction_id != f->war_with || o->arrested_ticks > 0) continue;
            int d = abs((int)o->x - (int)a->x) + abs((int)o->y - (int)a->y);
            if (d <= WAR_ENGAGE_RADIUS && d < bd) { bd = d; enemy = o; }
        }
        if (!enemy) continue;

        int killed = combat_attack(w, a, enemy, "the war", COMBAT_INJURY);
        if (killed) {
            f->casualties++;              /* the enemy faction lost a soldier */
            char t[112];
            snprintf(t, sizeof(t), "%.24s fell in the %.20s-%.20s war", enemy->name,
                     f->name, w->factions[f->war_with].name);
            events_post(w, EV_WAR, a->id, enemy->id, (int)enemy->x, (int)enemy->y, 0.8, t);
        }
    }
}

/* Law & order: a surge in crime provokes a police crackdown (temporary rise in
   catch rates), which fades after a couple of days. On day change. */
void law_daily(World *w) {
    int today = w->crimes - w->crimes_prev_day;
    w->crimes_prev_day = w->crimes;
    if (w->crackdown_days > 0) w->crackdown_days--;
    if (today >= CRACKDOWN_THRESHOLD && w->crackdown_days == 0) {
        w->crackdown_days = CRACKDOWN_DAYS;
        char t[96]; snprintf(t, sizeof(t), "Police declare a crackdown after %d crimes in a day", today);
        events_post(w, EV_ARREST, -1, -1, WORLD_W/2, WORLD_H/2, 0.7, t);
        fprintf(stderr, "[law] day %d: crackdown (%d crimes)\n", w->day, today);
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
static int cmp_dbl_asc(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
/* per-capita household wealth for an agent (own money if unhoused) */
static double agent_pcwealth(const World *w, const Agent *a) {
    if (a->home_id >= 0 && w->buildings[a->home_id].hh_size > 0)
        return w->buildings[a->home_id].hh_wealth / w->buildings[a->home_id].hh_size;
    return a->needs.money;
}

void economy_daily(World *w) {
    Economy *e = &w->econ;
    int alive = 0; double money_sum = 0;

    /* reference wealth for RELATIVE social class: the city's median per-capita
     * household wealth. Tiers are scaled to this so the distribution stays a
     * pyramid as the city grows richer, instead of everyone saturating at the top. */
    double wref = 0.0;
    { static double tmp[MAX_AGENTS]; int n = 0;
      for (int i = 0; i < w->n_agents; i++) if (w->agents[i].alive) tmp[n++] = agent_pcwealth(w, &w->agents[i]);
      if (n) { qsort(tmp, n, sizeof(double), cmp_dbl_asc); wref = tmp[n/2]; } }
    if (wref < 1.0) wref = 1.0;   /* guard against an all-broke city */
    w->median_wealth = wref;      /* published for crime wealth-tier classification */

    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive) continue;
        alive++;

        /* ── rent: a tenant pays their landlord; the rent is transferred, not burned ── */
        if (a->home_id >= 0) {
            Building *hb = &w->buildings[a->home_id];
            if (hb->owner_id >= 0 && hb->owner_id != a->id) {
                double rent = RENT_TO_LANDLORD * e->goods_price;
                if (rent > a->needs.money) rent = a->needs.money;
                a->needs.money -= rent;
                a->day_income -= rent;                      /* rent paid (net income) */
                Agent *ll = world_agent_by_id(w, hb->owner_id);
                if (ll && ll->alive) { ll->needs.money += rent; ll->day_income += rent; }  /* rent received */
            }
        }
        /* municipal upkeep (a modest sink) */
        double upkeep = a->needs.money * UPKEEP_FRACTION;
        a->needs.money = clampd(a->needs.money - upkeep, 0.0, 1e9);
        a->day_income -= upkeep;                            /* cost of living (net income) */

        /* ── credit: interest accrues; repay when flush, borrow when destitute ── */
        /* Banking (tech) lowers the rate as it spreads */
        double interest = DAILY_INTEREST * (1.0 - 0.5 * w->sci.adoption[TECH_BANKING]);
        if (a->debt > 0.0) {
            a->day_income -= a->debt * interest;            /* interest expense (net income) */
            a->debt *= (1.0 + interest);
            if (a->needs.money > LOW_MONEY * 2) {
                double repay = a->needs.money - LOW_MONEY * 2;
                if (repay > a->debt) repay = a->debt;
                a->needs.money -= repay; a->debt -= repay;
                if (a->debt < 0.5) a->debt = 0.0;
            }
        }
        if (a->needs.money < MEAL_PRICE && a->debt < DEBT_CEILING) {
            a->needs.money += LOAN_AMOUNT; a->debt += LOAN_AMOUNT;
        }

        if (a->addiction > 0.0f) a->addiction = (float)clampd(a->addiction - 0.015, 0, 1);  /* habit fades without use */
        if (a->injury > 0.0f) a->injury = (float)clampd(a->injury - 0.08, 0, 2);            /* wounds slowly heal */
        a->reputation = (float)clampd(a->reputation + (a->reputation > 0 ? -0.01 : 0.01), -1, 1);  /* drift to neutral */

        /* recompute social tier from household wealth + reputation + role + debt.
         * Class is household-based AND relative: tiers scale to the city median
         * (wref), so a low-wage spouse in a rich home reads well-off and the tiers
         * don't all saturate as the city grows richer. An absolute poverty floor
         * (LOW_MONEY) still marks the truly destitute. */
        double pcwealth = agent_pcwealth(w, a);
        int tier = 2;
        if (pcwealth > wref * 1.5) tier++;
        if (pcwealth > wref * 3.0) tier++;
        if (pcwealth < wref * 0.5) tier--;
        if (pcwealth < LOW_MONEY) tier--;   /* absolute destitution floor */
        if (a->reputation > 0.3f) tier++;
        if (a->reputation < -0.3f) tier--;
        if (a->crime_role == CR_KINGPIN) tier++;
        if (a->debt > DEBT_CEILING * 0.75) tier--;
        if (tier < 0) tier = 0; if (tier > 4) tier = 4;
        a->status = (unsigned char)tier;

        money_sum += a->needs.money;

        /* ambient: becoming destitute (can't afford a meal) */
        if (a->needs.money < MEAL_PRICE && !a->broke_flagged) {
            a->broke_flagged = 1;
            char t[96]; snprintf(t, sizeof(t), "%s is destitute", a->name);
            events_post(w, EV_HARDSHIP, a->id, -1, (int)a->x, (int)a->y, 0.4, t);
        } else if (a->needs.money >= LOW_MONEY) {
            a->broke_flagged = 0;
        }
    }

    /* ── child-rearing: dependent children cost their parents daily upkeep, which is
       transferred to the child (an allowance to live on). Strapped parents pay what
       they can and the shortfall leaves the child a little worse off. ── */
    double cc = get_child_cost();
    if (cc > 0.0) {
        for (int i = 0; i < w->n_agents; i++) {
            Agent *c = &w->agents[i];
            if (!c->alive || c->age >= AGE_WORK) continue;        /* only dependents */
            Agent *pr[2]; int np = 0;
            Agent *m = c->mother_id >= 0 ? world_agent_by_id(w, c->mother_id) : NULL;
            Agent *f = c->father_id >= 0 ? world_agent_by_id(w, c->father_id) : NULL;
            if (m && m->alive) pr[np++] = m;
            if (f && f->alive) pr[np++] = f;
            if (np == 0) continue;                                 /* orphan: no one to charge */
            double paid = 0;
            for (int p = 0; p < np; p++) {
                double share = (cc - paid) / (np - p);             /* split the remainder evenly */
                if (share > pr[p]->needs.money) share = pr[p]->needs.money;
                pr[p]->needs.money -= share; paid += share;
                pr[p]->day_income -= share;                        /* child upkeep paid (net income) */
            }
            c->needs.money += paid;                                /* the child's allowance */
            c->day_income += paid;                                 /* allowance received (net income) */
            if (paid < cc * 0.5) {                                 /* under-provided: a child goes without */
                c->needs.hunger = clampd(c->needs.hunger - 0.08, 0, 1);
                c->needs.safety = clampd(c->needs.safety - 0.04, 0, 1);
            }
        }
    }

    /* ── markets settle for the day ── */
    double avg_money = alive ? money_sum / alive : 0.0;
    e->goods_price = clampd(0.6 + avg_money / 700.0, 0.6, 3.0);   /* a richer city is a pricier one */
    e->wage_mult   = clampd(0.6 + e->goods_price * 0.5, 0.6, 1.6); /* wages chase the cost of living */
    e->wage_mult  *= (1.0 + 0.2 * w->sci.adoption[TECH_TOOLING]);   /* better tools raise output/pay */
}

/* theory prerequisite before each tech can be attempted (chain prereq = prior tech) */
static const int TECH_THEORY_REQ[TECH_COUNT] = { 1, 2, 3, 4, 6, 8 };

/* Knowledge & technology (on day change): educated scholars accrue research into
   theories; theories (+ the prior tech) unlock a discovery; discoveries diffuse. */
void knowledge_daily(World *w) {
    Science *s = &w->sci;

    /* scholarship from educated adults (clergy weighted); track the top aptitude */
    double scholarship = 0.0, top = 0.0, lit = 0.0; int adults = 0;
    for (int i = 0; i < w->n_agents; i++) {
        Agent *a = &w->agents[i];
        if (!a->alive || a->age < AGE_ADULT) continue;
        adults++; lit += a->education;
        if (a->arrested_ticks > 0) continue;
        double contrib = a->intellect * a->education;
        if (a->occupation == OCC_CLERGY) contrib *= CLERGY_SCHOLAR;
        scholarship += contrib;
        double apt = a->intellect * (0.4 + 0.6 * a->education);
        if (apt > top) top = apt;
    }
    lit = adults ? lit / adults : 0.0;

    double printing = s->discovered[TECH_PRINTING] ? s->adoption[TECH_PRINTING] : 0.0;
    s->research += scholarship * get_research_rate() * (1.0 + printing * 0.8);

    /* research matures into theories (rising cost each time) */
    double cost = THEORY_BASE * pow(THEORY_GROWTH, s->theories);
    while (s->research >= cost) {
        s->research -= cost; s->theories++;
        char t[96]; snprintf(t, sizeof(t), "The city's scholars established a new theory (#%d)", s->theories);
        events_post(w, EV_FACTION, -1, -1, WORLD_W/2, WORLD_H/2, 0.5, t);
        cost = THEORY_BASE * pow(THEORY_GROWTH, s->theories);
    }

    /* discover the frontier tech once its prereqs are met */
    for (int k = 0; k < TECH_COUNT; k++) {
        if (s->discovered[k]) continue;
        if (k > 0 && !s->discovered[k-1]) break;      /* chain: need the prior tech */
        if (s->theories < TECH_THEORY_REQ[k]) break;  /* knowledge prerequisite */
        if (rng_double(&w->rng) < DISCOVERY_BASE * (0.3 + top)) {
            s->discovered[k] = 1; s->adoption[k] = 0.05f;
            char t[96]; snprintf(t, sizeof(t), "Breakthrough: the city discovered %s", tech_name(k));
            events_post(w, EV_FACTION, -1, -1, WORLD_W/2, WORLD_H/2, 0.85, t);
        }
        break;                                        /* one frontier attempt per day */
    }

    /* discovered tech diffuses into use (faster in a literate city, and with writing) */
    double writing = s->discovered[TECH_WRITING] ? s->adoption[TECH_WRITING] : 0.0;
    for (int k = 0; k < TECH_COUNT; k++)
        if (s->discovered[k] && s->adoption[k] < 1.0f)
            s->adoption[k] = (float)clampd(s->adoption[k] + ADOPT_RATE * (0.5 + lit + writing*0.5) * (1.0 - s->adoption[k]), 0, 1);
}
