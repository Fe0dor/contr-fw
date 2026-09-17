/* Сеть: MAC из UID (FW-226), разбор SYST:PROV:NET (FW-227), возврат к стандартному
 * адресу (FW-228), кнопка USER (FW-229), SYST:NET? (FW-230). */
#include "cfg.h"
#include "check.h"
#include "config.h"
#include "netcfg.h"

static void test_mac_from_uid(void)
{
    uint8_t uid[12] = {0x30, 0x00, 0x33, 0x00, 0x11, 0x51, 0x38, 0x36, 0x33, 0x34, 0x31, 0x39};
    uint8_t mac[6], mac2[6];
    netcfg_mac_from_uid(uid, mac);
    CHECK((mac[0] & 0x02) != 0); /* локально администрируемый */
    CHECK((mac[0] & 0x01) == 0); /* unicast */
    netcfg_mac_from_uid(uid, mac2);
    CHECK(memcmp(mac, mac2, 6) == 0);
    uid[11] ^= 1;
    netcfg_mac_from_uid(uid, mac2);
    CHECK(memcmp(mac, mac2, 6) != 0);
    /* известный UID даёт известный адрес */
    uint8_t known[12] = {0};
    netcfg_mac_from_uid(known, mac);
    char s[18];
    snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    CHECK_STR(s, "02:7B:D5:C6:6F:CB");
}

static void test_parse(void)
{
    uint32_t ip;
    CHECK(netcfg_parse_ip("192.168.0.20", &ip) && ip == 0xC0A80014u);
    CHECK(!netcfg_parse_ip("192.168.0", &ip));
    CHECK(!netcfg_parse_ip("192.168.0.256", &ip));
    CHECK(!netcfg_parse_ip("a.b.c.d", &ip));
    char out[16];
    netcfg_format_ip(0xC0A80014u, out);
    CHECK_STR(out, "192.168.0.20");

    struct cfg_net n;
    char a1[] = "static", a2[] = "10.0.0.5", a3[] = "255.255.0.0", a4[] = "10.0.0.1";
    char *argv[] = {a1, a2, a3, a4};
    CHECK(netcfg_parse_prov(4, argv, &n) && n.dhcp == 0 && n.addr == 0x0A000005u && n.mask == 0xFFFF0000u);
    char d[] = "DHCP";
    char *argv2[] = {d};
    CHECK(netcfg_parse_prov(1, argv2, &n) && n.dhcp == 1);
    CHECK(!netcfg_parse_prov(2, argv, &n));
    char bad[] = "auto";
    char *argv3[] = {bad};
    CHECK(!netcfg_parse_prov(1, argv3, &n));
}

static void test_prov_net_and_status(void)
{
    device_boot();
    CHECK(host_net_initialized());
    const char *r = con_cmd("SYST:NET?");
    CHECK_PREFIX(r, "static,02:");
    CHECK(strstr(r, ",192.168.0.20,UP") != NULL);
    CHECK_STR(con_cmd("SYST:PROV:NET static,192.168.0.21,255.255.255.0,192.168.0.100"), "OK");
    /* до сброса — прежний адрес; после — новый */
    CHECK(strstr(con_cmd("SYST:NET?"), ",192.168.0.20,") != NULL);
    board_early_init();
    core_init();
    CHECK(strstr(con_cmd("SYST:NET?"), ",192.168.0.21,") != NULL);
    CHECK_STR(con_cmd("SYST:PROV:NET dhcp"), "OK");
    CHECK_STR(con_cmd("SYST:PROV:NET static,1.2.3"), "ERR:RANGE,net");
}

static void test_dhcp_fallback(void)
{
    device_boot();
    con_cmd("SYST:PROV:NET dhcp");
    board_early_init();
    core_init();
    CHECK_PREFIX(con_cmd("SYST:NET?"), "dhcp,");
    host_advance_ms(DHCP_TIMEOUT_MS - 1);
    device_run(1);
    CHECK(host_net_static_addr() == 0);
    host_advance_ms(2);
    device_run(1);
    CHECK(host_net_static_addr() == 0xC0A80014u); /* FW-228 */
    CHECK_PREFIX(con_cmd("SYST:NET?"), "dhcp-fallback,");
}

static void test_button_reset(void)
{
    device_boot();
    con_cmd("SYST:PROV:NET static,10.0.0.5,255.0.0.0,10.0.0.1");
    /* короткое нажатие ничего не меняет */
    host_set_button(true);
    device_run(1);
    host_advance_ms(1000);
    device_run(1);
    host_set_button(false);
    device_run(1);
    CHECK(!host_reset_requested());
    /* удержание 5 с → стандартные настройки и сброс */
    host_set_button(true);
    device_run(1);
    host_advance_ms(BUTTON_RESET_HOLD_MS);
    device_run(1);
    CHECK(host_reset_requested());
    struct cfg_net n;
    CHECK(cfg_get_net(&n) && n.addr == 0xC0A80014u && n.dhcp == 0);
}

int main(void)
{
    test_mac_from_uid();
    test_parse();
    test_prov_net_and_status();
    test_dhcp_fallback();
    test_button_reset();
    return test_result("test_netcfg");
}
