/* Сгенерировано tools/gen_board.py из board/rev1/signals.yaml — не править руками (FW-239). */
#ifndef GEN_SIGNAL_TABLE_H
#define GEN_SIGNAL_TABLE_H

#include <stdint.h>

#define SIGNAL_COUNT 39

/* dir: выход, вход, линия изделия (вход с подтяжкой вниз, при команде — выход 1) */
enum signal_dir { SIGNAL_OUT = 0, SIGNAL_IN = 1, SIGNAL_DUT = 2 };
enum signal_pull { SIGNAL_PULL_NONE = 0, SIGNAL_PULL_UP = 1, SIGNAL_PULL_DOWN = 2 };

enum signal_id {
    SIG_PWRON_A,
    SIG_PWRON_B,
    SIG_PWRON_RSP,
    SIG_CS_PWR,
    SIG_HLG,
    SIG_RESETA0,
    SIG_RESETA1,
    SIG_RESETB0,
    SIG_RESETB1,
    SIG_PWROK1,
    SIG_PWROK2,
    SIG_PWROK3,
    SIG_PWROK4,
    SIG_PWROK5,
    SIG_RSP_DC_OK,
    SIG_RSP_ALARM,
    SIG_SR_TEST_IN,
    SIG_USER,
    SIG_LD1,
    SIG_C1,
    SIG_C2,
    SIG_C3,
    SIG_C4,
    SIG_C5,
    SIG_C6,
    SIG_C7,
    SIG_C8,
    SIG_C9,
    SIG_C10,
    SIG_C11,
    SIG_C12,
    SIG_C13,
    SIG_C14,
    SIG_C15,
    SIG_C16,
    SIG_C17,
    SIG_C18,
    SIG_C19,
    SIG_C20,
};

struct signal_desc {
    const char *name;
    const char *address;
    uint8_t port;   /* 0 = A */
    uint8_t pin;
    uint8_t dir;    /* enum signal_dir */
    uint8_t pull;   /* enum signal_pull */
    uint8_t init;   /* уровень выхода при старте; для входов 0 */
};

extern const struct signal_desc signal_table[SIGNAL_COUNT];

#endif /* GEN_SIGNAL_TABLE_H */
