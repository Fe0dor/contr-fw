#include "netcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "errlog.h"
#include "hal.h"
#include "update.h" /* crc32_update */

static struct cfg_net active;
static uint8_t mac[6];
static bool dhcp_fallback_done;
static uint32_t start_ms;
static uint32_t button_down_since;
static bool button_was_down;

void netcfg_mac_from_uid(const uint8_t uid[12], uint8_t out[6])
{
    /* два независимых CRC32 дают 40 бит; первый байт — локально администрируемый unicast */
    uint32_t a = crc32_update(0, uid, 12);
    uint32_t b = crc32_update(0x5A5A5A5Au, uid, 12);
    out[0] = 0x02;
    out[1] = (uint8_t)(a >> 24);
    out[2] = (uint8_t)(a >> 16);
    out[3] = (uint8_t)(a >> 8);
    out[4] = (uint8_t)a;
    out[5] = (uint8_t)b;
}

bool netcfg_parse_ip(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int parts = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        unsigned long n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10u + (unsigned long)(*s - '0');
            s++;
            if (++digits > 3 || n > 255u) {
                return false;
            }
        }
        v = (v << 8) | (uint32_t)n;
        parts++;
        if (*s == '.') {
            s++;
            if (*s == '\0') {
                return false;
            }
        } else if (*s != '\0') {
            return false;
        }
    }
    if (parts != 4) {
        return false;
    }
    *out = v;
    return true;
}

void netcfg_format_ip(uint32_t addr, char *out)
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(addr >> 24) & 255u, (unsigned)(addr >> 16) & 255u,
             (unsigned)(addr >> 8) & 255u, (unsigned)addr & 255u);
}

void netcfg_defaults(struct cfg_net *out)
{
    out->dhcp = 0;
    netcfg_parse_ip(NET_DEFAULT_ADDR, &out->addr);
    netcfg_parse_ip(NET_DEFAULT_MASK, &out->mask);
    netcfg_parse_ip(NET_DEFAULT_GW, &out->gw);
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

bool netcfg_parse_prov(int argc, char *const *argv, struct cfg_net *out)
{
    if (argc < 1) {
        return false;
    }
    if (ieq(argv[0], "dhcp")) {
        if (argc != 1) {
            return false;
        }
        netcfg_defaults(out);
        out->dhcp = 1;
        return true;
    }
    if (!ieq(argv[0], "static") || argc != 4) {
        return false;
    }
    out->dhcp = 0;
    return netcfg_parse_ip(argv[1], &out->addr) && netcfg_parse_ip(argv[2], &out->mask)
           && netcfg_parse_ip(argv[3], &out->gw);
}

void netcfg_start(void)
{
    uint8_t uid[12];
    hal_uid(uid);
    netcfg_mac_from_uid(uid, mac);
    if (!cfg_get_net(&active)) {
        netcfg_defaults(&active);
    }
    struct hal_net_config hc = {
        .dhcp = active.dhcp != 0,
        .addr = active.addr,
        .mask = active.mask,
        .gw = active.gw,
        .keepalive_idle_s = TCP_KEEPALIVE_IDLE_S,
        .keepalive_intvl_s = TCP_KEEPALIVE_INTVL_S,
        .keepalive_cnt = TCP_KEEPALIVE_CNT,
        .port = NET_PORT,
    };
    memcpy(hc.mac, mac, 6);
    hal_net_init(&hc);
    start_ms = hal_millis();
    dhcp_fallback_done = false;
}

void netcfg_poll(void)
{
    uint32_t now = hal_millis();
    /* FW-228: DHCP без ответа → стандартный адрес */
    if (active.dhcp && !dhcp_fallback_done && !hal_net_dhcp_bound() && (uint32_t)(now - start_ms) >= DHCP_TIMEOUT_MS) {
        struct cfg_net d;
        netcfg_defaults(&d);
        hal_net_set_static(d.addr, d.mask, d.gw);
        dhcp_fallback_done = true;
        log_event("DHCP timeout, static %s", NET_DEFAULT_ADDR);
    }
    /* FW-229: удержание USER 5 с → стандартные настройки и перезапуск */
    bool down = hal_button_pressed();
    if (down && !button_was_down) {
        button_down_since = now;
    }
    if (down && (uint32_t)(now - button_down_since) >= BUTTON_RESET_HOLD_MS) {
        struct cfg_net d;
        netcfg_defaults(&d);
        log_event("USER 5s: net defaults, reset");
        cfg_set_net(&d);
        hal_reset();
        button_down_since = now; /* хост: hal_reset возвращается */
    }
    button_was_down = down;
}

const char *netcfg_mode_name(void)
{
    if (!active.dhcp) {
        return "static";
    }
    return dhcp_fallback_done ? "dhcp-fallback" : "dhcp";
}

void netcfg_mac_string(char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
