#ifndef CORE_RELAYS_H
#define CORE_RELAYS_H
#include "cmd.h"
#include "relay_table.h"
bool relays_bit(const uint32_t map[RELAY_WORDS], unsigned number);
/* Validates before touching hardware. Fields contain the canonical pair and names. */
bool relays_conflict(const uint32_t map[RELAY_WORDS], char *fields, size_t size);
void cmd_route(struct cmdctx *c);
void cmd_route_stat(struct cmdctx *c);
#endif
