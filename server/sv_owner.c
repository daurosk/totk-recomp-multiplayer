/* The owner rules (sv_owner.h; docs/online-wire.md section 9.1 numbers them). */
#include "sv_owner.h"

#include <stddef.h>

static const svo_game *game_of(const svo_game *games, int n, uint64_t account) {
    if (!account) return NULL;
    for (int i = 0; i < n; i++)
        if (games[i].account == account) return &games[i];
    return NULL;
}

/* The nearest present candidate other than `not`, or NULL. */
static const svo_game *nearest_candidate(const svo_game *games, int n, uint64_t not) {
    const svo_game *best = NULL;
    for (int i = 0; i < n; i++) {
        const svo_game *g = &games[i];
        if (!g->present || g->away || !g->candidate || g->account == not) continue;
        if (!best || g->dist < best->dist) best = g;
    }
    return best;
}

static int pickable(uint8_t class_) {
    return class_ == OW_CLASS_ITEM || class_ == OW_CLASS_WORLD_OBJECT || class_ == OW_CLASS_DEVICE;
}

int svo_in_fight(const svo_actor *a, const svo_game *games, int n, uint64_t now) {
    if (a->rules & OW_CR_LOCK)
        for (int i = 0; i < n; i++)
            if (games[i].present && games[i].dist <= SVO_NEAR_M) return 1;
    return (a->rules & (OW_CR_FIGHT | OW_CR_LOCK)) && a->last_hit && now - a->last_hit < SVO_FIGHT_HIT;
}

uint32_t svo_hold_ms(uint8_t class_, uint32_t rules, uint8_t cause) {
    (void)cause;
    if (rules & OW_CR_FIXED) return 0xFFFFFFFFu;
    if (pickable(class_)) return 0; /* a pick-up from a far owner may move it at once */
    return (uint32_t)(SVO_FAR_HOLD / 1000);
}

static void give(svo_result *r, const svo_actor *a, uint64_t to, uint8_t cause) {
    r->change = 1;
    r->to = to;
    r->cause = cause;
    r->hold_ms = to ? svo_hold_ms(a->class_, a->rules, cause) : 0;
    r->kept = SVO_KEPT_NONE;
}

static void keep(svo_result *r, int why) {
    r->change = 0;
    r->kept = why;
}

/* Rule 1, for an actor without an owner or whose owner is away. */
static void no_owner(const svo_actor *a, const svo_game *games, int n, uint64_t now, int owner_gone,
                     const svo_game *o, svo_result *r) {
    const svo_game *c = nearest_candidate(games, n, a->owner);
    if (!c || c->dist > SVO_NEAR_M) {
        /* An owner that left the room for good loses the actor to nobody; an away one keeps it until a candidate
         * qualifies, so a quick reconnect finds it. */
        if (a->owner && !o) give(r, a, 0, OW_OWNER_LEFT);
        else keep(r, SVO_KEPT_ORPHAN_FAR);
        return;
    }
    if (now - a->unowned_since < SVO_ORPHAN_WAIT)
        for (int i = 0; i < n; i++) {
            const svo_game *g = &games[i];
            if (g != c && g->present && !g->away && !g->gave_up && g->dist + SVO_ORPHAN_MARGIN_M < c->dist) {
                keep(r, SVO_KEPT_ORPHAN_NEARER);
                return;
            }
        }
    give(r, a, c->account, owner_gone || a->left_by_owner ? OW_OWNER_LEFT : OW_OWNER_OFFER);
}

void svo_decide(const svo_actor *a, const svo_game *games, int n, uint64_t now, svo_result *r) {
    keep(r, SVO_KEPT_NONE);
    r->to = 0;
    r->cause = 0;
    r->hold_ms = 0;
    const svo_game *o = game_of(games, n, a->owner);
    int owner_gone = a->owner && (!o || o->away);

    if ((a->rules & OW_CR_FIXED) && a->owner && o) {
        keep(r, SVO_KEPT_FIXED);
        return;
    }
    /* Rule 10: a hold takes it at once, and nothing else moves it while it lasts or just after. */
    const svo_game *h = game_of(games, n, a->holder);
    if (h) {
        if (a->owner != a->holder) give(r, a, a->holder, OW_OWNER_HOLD);
        else keep(r, SVO_KEPT_HELD);
        return;
    }
    if (a->owner && !owner_gone && now < a->hold_until) {
        keep(r, SVO_KEPT_HELD);
        return;
    }
    /* Rules 1 and 9. */
    if (!a->owner || owner_gone) {
        no_owner(a, games, n, now, owner_gone, o, r);
        return;
    }
    if (a->rules & OW_CR_QUEST) {
        keep(r, SVO_KEPT_QUEST);
        return;
    }
    if (!o->present) { /* paused, not yet for long: away covers the rest */
        keep(r, SVO_KEPT_PAUSED);
        return;
    }
    int fight = svo_in_fight(a, games, n, now);
    /* Rule 5: an actor that stays where it is. Who runs it matters only once a player touches it, and a touch moves
     * it by rule 4 (a fight by rule 7), so rule 3 waits for every class. Rule 2 waits only for things that do not act
     * by themselves: an enemy may stand still just because its far owner's game no longer runs it, while a game that
     * can no longer run an object releases it or falls silent (rule 9). */
    int still = now - a->moved_at >= SVO_STILL_AFTER;
    int passive = pickable(a->class_) || a->class_ == OW_CLASS_MAP_OBJECT || (a->rules & OW_CR_STILL);
    const svo_game *c = nearest_candidate(games, n, a->owner);
    if (!c) return;
    /* The first owner counts as a change: an actor just handed out waits like one just moved. */
    int recent_far = now - a->last_change < SVO_FAR_HOLD;
    int recent_close = now - a->last_change < SVO_CLOSER_HOLD;

    /* Rule 2, owner far, with rule 7's longer reach in a fight and rule 6's sleep. */
    float far_m = fight ? SVO_FIGHT_FAR_M : SVO_NEAR_M;
    int far_asked = c->dist <= SVO_NEAR_M && c->dist + SVO_FAR_MARGIN_M < o->dist;
    if (o->dist > SVO_NEAR_M && o->dist <= far_m && far_asked) {
        keep(r, SVO_KEPT_FIGHT);
        return;
    }
    if (o->dist > far_m) {
        if (!far_asked) keep(r, SVO_KEPT_FAR_MARGIN);
        else if (a->asleep) keep(r, SVO_KEPT_ASLEEP);
        else if (still && passive) keep(r, SVO_KEPT_STILL);
        else if (recent_far) keep(r, SVO_KEPT_RECENT);
        else give(r, a, c->account, OW_OWNER_FAR);
        return;
    }
    if (fight) {
        keep(r, SVO_KEPT_FIGHT);
        return;
    }
    /* Rule 4, pick-up. */
    if (pickable(a->class_) && c->dist <= SVO_PICKUP_M && o->dist > SVO_PICKUP_M && o->dist > c->dist) {
        if (o->dist <= SVO_PICKUP_BOTH_M && recent_close) keep(r, SVO_KEPT_RECENT);
        else give(r, a, c->account, OW_OWNER_PICKUP);
        return;
    }
    /* Rule 3, clearly closer, limited by rules 5 and 6. */
    if (c->dist * 2.0f <= o->dist && c->dist + SVO_CLOSER_MARGIN_M <= o->dist) {
        if (a->asleep) keep(r, SVO_KEPT_ASLEEP);
        else if (still) keep(r, SVO_KEPT_STILL);
        else if (recent_close) keep(r, SVO_KEPT_RECENT);
        else give(r, a, c->account, OW_OWNER_CLOSER);
    }
}

const char *svo_kept_name(int kept) {
    static const char *const names[SVO_KEPT_COUNT] = {
        "nothing to do", "fixed", "held", "quest", "fight", "asleep", "still", "changed lately",
        "candidate not near or nearer enough", "a nearer game may offer", "no candidate within 120 m", "owner paused"};
    return kept >= 0 && kept < SVO_KEPT_COUNT ? names[kept] : "?";
}

const char *svo_cause_name(uint8_t cause) {
    static const char *const names[] = {"offer", "owner far", "clearly closer", "pick-up", "owner left", "hold", "host"};
    return cause <= OW_OWNER_LAST ? names[cause] : "?";
}
