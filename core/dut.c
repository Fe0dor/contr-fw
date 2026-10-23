/* FW-162..170: released lines are inputs, never actively driven low. */
#include "dut.h"
#include <ctype.h>
#include <string.h>
#include "config.h"
#include "errlog.h"
#include "hal.h"
#include "signals.h"
#include "state.h"

#define DUT_COUNT 20u
static const enum signal_id lines[DUT_COUNT] = {
    SIG_C1, SIG_C2, SIG_C3, SIG_C4, SIG_C5, SIG_C6, SIG_C7, SIG_C8, SIG_C9, SIG_C10,
    SIG_C11, SIG_C12, SIG_C13, SIG_C14, SIG_C15, SIG_C16, SIG_C17, SIG_C18, SIG_C19, SIG_C20
};
static uint32_t manual, candidate, since[DUT_COUNT], last_poll;
static bool intervention;

void dut_init(void)
{
    desired.dut_lines = manual = candidate = 0;
    intervention = false;
    last_poll = hal_millis();
    memset(since, 0, sizeof since);
}

static void set_lines(uint32_t mask, bool on)
{
    uint32_t next = on ? desired.dut_lines | mask : desired.dut_lines & ~mask;
    uint32_t changed = next ^ desired.dut_lines;
    for (unsigned i = 0; i < DUT_COUNT; i++) {
        uint32_t bit = 1u << i;
        if (!(changed & bit)) continue;
        signal_set(lines[i], on);
        /* Driven voltage is not a manual press. Start a fresh observation on release. */
        if (manual & bit) log_event("DUT C%u MANUAL -> ON", i + 1);
        manual &= ~bit;
        candidate &= ~bit;
        since[i] = hal_millis();
    }
    desired.dut_lines = next;
}

void dut_off_all(void)
{
    set_lines((1u << DUT_COUNT) - 1u, false);
    /* Also restore pins changed by bringup DBG commands, without clearing manual evidence. */
    for (unsigned i = 0; i < DUT_COUNT; i++) signal_set(lines[i], false);
}

void dut_poll(void)
{
    uint32_t now = hal_millis();
    if ((uint32_t)(now - last_poll) < DUT_POLL_MS) return;
    last_poll = now;
    for (unsigned i = 0; i < DUT_COUNT; i++) {
        uint32_t bit = 1u << i;
        if (desired.dut_lines & bit) continue;
        bool high = signal_read(lines[i]);
        if (high != ((candidate & bit) != 0)) {
            candidate ^= bit;
            since[i] = now;
        }
        if (high != ((manual & bit) != 0) && (uint32_t)(now - since[i]) >= DUT_DEBOUNCE_MS) {
            manual ^= bit;
            intervention = true;
            observed.generation++;
            log_event("DUT C%u %s", i + 1, high ? "MANUAL" : "OFF");
        }
    }
}

static bool eq(const char *a, const char *b)
{
    while (*a && *b && toupper((unsigned char)*a) == *b) { a++; b++; }
    return !*a && !*b;
}

void cmd_dut_set(struct cmdctx *c)
{
    bool on = eq(c->argv[c->argc - 1], "ON");
    if (!on && !eq(c->argv[c->argc - 1], "OFF")) { resp_err(c, ERR_RANGE, NULL); return; }
    uint32_t mask = 0;
    for (int a = 0; a < c->argc - 1; a++) {
        unsigned i;
        for (i = 0; i < DUT_COUNT; i++) if (eq(c->argv[a], signal_table[lines[i]].name)) break;
        if (i == DUT_COUNT || (mask & (1u << i))) { resp_err(c, ERR_RANGE, NULL); return; }
        mask |= 1u << i;
    }
    set_lines(mask, on);
    resp_ok(c);
}

void cmd_dut_off(struct cmdctx *c) { dut_off_all(); resp_ok(c); }

void cmd_dut_stat(struct cmdctx *c)
{
    /* Snapshot before streaming the response: transport polling can observe new presses. */
    uint32_t on = desired.dut_lines, pressed = manual;
    bool flag = intervention;
    intervention = false;
    for (unsigned i = 0; i < DUT_COUNT; i++) {
        uint32_t bit = 1u << i;
        resp_printf(c, "%sC%u,%s", i ? ";" : "", i + 1,
                    on & bit ? "ON" : pressed & bit ? "MANUAL" : "OFF");
    }
    resp_printf(c, ";INTERVENTION,%u;PWROK,", flag ? 1 : 0);
#if PWROK_PRESENT
    const enum signal_id ready[] = {SIG_PWROK1, SIG_PWROK2, SIG_PWROK3};
    for (unsigned i = 0; i < 3; i++)
        resp_printf(c, "%s%u", i ? "," : "", signal_read(ready[i]) != (PWROK_ACTIVE_LOW != 0));
#else
    resp_puts(c, "NA,NA,NA");
#endif
    resp_end(c);
}
