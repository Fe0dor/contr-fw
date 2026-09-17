/* Сгенерировано tools/gen_board.py из board/rev1/relays.yaml — не править руками (FW-239). */
#ifndef GEN_RELAY_TABLE_H
#define GEN_RELAY_TABLE_H

#include <stdint.h>

#define RELAY_TABLE_VERSION "rev1.1"
#define RELAY_TABLE_CHECKSUM "sha256:968891d679c902bc09aecebc42f84bc055cf5c80b4795240e6b5176b0936bd70"
#define RELAY_COUNT 81
#define RELAY_MAX_NUMBER 81
#define RELAY_WORDS 3
#define RELAY_GROUP_COUNT 12

/* Адрес реле: вывод MCU (a = порт 0=A…, b = номер вывода) или выход цепочки
 * сдвиговых регистров (a = цепочка, b = регистр от MCU, c = выход 0=A…7=H). */
enum relay_addr_kind { RELAY_ADDR_PIN = 0, RELAY_ADDR_SR = 1 };

struct relay_addr {
    uint8_t kind;
    uint8_t a;
    uint8_t b;
    uint8_t c;
};

struct relay_desc {
    const char *name;      /* каноническое имя (FW-112) */
    uint8_t number;        /* постоянный номер (FW-244) */
    const char *block;     /* блок описания платы */
    const char *address;   /* адрес как в описании платы */
    struct relay_addr addr;
    uint16_t group_mask;   /* бит i — членство в relay_groups[i] */
};

/* Битовая карта реле: бит (number - 1) в словах по 32 (архитектура А7). */
struct relay_group {
    const char *name;
    uint32_t mask[RELAY_WORDS];
};

extern const struct relay_desc relay_table[RELAY_COUNT];
extern const struct relay_group relay_groups[RELAY_GROUP_COUNT];

#endif /* GEN_RELAY_TABLE_H */
