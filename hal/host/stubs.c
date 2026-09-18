/* Заглушки железа для хоста: виртуальное время, журнал вызовов, модели GPIO, цепочек,
 * SPI A, I²C, watchdog, flash, сброса (А2). Каналы — в mem_channels.c / sock_channels.c. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hal_host.h"

static struct {
    uint32_t now_ms;
    char journal[HOST_JOURNAL_MAX][96];
    size_t journal_n;
    struct host_pin pins[11][16];
    uint8_t sr_shift[HAL_SR_CHAINS][4];
    uint8_t sr_out[HAL_SR_CHAINS][4];
    bool sr_srclr[HAL_SR_CHAINS], sr_oe[HAL_SR_CHAINS];
    host_i2c_fn i2c;
    uint8_t spia_reply[64];
    size_t spia_reply_len;
    bool reset_requested;
    uint32_t reset_cause;
    uint8_t bank[2][HAL_BANK_SIZE];
    uint8_t active_bank, boot_bank;
    struct hal_optbytes opt;
    uint8_t uid[12];
    bool button;
    unsigned wdt_kicks;
    unsigned flash_ops;
} h;

static void journal(const char *fmt, ...)
{
    if (h.journal_n >= HOST_JOURNAL_MAX) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h.journal[h.journal_n++], sizeof h.journal[0], fmt, ap);
    va_end(ap);
}

void host_mem_channels_reset(void);

void host_reset_all(void)
{
    memset(&h, 0, sizeof h);
    memset(h.bank, 0xFF, sizeof h.bank);
    h.active_bank = 1;
    h.boot_bank = 1;
    h.opt.ndbank = false;
    h.opt.ndboot = false;
    h.opt.iwdg_sw = false;
    for (int i = 0; i < 12; i++) {
        h.uid[i] = (uint8_t)(0x10 + i);
    }
    for (unsigned c = 0; c < HAL_SR_CHAINS; c++) {
        h.sr_srclr[c] = false;
        h.sr_oe[c] = false;
    }
    host_mem_channels_reset();
}

/* ---- время и сброс ---- */

void host_advance_ms(uint32_t ms) { h.now_ms += ms; }
void host_set_ms(uint32_t ms) { h.now_ms = ms; }
uint32_t hal_millis(void) { return h.now_ms; }
void hal_delay_us(uint32_t us) { (void)us; }
void hal_init(void) { journal("hal_init"); }

void hal_reset(void)
{
    journal("reset");
    h.reset_requested = true;
}

bool host_reset_requested(void) { return h.reset_requested; }
void host_set_reset_cause(uint32_t cause) { h.reset_cause = cause; }
uint32_t hal_reset_cause(void) { return h.reset_cause; }
void host_set_uid(const uint8_t uid[12]) { memcpy(h.uid, uid, 12); }
void hal_uid(uint8_t out[12]) { memcpy(out, h.uid, 12); }

/* ---- журнал ---- */

size_t host_journal_count(void) { return h.journal_n; }
const char *host_journal_at(size_t i) { return i < h.journal_n ? h.journal[i] : ""; }
void host_journal_clear(void) { h.journal_n = 0; }

int host_journal_find(const char *prefix, size_t from)
{
    size_t n = strlen(prefix);
    for (size_t i = from; i < h.journal_n; i++) {
        if (strncmp(h.journal[i], prefix, n) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- GPIO ---- */

static const char *mode_name(enum hal_pin_mode m) { return m == HAL_PIN_OUT ? "out" : "in"; }
static const char *pull_name(enum hal_pull p) { return p == HAL_PULL_UP ? "up" : p == HAL_PULL_DOWN ? "down" : "none"; }

struct host_pin *host_pin(uint8_t port, uint8_t pin)
{
    return &h.pins[port % 11][pin % 16];
}

void host_gpio_set_input(uint8_t port, uint8_t pin, bool level)
{
    struct host_pin *p = host_pin(port, pin);
    if (p->mode == HAL_PIN_IN) {
        p->level = level;
    }
}

void hal_gpio_config(uint8_t port, uint8_t pin, enum hal_pin_mode mode, enum hal_pull pull, bool level)
{
    struct host_pin *p = host_pin(port, pin);
    p->configured = true;
    p->mode = mode;
    p->pull = pull;
    if (mode == HAL_PIN_OUT) {
        p->level = level;
    } else if (pull != HAL_PULL_NONE) {
        p->level = pull == HAL_PULL_UP; /* вход без внешнего источника — по подтяжке */
    }
    journal("gpio_config P%c%u %s %s %d", 'A' + port, pin, mode_name(mode), pull_name(pull), level ? 1 : 0);
}

void hal_gpio_write(uint8_t port, uint8_t pin, bool level)
{
    struct host_pin *p = host_pin(port, pin);
    if (p->mode == HAL_PIN_OUT) {
        p->level = level;
    }
    journal("gpio_write P%c%u %d", 'A' + port, pin, level ? 1 : 0);
}

bool hal_gpio_read(uint8_t port, uint8_t pin) { return host_pin(port, pin)->level; }
void hal_jtag_release_pb4(void) { journal("jtag_release"); hal_gpio_config(1, 4, HAL_PIN_IN, HAL_PULL_DOWN, false); }
bool hal_button_pressed(void) { return h.button; }
void host_set_button(bool pressed) { h.button = pressed; }

/* ---- цепочки SR ---- */

void hal_sr_ctrl(uint8_t chain, enum hal_sr_line line, bool active)
{
    if (line == HAL_SR_SRCLR) {
        h.sr_srclr[chain] = active;
        if (active) {
            memset(h.sr_shift[chain], 0, 4);
        }
    } else {
        h.sr_oe[chain] = active;
    }
    journal("sr_ctrl %u %s %d", chain, line == HAL_SR_SRCLR ? "srclr" : "oe", active ? 1 : 0);
}

void hal_sr_shift(uint8_t chain, const uint8_t *image, size_t len)
{
    memcpy(h.sr_shift[chain], image, len > 4 ? 4 : len);
    char hex[9] = "";
    for (size_t i = 0; i < len && i < 4; i++) {
        snprintf(hex + 2 * i, 3, "%02X", image[i]);
    }
    journal("sr_shift %u %s", chain, hex);
}

void hal_sr_latch(uint8_t chain)
{
    memcpy(h.sr_out[chain], h.sr_shift[chain], 4);
    journal("sr_latch %u", chain);
}

const uint8_t *host_sr_outputs(uint8_t chain) { return h.sr_out[chain]; }
bool host_sr_oe_active(uint8_t chain) { return h.sr_oe[chain]; }

/* ---- SPI A, I²C ---- */

void host_spia_set_reply(const uint8_t *rx, size_t len)
{
    h.spia_reply_len = len > sizeof h.spia_reply ? sizeof h.spia_reply : len;
    memcpy(h.spia_reply, rx, h.spia_reply_len);
}

void hal_spia_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    char hex[65] = "";
    for (size_t i = 0; i < len && i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02X", tx ? tx[i] : 0xFF);
    }
    journal("spia %s", hex);
    for (size_t i = 0; i < len; i++) {
        if (rx) {
            rx[i] = i < h.spia_reply_len ? h.spia_reply[i] : 0xFF;
        }
    }
}

void host_i2c_set_handler(host_i2c_fn fn) { h.i2c = fn; }

int hal_i2c_write_read(uint8_t bus, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen)
{
    journal("i2c %u 0x%02X w%u r%u", bus, addr7, (unsigned)wlen, (unsigned)rlen);
    return h.i2c ? h.i2c(bus, addr7, w, wlen, r, rlen) : HAL_I2C_NACK;
}

int hal_i2c_write(uint8_t bus, uint8_t addr7, const uint8_t *data, size_t len)
{
    return hal_i2c_write_read(bus, addr7, data, len, NULL, 0);
}

int hal_i2c_read(uint8_t bus, uint8_t addr7, uint8_t *data, size_t len)
{
    return hal_i2c_write_read(bus, addr7, NULL, 0, data, len);
}

void hal_i2c_reset(uint8_t bus) { journal("i2c_reset %u", bus); }
uint32_t hal_i2c_status(uint8_t bus, bool *scl, bool *sda) { (void)bus; *scl = *sda = true; return 0; }
void hal_i2c_set_timing(uint8_t bus, uint32_t timingr) { journal("i2c_timing %u %08lx", bus, (unsigned long)timingr); }

/* ---- watchdog ---- */

void hal_wdt_start(uint32_t period_ms) { journal("wdt_start %lu", (unsigned long)period_ms); }
void hal_wdt_kick(void) { h.wdt_kicks++; }
unsigned host_wdt_kicks(void) { return h.wdt_kicks; }

/* ---- flash ---- */

static int flash_program(uint8_t *dst, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if ((dst[i] & data[i]) != data[i]) {
            return -1; /* нельзя поднять бит без стирания */
        }
        dst[i] &= data[i];
    }
    return 0;
}

uint8_t *host_cfg_memory(void) { return h.bank[h.active_bank - 1u] + HAL_BANK_SIZE - 128u * 1024u; }
const uint8_t *hal_cfg_base(void) { return host_cfg_memory(); }

int hal_cfg_erase(void)
{
    memset(host_cfg_memory(), 0xFF, HAL_CFG_SIZE);
    h.flash_ops++;
    journal("cfg_erase");
    return 0;
}

int hal_cfg_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (offset + len > HAL_CFG_SIZE || (offset & 3u) || (len & 3u)) {
        return -1;
    }
    h.flash_ops++;
    return flash_program(host_cfg_memory() + offset, data, len);
}

uint8_t *host_bank_memory(void) { return h.bank[2u - h.active_bank]; }
const uint8_t *hal_bank_inactive_base(void) { return host_bank_memory(); }

int hal_bank_erase_inactive(uint32_t sector)
{
    static const uint32_t sizes[12] = {16, 16, 16, 16, 64, 128, 128, 128, 128, 128, 128, 128};
    if (sector >= 12) {
        return -1;
    }
    uint32_t off = 0;
    for (uint32_t s = 0; s < sector; s++) {
        off += sizes[s] * 1024u;
    }
    memset(host_bank_memory() + off, 0xFF, sizes[sector] * 1024u);
    h.flash_ops++;
    journal("bank_erase %lu", (unsigned long)sector);
    return 0;
}

int hal_bank_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (offset + len > HAL_BANK_SIZE || (offset & 3u) || (len & 3u)) {
        return -1;
    }
    h.flash_ops++;
    return flash_program(host_bank_memory() + offset, data, len);
}

uint8_t hal_bank_active(void) { return h.active_bank; }
void host_set_active_bank(uint8_t bank) { h.active_bank = bank; }

int hal_bank_set_boot(uint8_t bank)
{
    h.boot_bank = bank;
    journal("set_boot %u", bank);
    return 0;
}

uint8_t host_boot_bank(void) { return h.boot_bank; }

void host_set_optbytes(bool ndbank, bool ndboot, bool iwdg_sw)
{
    h.opt.ndbank = ndbank;
    h.opt.ndboot = ndboot;
    h.opt.iwdg_sw = iwdg_sw;
}

struct hal_optbytes hal_optbytes_read(void) { return h.opt; }
unsigned host_flash_ops(void) { return h.flash_ops; }
