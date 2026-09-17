/* Наладочные команды сборки bringup (план 22, шаг 1). В рабочую прошивку не входят.
 * DBG:PIN пишет выводы и биты цепочек напрямую, минуя блокировки: только для наладки. */
#ifdef CONTR_BRINGUP

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "config.h"
#include "hal.h"
#include "signals.h"
#include "sr.h"

static bool parse_bool(const char *s, bool *out)
{
    if (strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    if (strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    return false;
}

void dbg_pin(struct cmdctx *c)
{
    bool level;
    if (!parse_bool(c->argv[1], &level)) {
        resp_err(c, ERR_RANGE, "level");
        return;
    }
    int s = signal_find(c->argv[0]);
    if (s >= 0) {
        if (signal_table[s].dir == SIGNAL_IN) {
            resp_err(c, ERR_RANGE, "input");
            return;
        }
        signal_set((enum signal_id)s, level);
        resp_ok(c);
        return;
    }
    int r = relay_find(c->argv[0]);
    if (r >= 0) {
        relay_drive(&relay_table[r], level);
        if (relay_table[r].addr.kind == RELAY_ADDR_SR) {
            sr_write(relay_table[r].addr.a);
        }
        resp_ok(c);
        return;
    }
    resp_err(c, ERR_RANGE, "name");
}

void dbg_pin_q(struct cmdctx *c)
{
    int s = signal_find(c->argv[0]);
    if (s >= 0) {
        resp_printf(c, "%d", signal_read((enum signal_id)s) ? 1 : 0);
        resp_end(c);
        return;
    }
    int r = relay_find(c->argv[0]);
    if (r >= 0) {
        const struct relay_desc *d = &relay_table[r];
        int v = d->addr.kind == RELAY_ADDR_PIN ? hal_gpio_read(d->addr.a, d->addr.b)
                                               : sr_image_get(d->addr.a, d->addr.b, d->addr.c);
        resp_printf(c, "%d", v ? 1 : 0);
        resp_end(c);
        return;
    }
    resp_err(c, ERR_RANGE, "name");
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, size_t max)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > max) {
        return -1;
    }
    for (size_t i = 0; i < n; i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i / 2] = (uint8_t)(hi * 16 + lo);
    }
    return (int)(n / 2);
}

static bool parse_chain(const char *s, uint8_t *chain)
{
    if (strcasecmp(s, "SR0") == 0 || strcmp(s, "0") == 0) {
        *chain = 0;
        return true;
    }
    if (strcasecmp(s, "SR1") == 0 || strcmp(s, "1") == 0) {
        *chain = 1;
        return true;
    }
    return false;
}

/* DBG:SR <цепочка>,<hex образа: регистр 0 первым> */
void dbg_sr(struct cmdctx *c)
{
    uint8_t chain, img[4];
    if (!parse_chain(c->argv[0], &chain)) {
        resp_err(c, ERR_RANGE, "chain");
        return;
    }
    int n = parse_hex(c->argv[1], img, 4);
    if (n != SR_REGS(chain)) {
        resp_err(c, ERR_RANGE, "image");
        return;
    }
    for (int i = 0; i < n; i++) {
        for (int b = 0; b < 8; b++) {
            sr_image_set(chain, (uint8_t)i, (uint8_t)b, (img[i] >> b) & 1u);
        }
    }
    sr_write(chain);
    resp_ok(c);
}

void dbg_sr_q(struct cmdctx *c)
{
    uint8_t chain;
    if (!parse_chain(c->argv[0], &chain)) {
        resp_err(c, ERR_RANGE, "chain");
        return;
    }
    for (int i = 0; i < SR_REGS(chain); i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) {
            v |= (uint8_t)(sr_image_get(chain, (uint8_t)i, (uint8_t)b) << b);
        }
        resp_printf(c, "%02X", v);
    }
    resp_end(c);
}

void dbg_i2c_scan(struct cmdctx *c)
{
    uint8_t bus;
    if (strcasecmp(c->argv[0], "A") == 0) {
        bus = 0;
    } else if (strcasecmp(c->argv[0], "B") == 0) {
        bus = 1;
    } else {
        resp_err(c, ERR_RANGE, "bus");
        return;
    }
    bool any = false;
    uint32_t t0 = hal_millis();
    int errors = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        /* пустая запись: только адрес, как SMBus quick */
        int rc = hal_i2c_write(bus, addr, NULL, 0);
        if (rc == HAL_I2C_OK) {
            resp_printf(c, "%s0x%02X", any ? "," : "", addr);
            any = true;
        } else if (rc != HAL_I2C_NACK) {
            errors++;
        }
    }
    if (!any) {
        resp_puts(c, "NONE");
    }
    resp_printf(c, ",%lu ms,%d errors", (unsigned long)(hal_millis() - t0), errors);
    resp_end(c);
}

static bool parse_bus(const char *s, uint8_t *bus)
{
    if (strcasecmp(s, "A") == 0) {
        *bus = 0;
        return true;
    }
    if (strcasecmp(s, "B") == 0) {
        *bus = 1;
        return true;
    }
    return false;
}

static const char *i2c_rc_name(int rc)
{
    switch (rc) {
    case HAL_I2C_OK: return "OK";
    case HAL_I2C_NACK: return "NACK";
    case HAL_I2C_BUS_ERROR: return "BUS_ERROR";
    case HAL_I2C_TIMEOUT: return "TIMEOUT";
    default: return "?";
    }
}

/* DBG:I2C <A|B>,<адрес hex>,<байты записи hex или ->,<число байт чтения> */
void dbg_i2c(struct cmdctx *c)
{
    uint8_t bus, w[32], r[32];
    if (!parse_bus(c->argv[0], &bus)) {
        resp_err(c, ERR_RANGE, "bus");
        return;
    }
    char *end;
    unsigned long addr = strtoul(c->argv[1], &end, 16);
    if (*end || addr > 0x7F) {
        resp_err(c, ERR_RANGE, "addr");
        return;
    }
    int wlen = strcmp(c->argv[2], "-") == 0 ? 0 : parse_hex(c->argv[2], w, sizeof w);
    unsigned long rlen = strtoul(c->argv[3], &end, 10);
    if (wlen < 0 || *end || rlen > sizeof r) {
        resp_err(c, ERR_RANGE, "data");
        return;
    }
    uint32_t t0 = hal_millis();
    int rc = hal_i2c_write_read(bus, (uint8_t)addr, w, (size_t)wlen, r, (size_t)rlen);
    resp_printf(c, "%s,%lu ms", i2c_rc_name(rc), (unsigned long)(hal_millis() - t0));
    for (unsigned long i = 0; rc == HAL_I2C_OK && i < rlen; i++) {
        resp_printf(c, "%s%02X", i ? "" : ",", r[i]);
    }
    resp_end(c);
}

void dbg_i2c_stat(struct cmdctx *c)
{
    uint8_t bus;
    if (!parse_bus(c->argv[0], &bus)) {
        resp_err(c, ERR_RANGE, "bus");
        return;
    }
    bool scl, sda;
    uint32_t isr = hal_i2c_status(bus, &scl, &sda);
    resp_printf(c, "ISR=%08lX,SCL=%d,SDA=%d", (unsigned long)isr, scl ? 1 : 0, sda ? 1 : 0);
    resp_end(c);
}

void dbg_i2c_timing(struct cmdctx *c)
{
    uint8_t bus;
    char *end;
    if (!parse_bus(c->argv[0], &bus)) {
        resp_err(c, ERR_RANGE, "bus");
        return;
    }
    unsigned long t = strtoul(c->argv[1], &end, 16);
    if (*end) {
        resp_err(c, ERR_RANGE, "timing");
        return;
    }
    hal_i2c_set_timing(bus, (uint32_t)t);
    resp_ok(c);
}

void dbg_i2c_reset(struct cmdctx *c)
{
    uint8_t bus;
    if (!parse_bus(c->argv[0], &bus)) {
        resp_err(c, ERR_RANGE, "bus");
        return;
    }
    hal_i2c_reset(bus);
    resp_ok(c);
}

/* DBG:SPI <hex>: обмен по SPI A под активным CS_PWR, ответ — принятые байты */
void dbg_spi(struct cmdctx *c)
{
    uint8_t tx[32], rx[32];
    int n = parse_hex(c->argv[0], tx, sizeof tx);
    if (n <= 0) {
        resp_err(c, ERR_RANGE, "hex");
        return;
    }
    signal_set(SIG_CS_PWR, true);
    hal_delay_us(1);
    hal_spia_xfer(tx, rx, (size_t)n);
    hal_delay_us(1);
    signal_set(SIG_CS_PWR, false);
    for (int i = 0; i < n; i++) {
        resp_printf(c, "%02X", rx[i]);
    }
    resp_end(c);
}

#endif /* CONTR_BRINGUP */
