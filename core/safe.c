#include "safe.h"

#include <string.h>

#include "config.h"
#include "cfg.h"
#include "core.h"
#include "errlog.h"
#include "hal.h"
#include "signals.h"
#include "sections.h"
#include "dut.h"
#include "psu.h"
#include "sr.h"

struct desired desired;
struct observed observed;

const char *safe_reason_name(enum safe_reason r)
{
    switch (r) {
    case SAFE_REASON_COMMAND: return "SAFE";
    case SAFE_REASON_CONNECT: return "CONNECT";
    case SAFE_REASON_CLIENT_LOST: return "CLIENT_LOST";
    case SAFE_REASON_RESET: return "RESET";
    default: return "NONE";
    }
}

const char *safe_step_name(enum safe_step_result r)
{
    switch (r) {
    case STEP_OK: return "OK";
    case STEP_FAILED: return "FAIL";
    case STEP_SILENT: return "SILENT";
    case STEP_SKIPPED: return "SKIPPED";
    default: return "?";
    }
}

/* Шаг 1: оба БП выключить, код ЦАП в минимум. */
static enum safe_step_result step_psu_off(void)
{
    return psu_safe_off() ? STEP_OK : STEP_FAILED;
}

/* Шаг 2: последовательность гашения контроллеру нагрузок — модуль loads, шаг 7 плана. */
static enum safe_step_result step_loads_off(void)
{
    desired.load_dac = 0;
    memset(desired.load_pwm, 0, sizeof desired.load_pwm);
    return STEP_SILENT; /* контроллер не опрашивается: погашение по его таймауту */
}

/* Шаг 3: снять линии C*. */
static enum safe_step_result step_dut_off(void)
{
    dut_off_all();
    return STEP_OK;
}

/* Шаг 4: ожидание снятия DC-OK ограничено временем; остальные шаги идут и при ошибке. */
static enum safe_step_result step_dcok_fall(void)
{
    return psu_wait_dcok_off() ? STEP_OK : STEP_FAILED;
}

/* Шаг 5: реле выключить — сначала L*, затем остальные. */
static enum safe_step_result step_relays_off(void)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < RELAY_COUNT; i++) {
            const struct relay_desc *r = &relay_table[i];
            bool is_load = r->name[0] == 'L' && r->name[1] >= '1' && r->name[1] <= '9';
            if ((pass == 0) != is_load) {
                continue;
            }
            relay_drive(r, false);
        }
        sr_write(0);
        sr_write(1);
    }
    memset(desired.relays, 0, sizeof desired.relays);
    return STEP_OK;
}

/* Шаг 6: секции выключить. */
static enum safe_step_result step_sections_off(void)
{
    section_off(0);
    section_off(1);
    return STEP_OK;
}

bool safe_enter(enum safe_reason reason)
{
    static enum safe_step_result (*const steps[SAFE_STEPS])(void) = {
        step_psu_off, step_loads_off, step_dut_off, step_dcok_fall, step_relays_off, step_sections_off,
    };
    struct safe_report *rep = &observed.safe;
    uint32_t t0 = hal_millis();
    rep->seq++;
    rep->reason = reason;
    rep->complete = true;
    for (int i = 0; i < SAFE_STEPS; i++) {
        rep->steps[i] = steps[i]();
        if (rep->steps[i] == STEP_FAILED) {
            rep->complete = false;
        }
    }
    rep->duration_ms = hal_millis() - t0;
    observed.in_safe_state = true;
    cfg_set_erase_allowed(rep->complete);
    observed.generation++;
    log_event("SAFE %s %s %lu ms", safe_reason_name(reason), rep->complete ? "OK" : "INCOMPLETE",
              (unsigned long)rep->duration_ms);
    return rep->complete;
}
