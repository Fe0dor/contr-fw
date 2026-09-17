/* Две структуры состояния (А3): desired — что установила прошивка, observed — что случилось. */
#ifndef CORE_STATE_H
#define CORE_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "relay_table.h"

#define SAFE_STEPS 6

enum safe_reason {
    SAFE_REASON_NONE = 0,
    SAFE_REASON_COMMAND,   /* SAFE по TCP или с консоли */
    SAFE_REASON_CONNECT,   /* новое соединение */
    SAFE_REASON_CLIENT_LOST,
    SAFE_REASON_RESET,     /* порядок инициализации при подаче питания */
};

enum safe_step_result {
    STEP_OK = 0,
    STEP_FAILED,
    STEP_SILENT,   /* подтверждён молчанием (FW-207) */
    STEP_SKIPPED,  /* бюджет исчерпан: выполнено без ожидания */
};

struct safe_report {
    uint32_t seq;
    enum safe_reason reason;
    enum safe_step_result steps[SAFE_STEPS];
    uint32_t duration_ms;
    bool complete;
};

/* Копируется целиком; умещается в десятки байт. */
struct desired {
    uint32_t relays[RELAY_WORDS];   /* битовая карта включённых реле */
    uint8_t sr_image[2][4];         /* образы цепочек SR0 (3 регистра) и SR1 (4) */
    bool psu_rsp_on;
    bool psu_hlg_on;
    uint16_t psu_dac;
    bool section_on[2];
    uint32_t dut_lines;             /* биты C1…C20 */
    uint16_t load_dac;
    uint32_t load_pwm_freq;
    uint8_t load_pwm[6];
};

/* Никогда не копируется. */
struct observed {
    uint32_t generation;
    bool in_safe_state;             /* с последнего входа не было команд установки */
    struct safe_report safe;
    uint32_t reset_cause;           /* HAL_RST_* */
    bool client_connected;
};

extern struct desired desired;
extern struct observed observed;

#endif /* CORE_STATE_H */
