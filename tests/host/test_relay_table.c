/* Сгенерированная таблица реле пригодна для прошивки (FW-119, FW-239, А7). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "relay_table.h"

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static unsigned popcount_words(const uint32_t *w)
{
    unsigned n = 0;
    for (unsigned i = 0; i < RELAY_WORDS; i++) {
        uint32_t x = w[i];
        while (x) {
            n += x & 1u;
            x >>= 1;
        }
    }
    return n;
}

int main(void)
{
    CHECK(RELAY_COUNT == 81);
    CHECK(strncmp(RELAY_TABLE_CHECKSUM, "sha256:", 7) == 0);
    CHECK(strlen(RELAY_TABLE_CHECKSUM) == 7 + 64);

    /* номера уникальны, имена уникальны, каждое реле имеет адрес */
    uint8_t seen[RELAY_MAX_NUMBER + 1] = {0};
    for (unsigned i = 0; i < RELAY_COUNT; i++) {
        const struct relay_desc *r = &relay_table[i];
        CHECK(r->number >= 1 && r->number <= RELAY_MAX_NUMBER);
        CHECK(!seen[r->number]);
        seen[r->number] = 1;
        CHECK(r->addr.kind == RELAY_ADDR_PIN || r->addr.kind == RELAY_ADDR_SR);
        if (r->addr.kind == RELAY_ADDR_SR) {
            CHECK(r->addr.a <= 1 && r->addr.b <= 3 && r->addr.c <= 7);
        }
        for (unsigned j = i + 1; j < RELAY_COUNT; j++) {
            CHECK(strcmp(r->name, relay_table[j].name) != 0);
        }
    }

    /* каждая группа — минимум два реле, маска группы согласована с group_mask реле */
    for (unsigned g = 0; g < RELAY_GROUP_COUNT; g++) {
        CHECK(popcount_words(relay_groups[g].mask) >= 2);
        for (unsigned i = 0; i < RELAY_COUNT; i++) {
            unsigned idx = relay_table[i].number - 1u;
            bool in_mask = (relay_groups[g].mask[idx / 32] >> (idx % 32)) & 1u;
            bool in_desc = (relay_table[i].group_mask >> g) & 1u;
            CHECK(in_mask == in_desc);
        }
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_relay_table: OK");
    return 0;
}
