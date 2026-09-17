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
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy;
        if (hal_i2c_read(bus, addr, &dummy, 1) == HAL_I2C_OK) {
            resp_printf(c, "%s0x%02X", any ? "," : "", addr);
            any = true;
        }
    }
    if (!any) {
        resp_puts(c, "NONE");
    }
    resp_end(c);
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
