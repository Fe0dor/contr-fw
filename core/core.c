/* Суперцикл и каналы (А1, А5). Колбэки сети только кладут байты в кольцо канала;
 * строки собираются в svc_poll(), исполняются по одной в core_step(). */
#include "core.h"

#include <ctype.h>
#include <string.h>

#include "cfg.h"
#include "cmd.h"
#include "config.h"
#include "errlog.h"
#include "hal.h"
#include "netcfg.h"
#include "safe.h"
#include "signals.h"
#include "dut.h"
#include "psu.h"
#include "sections.h"
#include "sr.h"
#include "state.h"
#include "update.h"

#define RING_SIZE 512u
#define LINE_QUEUE 4u
#define WRITE_STALL_MS 500u

struct line {
    char text[CMD_LINE_MAX + 1];
    bool busy;     /* пришла во время исполнения команды → ERR:BUSY */
    bool overflow; /* длиннее буфера → ERR:RANGE */
    bool safe;
};

struct channel {
    volatile uint8_t ring[RING_SIZE];
    /* Capture execution state at receipt, before the line queue can delay parsing. */
    bool ring_busy[RING_SIZE];
    volatile size_t head, tail;   /* head читает главный контекст, tail пишет источник */
    char partial[CMD_LINE_MAX + 1];
    size_t partial_len;
    bool partial_overflow;
    bool ring_overflow;
    uint16_t recovered_safe; /* complete SAFE lines rescued before buffer discard */
    struct line queue[LINE_QUEUE];
    size_t q_head, q_count;
    bool open;
};

static struct channel channels[CH_COUNT];
static bool executing;
static bool abort_requested;
static volatile bool accept_pending;
static volatile bool client_lost;
static enum channel_id next_channel;
static bool reply_started;
static uint32_t reply_started_ms;
static bool console_resync;
static uint32_t led_changed_ms;
static bool led_on;

/* ---- кольца ---- */

/* Inspect complete lines without consuming input. extra includes the byte
 * that would overflow the ring, so a SAFE terminated exactly there is retained. */
static unsigned buffered_safe_count(const struct channel *c, bool extra, uint8_t last)
{
    char text[CMD_LINE_MAX + 1];
    size_t len = c->partial_len;
    bool invalid = c->partial_overflow;
    unsigned count = 0;
    memcpy(text, c->partial, len);
    size_t available = (c->tail + RING_SIZE - c->head) % RING_SIZE;
    for (size_t i = 0; i < available + (extra ? 1u : 0u); i++) {
        uint8_t b = i == available ? last : c->ring[(c->head + i) % RING_SIZE];
        if (b == '\n') {
            text[len] = '\0';
            if (!invalid && cmd_is_safe_line(text)) count++;
            len = 0;
            invalid = false;
        } else if (b == 0) {
            invalid = true;
        } else if (b != '\r') {
            if (len < CMD_LINE_MAX) text[len++] = (char)b;
            else invalid = true;
        }
    }
    return count;
}

static void ring_push(struct channel *c, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (c->ring_overflow) {
            if (buf[i] != '\n') continue;
            c->ring_overflow = false;
        }
        size_t next = (c->tail + 1) % RING_SIZE;
        if (next == c->head) {
            /* Preserve complete SAFE requests before discarding corrupted input.
             * Other lines cannot be reconstructed after overflow. */
            unsigned safe_count = buffered_safe_count(c, true, buf[i]);
            unsigned room = UINT16_MAX - c->recovered_safe;
            c->recovered_safe += (uint16_t)(safe_count < room ? safe_count : room);
            if (safe_count && executing) abort_requested = true;
            c->head = c->tail;
            c->partial_len = 0;
            c->partial_overflow = true;
            c->ring_overflow = buf[i] != '\n';
            if (!c->ring_overflow) {
                c->ring[c->tail] = '\n';
                c->ring_busy[c->tail] = executing;
                c->tail = next;
            }
            continue;
        }
        c->ring[c->tail] = buf[i];
        c->ring_busy[c->tail] = executing;
        c->tail = next;
    }
}

static bool ring_pop(struct channel *c, uint8_t *out, bool *busy)
{
    if (c->head == c->tail) {
        return false;
    }
    *out = c->ring[c->head];
    *busy = c->ring_busy[c->head];
    c->head = (c->head + 1) % RING_SIZE;
    return true;
}

static void channel_reset(struct channel *c)
{
    c->head = c->tail = 0;
    c->partial_len = 0;
    c->partial_overflow = false;
    c->ring_overflow = false;
    c->recovered_safe = 0;
    c->q_head = c->q_count = 0;
}

/* ---- колбэки сети (только очередь) ---- */

bool core_net_on_accept(void)
{
    if (observed.client_connected || accept_pending || client_lost) {
        return false;
    }
    channel_reset(&channels[CH_TCP]);
    accept_pending = true;
    return true;
}

void core_net_on_data(const uint8_t *buf, size_t len)
{
    ring_push(&channels[CH_TCP], buf, len);
}

void core_net_on_closed(void)
{
    client_lost = true;
}

bool core_client_connected(void)
{
    return observed.client_connected;
}

/* ---- сборка строк ---- */

static void enqueue_line(struct channel *c, bool received_busy)
{
    if (c->q_count == LINE_QUEUE) {
        return; /* очередь полна: строка подождёт в partial — не бывает, partial уже собран */
    }
    struct line *l = &c->queue[(c->q_head + c->q_count) % LINE_QUEUE];
    c->partial[c->partial_len] = '\0';
    memcpy(l->text, c->partial, c->partial_len + 1);
    l->overflow = c->partial_overflow;
    l->safe = !l->overflow && cmd_is_safe_line(l->text);
    l->busy = received_busy && !l->safe;
    if (l->safe && executing) {
        abort_requested = true;
    }
    c->q_count++;
    c->partial_len = 0;
    c->partial_overflow = false;
}

static void pump_channel(struct channel *c)
{
    while (c->recovered_safe && c->q_count < LINE_QUEUE) {
        struct line *l = &c->queue[(c->q_head + c->q_count) % LINE_QUEUE];
        strcpy(l->text, "SAFE");
        l->safe = true;
        l->busy = l->overflow = false;
        c->q_count++;
        c->recovered_safe--;
        if (executing) abort_requested = true;
    }
    uint8_t b;
    bool received_busy;
    while (c->q_count < LINE_QUEUE && ring_pop(c, &b, &received_busy)) {
        if (b == '\n') {
            enqueue_line(c, received_busy);
        } else if (b == 0) {
            c->partial_overflow = true; /* never execute a C-string prefix */
        } else if (b == '\r') {
            continue;
        } else if (c->partial_len < CMD_LINE_MAX) {
            c->partial[c->partial_len++] = (char)b;
        } else {
            c->partial_overflow = true;
        }
    }
    if (executing && !abort_requested && buffered_safe_count(c, false, 0)) {
        abort_requested = true;
    }
}

static bool dequeue_line(struct channel *c, struct line *out)
{
    if (c->q_count == 0) {
        return false;
    }
    *out = c->queue[c->q_head];
    c->q_head = (c->q_head + 1) % LINE_QUEUE;
    c->q_count--;
    return true;
}

/* ---- служебный шаг ---- */

void svc_poll(void)
{
    hal_wdt_kick();
    /* Blue LD2 (PB7) indicates service-loop progress at 2 Hz.
     * PB7 must remain an input when the SR0 loop is fitted. */
#if !SR0_LOOP_PRESENT
    uint32_t now = hal_millis();
    if ((uint32_t)(now - led_changed_ms) >= 250u) {
        led_changed_ms = now;
        led_on = !led_on;
        signal_set(SIG_SR_TEST_IN, led_on);
    }
#endif
    hal_net_poll();
    uint8_t buf[64];
    size_t n;
    while ((n = hal_console_read(buf, sizeof buf)) > 0) {
        ring_push(&channels[CH_CONSOLE], buf, n);
    }
    pump_channel(&channels[CH_TCP]);
    pump_channel(&channels[CH_CONSOLE]);
    dut_poll();
    psu_poll();
    netcfg_poll();
    upd_poll();
}

bool core_abort_requested(void)
{
    return abort_requested;
}

enum wait_result wait_until(uint32_t deadline_ms)
{
    for (;;) {
        svc_poll();
        if (abort_requested) {
            return WAIT_ABORTED;
        }
        if ((int32_t)(hal_millis() - deadline_ms) >= 0) {
            return WAIT_DONE;
        }
    }
}

enum wait_result wait_for(bool (*cond)(void *), void *arg, uint32_t deadline_ms, bool *timed_out)
{
    *timed_out = false;
    for (;;) {
        svc_poll();
        if (abort_requested) {
            return WAIT_ABORTED;
        }
        if (cond(arg)) {
            return WAIT_DONE;
        }
        if ((int32_t)(hal_millis() - deadline_ms) >= 0) {
            *timed_out = true;
            return WAIT_DONE;
        }
    }
}

bool channel_write(enum channel_id ch, const uint8_t *buf, size_t len)
{
    if (!reply_started) {
        reply_started = true;
        reply_started_ms = hal_millis();
    }
    size_t sent = 0;
    uint32_t last_progress = hal_millis();
    while (sent < len) {
        size_t n;
        if (ch == CH_TCP) {
            if (!observed.client_connected || client_lost) {
                return false;
            }
            if ((uint32_t)(hal_millis() - reply_started_ms) >= WRITE_STALL_MS) {
                hal_net_close_client();
                core_net_on_closed();
                return false;
            }
            n = hal_net_send(buf + sent, len - sent);
        } else {
            if (console_resync) {
                /* Terminate the old, possibly partial frame before any new
                 * reply. The client must discard its timed-out transaction. */
                if (hal_console_write((const uint8_t *)"\n", 1) == 1) {
                    console_resync = false;
                }
                n = 0;
            } else {
                n = hal_console_write(buf + sent, len - sent);
            }
        }
        sent += n;
        if (n) last_progress = hal_millis();
        if (sent < len) {
            svc_poll();
            /* A partial TCP frame cannot be resumed as a new reply. Close the
             * session on SAFE or a stalled reader; core_step then enters SAFE.
             * UART keeps frame order, but a failed transmitter cannot hang us. */
            if ((ch == CH_TCP && abort_requested)
                || (ch == CH_TCP && (uint32_t)(hal_millis() - reply_started_ms) >= WRITE_STALL_MS)
                || (uint32_t)(hal_millis() - last_progress) >= WRITE_STALL_MS) {
                if (ch == CH_TCP) {
                    hal_net_close_client();
                    core_net_on_closed();
                } else {
                    console_resync = true;
                }
                return false;
            }
        }
    }
    return true;
}

/* ---- старт ---- */

void board_early_init(void)
{
    /* FW-209: 1. PWRON_A/B выключено */
    signal_set(SIG_PWRON_A, false);
    signal_set(SIG_PWRON_B, false);
    /* 2. CS_PWR неактивен */
    hal_gpio_config(signal_table[SIG_CS_PWR].port, signal_table[SIG_CS_PWR].pin, HAL_PIN_OUT, HAL_PULL_NONE,
                    CS_ACTIVE_LOW != 0);
    /* 3. JTAG на PB4 */
    hal_jtag_release_pb4();
    /* 4. линии C* входами с подтяжкой вниз */
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (signal_table[i].dir == SIGNAL_DUT) {
            signal_set((enum signal_id)i, false);
        }
    }
    /* остальные сигналы по описанию, выводы реле — выходы 0 */
    signals_configure_rest();
    relay_pin_config_all_off();
    /* 5. выключить цепочки без ожиданий; полный образ после учёта trial. */
    /* Quiesce external shift registers without waiting for SPI. */
    for (uint8_t chain=0; chain<HAL_SR_CHAINS; chain++) {
        hal_sr_ctrl(chain, HAL_SR_OE, false);
        hal_sr_ctrl(chain, HAL_SR_SRCLR, true);
        hal_sr_latch(chain);
    }
    memset(desired.relays, 0, sizeof desired.relays);
}

void core_init(void)
{
    for (int i = 0; i < CH_COUNT; i++) {
        channel_reset(&channels[i]);
    }
    channels[CH_CONSOLE].open = true;
    channels[CH_TCP].open = false;
    executing = false;
    next_channel = CH_CONSOLE;
    reply_started = false;
    console_resync = false;
    abort_requested = false;
    accept_pending = false;
    client_lost = false;
    memset(&observed, 0, sizeof observed);
    errlog_init();
    hal_wdt_start(WDT_PERIOD_MS);
    observed.reset_cause = hal_reset_cause();
    cfg_init();
    upd_boot_check();
#ifdef CONTR_HIL_COMPACT_ON_BOOT
    if (upd_get_state()==UPD_TRIAL) {
        cfg_set_erase_allowed(true);
        int rc=cfg_compact();
        log_event("HIL CFG compact %d",rc);
        cfg_set_erase_allowed(false);
    }
#endif
    /* Trial count must be durable before any clock/SPI/I2C wait can hang. */
#ifdef CONTR_HIL_HANG_BEFORE_HAL
    for (;;) {} /* HIL only: watchdog must return the previous bank on next boot */
#endif
    sr_start();
    hal_init();
    led_changed_ms = hal_millis();
    led_on = false;
    signal_set(SIG_LD1, false);
#if !SR0_LOOP_PRESENT
    const struct signal_desc *led = &signal_table[SIG_SR_TEST_IN];
    hal_gpio_config(led->port, led->pin, HAL_PIN_OUT, HAL_PULL_NONE, false);
#endif
    dut_init();
    bool psu_ready=psu_init();
    section_off(0);
    section_off(1);
    /* FW-209: DAC minimum is already written; loadboard shutdown is step 7. */
    observed.safe.reason = SAFE_REASON_RESET;
    observed.safe.seq = 0;
    for (int i = 0; i < SAFE_STEPS; i++) {
        observed.safe.steps[i] = STEP_OK;
    }
    observed.safe.steps[1] = STEP_SILENT;
    observed.safe.steps[0]=psu_ready?STEP_OK:STEP_FAILED;
    observed.safe.complete = psu_ready;
    observed.in_safe_state = true;
    cfg_set_erase_allowed(psu_ready);
    netcfg_start();
    const char *cause = (observed.reset_cause & HAL_RST_IWDG) ? "watchdog"
                        : (observed.reset_cause & HAL_RST_PIN) ? "pin"
                        : (observed.reset_cause & HAL_RST_SW)  ? "software"
                        : (observed.reset_cause & HAL_RST_POR) ? "power-on"
                                                               : "unknown";
    log_event("reset: %s", cause);
    if (observed.reset_cause & HAL_RST_IWDG) {
        err_push(ERR_NONE, "reset: watchdog");
    }
}

/* ---- суперцикл ---- */

static void execute_line(enum channel_id ch, struct line *l)
{
    reply_started = false;
    if (l->overflow) {
        cmd_reply_overflow(ch);
        return;
    }
    if (l->busy) {
        cmd_reply_busy(ch);
        return;
    }
    executing = true;
    abort_requested = false;
    cmd_execute(ch, l->text);
    executing = false;
    if (abort_requested) {
        /* SAFE пришёл во время команды: сама строка SAFE стоит в очереди и исполнится следом */
        abort_requested = false;
    }
}

void core_step(void)
{
    svc_poll();

    if (client_lost) {
        client_lost = false;
        bool had_client = observed.client_connected || accept_pending;
        accept_pending = false;
        if (had_client) {
            observed.client_connected = false;
            channel_reset(&channels[CH_TCP]);
            channels[CH_TCP].open = false;
            log_event("client lost");
            safe_enter(SAFE_REASON_CLIENT_LOST);
        }
    }
    if (accept_pending) {
        accept_pending = false;
        channels[CH_TCP].open = true;
        observed.client_connected = true;
        log_event("client connected");
        safe_enter(SAFE_REASON_CONNECT); /* до чтения первой команды (FW-101) */
        pump_channel(&channels[CH_TCP]);
    }

    struct line l;
    /* Fair arbitration preserves each channel's FIFO reply order. A queued
     * console command no longer depends on a gap in TCP traffic. */
    for (unsigned i = 0; i < CH_COUNT; i++) {
        enum channel_id ch = (enum channel_id)((next_channel + i) % CH_COUNT);
        if (ch == CH_TCP && !observed.client_connected) continue;
        if (dequeue_line(&channels[ch], &l)) {
            next_channel = (enum channel_id)((ch + 1) % CH_COUNT);
            execute_line(ch, &l);
            return;
        }
    }
}

void core_run(void)
{
    for (;;) {
        core_step();
    }
}
