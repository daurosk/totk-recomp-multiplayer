/* The economy: see sv_econ.h. economy.txt holds one fact per line:
 *   wallet ACCOUNT RUPEES
 *   spent ACCOUNT LIGHTS SEEDS
 *   light KEY | seed KEY      (a Light or Korok seed some player found; KEY is the game's key hash in hex)
 */
#include "sv_econ.h"

#include <stdlib.h>
#include <string.h>

typedef struct econ_acct {
    uint64_t id;
    uint32_t rupees, lights_spent, seeds_spent;
    double budget;     /* rupees this account may still gain this minute */
    uint64_t budget_at;
} econ_acct;

static struct {
    sv_config cfg;
    econ_acct *a;
    int na, ca;
    uint32_t found[2][SV_LEDGER_MAX];
    int nfound[2];
    int dirty;
    uint64_t saved_at;
} E;

static econ_acct *acct(uint64_t id, int make) {
    for (int i = 0; i < E.na; i++)
        if (E.a[i].id == id) return &E.a[i];
    if (!make) return NULL;
    if (E.na == E.ca) {
        E.ca = E.ca ? E.ca * 2 : 16;
        E.a = realloc(E.a, (size_t)E.ca * sizeof *E.a);
        if (!E.a) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    econ_acct *a = &E.a[E.na++];
    memset(a, 0, sizeof *a);
    a->id = id;
    a->rupees = (uint32_t)E.cfg.start_rupees;
    return a;
}

static int has_found(int kind, uint32_t key) {
    for (int i = 0; i < E.nfound[kind]; i++)
        if (E.found[kind][i] == key) return 1;
    return 0;
}

static int add_found(int kind, uint32_t key) {
    if (!key || has_found(kind, key) || E.nfound[kind] >= SV_LEDGER_MAX) return 0;
    E.found[kind][E.nfound[kind]++] = key;
    return 1;
}

void sv_econ_open(const sv_config *c) {
    free(E.a);
    memset(&E, 0, sizeof E);
    E.cfg = *c;
    char path[640], line[256];
    snprintf(path, sizeof path, "%s/economy.txt", g_store.dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int no = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        unsigned long long id, x, y;
        unsigned key;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "wallet %llu %llu", &id, &x) == 2 && id) {
            acct(id, 1)->rupees = x > SV_RUPEES_MAX ? SV_RUPEES_MAX : (uint32_t)x;
        } else if (sscanf(line, "spent %llu %llu %llu", &id, &x, &y) == 3 && id) {
            econ_acct *a = acct(id, 1);
            a->lights_spent = x > SV_LEDGER_MAX ? SV_LEDGER_MAX : (uint32_t)x;
            a->seeds_spent = y > SV_LEDGER_MAX ? SV_LEDGER_MAX : (uint32_t)y;
        } else if (sscanf(line, "light %x", &key) == 1) {
            add_found(SV_LEDGER_LIGHT, key);
        } else if (sscanf(line, "seed %x", &key) == 1) {
            add_found(SV_LEDGER_SEED, key);
        } else {
            sv_log("economy.txt:%d: not a line of the economy (ignored)", no);
        }
    }
    fclose(f);
}

void sv_econ_save(void) {
    if (!E.dirty) return;
    char tmp[660], dst[640];
    snprintf(dst, sizeof dst, "%s/economy.txt", g_store.dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        sv_log("cannot write %s", tmp);
        return;
    }
    fprintf(f, "# rupees, what each account spent of the shared Lights and Korok seeds, and every one found.\n"
               "# Written by the server.\n");
    for (int i = 0; i < E.na; i++) {
        fprintf(f, "wallet %llu %u\n", (unsigned long long)E.a[i].id, E.a[i].rupees);
        if (E.a[i].lights_spent || E.a[i].seeds_spent)
            fprintf(f, "spent %llu %u %u\n", (unsigned long long)E.a[i].id, E.a[i].lights_spent, E.a[i].seeds_spent);
    }
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < E.nfound[k]; i++) fprintf(f, "%s %x\n", k ? "seed" : "light", E.found[k][i]);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || sv_replace_file(tmp, dst) != 0) sv_log("cannot replace %s", dst);
    E.dirty = 0;
}

void sv_econ_tick(uint64_t now_us) {
    if (E.dirty && now_us - E.saved_at >= 1000000ull) {
        E.saved_at = now_us;
        sv_econ_save();
    }
}

uint32_t sv_wallet_get(uint64_t account) {
    econ_acct *a = acct(account, 0);
    return a ? a->rupees : (uint32_t)E.cfg.start_rupees;
}

void sv_wallet_set(uint64_t account, uint32_t rupees) {
    acct(account, 1)->rupees = rupees > SV_RUPEES_MAX ? SV_RUPEES_MAX : rupees;
    E.dirty = 1;
}

int32_t sv_wallet_apply(uint64_t account, int cause, int32_t delta, uint64_t now_us, uint32_t *rupees) {
    econ_acct *a = acct(account, 1);
    int64_t d = delta;
    if ((cause == OW_WALLET_SPEND && d > 0) || (cause == OW_WALLET_EARN && d < 0)) d = 0;
    if (d < 0) {
        if (-d > (int64_t)a->rupees) d = -(int64_t)a->rupees;
    } else if (d > 0) {
        double per_min = E.cfg.wallet_earn_per_minute;
        if (!a->budget_at) {
            a->budget = per_min;
            a->budget_at = now_us;
        }
        a->budget += (double)(now_us - a->budget_at) / 1e6 * (per_min / 60.0);
        a->budget_at = now_us;
        if (a->budget > per_min) a->budget = per_min;
        if (d > E.cfg.wallet_earn_max) d = E.cfg.wallet_earn_max;
        if (d > (int64_t)SV_RUPEES_MAX - a->rupees) d = (int64_t)SV_RUPEES_MAX - a->rupees;
        if ((double)d > a->budget) d = (int64_t)a->budget;
        a->budget -= (double)d;
    }
    if (d) {
        a->rupees = (uint32_t)((int64_t)a->rupees + d);
        E.dirty = 1;
    }
    *rupees = a->rupees;
    return (int32_t)d;
}

int sv_ledger_find(int kind, uint32_t key) {
    if (!add_found(kind, key)) return 0;
    E.dirty = 1;
    return 1;
}

uint32_t sv_ledger_spend(uint64_t account, int kind, uint32_t count) {
    econ_acct *a = acct(account, 1);
    uint32_t *spent = kind == SV_LEDGER_LIGHT ? &a->lights_spent : &a->seeds_spent;
    uint32_t found = (uint32_t)E.nfound[kind];
    uint32_t left = found - (*spent > found ? found : *spent);
    if (count > left) count = left;
    if (count) {
        *spent += count;
        E.dirty = 1;
    }
    return count;
}

void sv_ledger_get(uint64_t account, sv_ledger_view *v) {
    econ_acct *a = acct(account, 0);
    v->lights_found = (uint32_t)E.nfound[SV_LEDGER_LIGHT];
    v->seeds_found = (uint32_t)E.nfound[SV_LEDGER_SEED];
    v->lights_spent = a ? a->lights_spent : 0;
    v->seeds_spent = a ? a->seeds_spent : 0;
}
