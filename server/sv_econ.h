/* The room's shared economy: each account's rupees and the Lights ledger.
 *
 * Rupees belong to an account. A game never sets them: it asks for a change and the server accepts it or clamps it
 * (sv_wallet_apply). The ledger counts Lights of Blessing and Korok seeds: one found by anyone counts for every
 * player, while what a player spends is that player's own (sv_ledger_find, sv_ledger_spend). Both are kept in
 * economy.txt in the data folder and written at most once a second after a change. The wire side is the economy
 * add-on of docs/online-wire.md; the rules are described for hosts in docs/server.md ("The economy").
 */
#ifndef SV_ECON_H
#define SV_ECON_H

#include <stdint.h>

#include "sv.h"

#define SV_RUPEES_MAX 999999u
/* The most finds of one kind the ledger keeps: more than the game has, so a hostile client cannot grow it. */
#define SV_LEDGER_MAX 2048

enum { SV_LEDGER_LIGHT, SV_LEDGER_SEED };

typedef struct sv_ledger_view {
    uint32_t lights_found, lights_spent, seeds_found, seeds_spent;
} sv_ledger_view;

/* Loads economy.txt from the data folder (a missing file is an empty economy). */
void sv_econ_open(const sv_config *c);
/* Writes the file now when something changed. */
void sv_econ_save(void);
/* Writes it when something changed and a second has passed since the last write. */
void sv_econ_tick(uint64_t now_us);

uint32_t sv_wallet_get(uint64_t account);
/* A game asked to change the account's rupees by delta for cause (OW_WALLET_*). Returns the change applied, which is
 * 0 for a wrong sign (a spend that adds, an earn that takes), at most the balance for a take, at most the room up to
 * SV_RUPEES_MAX for a gain, at most wallet_earn_max in one request and, over a minute, wallet_earn_per_minute.
 * *rupees is the balance after it. */
int32_t sv_wallet_apply(uint64_t account, int cause, int32_t delta, uint64_t now_us, uint32_t *rupees);
/* Console and tests: sets a balance outright. */
void sv_wallet_set(uint64_t account, uint32_t rupees);

/* A game found a Light or a Korok seed, named by its key. Returns 1 when nobody had found that one before. */
int sv_ledger_find(int kind, uint32_t key);
/* The account spent count of a kind. Returns how many the ledger accepted: never more than found minus spent. */
uint32_t sv_ledger_spend(uint64_t account, int kind, uint32_t count);
void sv_ledger_get(uint64_t account, sv_ledger_view *v);

#endif
