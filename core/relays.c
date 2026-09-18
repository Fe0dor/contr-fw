/* FW-118..129: one owner (superloop), complete images, break before make. */
#include "relays.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "hal.h"
#include "signals.h"
#include "sr.h"
#include "state.h"

bool relays_bit(const uint32_t map[RELAY_WORDS], unsigned number)
{
    return (map[(number - 1) / 32] >> ((number - 1) % 32)) & 1u;
}
static void bit_set(uint32_t map[RELAY_WORDS], unsigned number, bool on)
{
    uint32_t mask = 1u << ((number - 1) % 32);
    if (on) map[(number - 1) / 32] |= mask;
    else map[(number - 1) / 32] &= ~mask;
}
bool relays_conflict(const uint32_t map[RELAY_WORDS], char *fields, size_t size)
{
    /* Group order and ascending relay numbers match generator expand_pairs(). */
    for (int g = 0; g < RELAY_GROUP_COUNT; ++g) {
        unsigned a = 0, b = 0;
        for (unsigned n = 1; n <= RELAY_MAX_NUMBER; ++n) {
            if (relays_bit(map, n) && relays_bit(relay_groups[g].mask, n)) {
                if (!a) a = n; else { b = n; break; }
            }
        }
        if (!b) continue;
        const char *an = "", *bn = "";
        for (int i = 0; i < RELAY_COUNT; ++i) {
            if (relay_table[i].number == a) an = relay_table[i].name;
            if (relay_table[i].number == b) bn = relay_table[i].name;
        }
        char pair[96];
        snprintf(pair, sizeof pair, "%s__%s__%s", relay_groups[g].name, an, bn);
        for (char *p = pair; *p; ++p)
            if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
        snprintf(fields, size, "%s,%s,%s", pair, an, bn);
        return true;
    }
    return false;
}
static void write_map(const uint32_t map[RELAY_WORDS])
{
    bool chains[2] = {false, false};
    for (int i = 0; i < RELAY_COUNT; ++i) {
        const struct relay_desc *r = &relay_table[i];
        bool on = relays_bit(map, r->number);
        if (on == relays_bit(desired.relays, r->number)) continue;
        relay_drive(r, on);
        if (r->addr.kind == RELAY_ADDR_SR) chains[r->addr.a] = true;
    }
    for (int i = 0; i < 2; ++i) if (chains[i]) sr_write((uint8_t)i);
    memcpy(desired.relays, map, sizeof desired.relays);
}
void cmd_route(struct cmdctx *c)
{
    bool low = strcmp(c->cmd->name, "ROUT:LOW") == 0;
    bool replace = strcmp(c->cmd->name, "ROUT:SET") == 0;
    bool all_off = strcmp(c->cmd->name, "ROUT:LOW:ALL") == 0;
    uint32_t target[RELAY_WORDS] = {0};
    if (!replace && !all_off) memcpy(target, desired.relays, sizeof target);
    for (int a = 0; a < c->argc; ++a) {
        const struct relay_desc *r = NULL;
        for (int i = 0; i < RELAY_COUNT; ++i)
            if (strcasecmp(c->argv[a], relay_table[i].name) == 0) { r = &relay_table[i]; break; }
        if (!r) { resp_err(c, ERR_RANGE, NULL); return; }
        bit_set(target, r->number, !low);
    }
    char fields[128];
    if (relays_conflict(target, fields, sizeof fields)) { resp_err(c, ERR_INTERLOCK, fields); return; }
    uint32_t intermediate[RELAY_WORDS];
    bool making = false;
    for (int w = 0; w < RELAY_WORDS; ++w) {
        intermediate[w] = desired.relays[w] & target[w];
        making |= (target[w] & ~desired.relays[w]) != 0;
    }
    if (memcmp(target, desired.relays, sizeof target) == 0) { resp_ok(c); return; }
    observed.in_safe_state = false;
    write_map(intermediate);
    if (making) {
        /* Service communications without executing another setter. Also covers
         * separate LOW then HIGH commands and a wrap of the millisecond tick. */
        /* One extra tick prevents truncation to 19.x ms on a 1 ms clock. */
        uint32_t deadline = hal_millis() + RELAY_BREAK_MS + 1u;
        bool had_client = observed.client_connected;
        do {
            svc_poll();
            if (core_abort_requested() || (had_client && !hal_net_client_connected())) {
                resp_err(c, ERR_ABORTED, NULL);
                return; /* queued SAFE / disconnect handles the remaining outputs */
            }
        } while ((int32_t)(hal_millis() - deadline) < 0);
        write_map(target);
    }
    resp_ok(c);
}
void cmd_route_stat(struct cmdctx *c)
{
    bool first = true;
    for (int i = 0; i < RELAY_COUNT; ++i) {
        if (!relays_bit(desired.relays, relay_table[i].number)) continue;
        if (!first) resp_putc(c, ',');
        resp_puts(c, relay_table[i].name);
        first = false;
    }
    resp_end(c);
}
