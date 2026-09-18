/* Сектор CFG (А10): последняя корректная запись, испорченная запись, уплотнение. */
#include "cfg.h"
#include "check.h"

static void test_last_valid_wins(void)
{
    host_reset_all();
    cfg_init();
    cfg_set_erase_allowed(true);
    struct cfg_net a = {0, 0xC0A80015u, 0xFFFFFF00u, 0xC0A80064u};
    struct cfg_net b = {1, 0, 0, 0};
    CHECK(cfg_set_net(&a) == 0);
    CHECK(cfg_set_net(&b) == 0);
    struct cfg_net out;
    CHECK(cfg_get_net(&out) && out.dhcp == 1);
    /* испортить последнюю запись: действует предыдущая */
    uint8_t *mem = host_cfg_memory();
    mem[CFG_RECORD_SIZE + 10] ^= 0xFF;
    cfg_init();
    CHECK(cfg_get_net(&out) && out.dhcp == 0 && out.addr == 0xC0A80015u);
    /* серийный номер отдельно от сети */
    CHECK(!cfg_get_serial((char[CFG_SERIAL_MAX + 1]){0}));
    CHECK(cfg_set_serial("CONTR-0001") == 0);
    char serial[CFG_SERIAL_MAX + 1];
    CHECK(cfg_get_serial(serial) && strcmp(serial, "CONTR-0001") == 0);
}

static void test_fill_and_compact(void)
{
    host_reset_all();
    cfg_init();
    cfg_set_erase_allowed(false);
    CHECK(cfg_set_serial("S1") == 0);
    struct cfg_net n = {0, 1, 2, 3};
    /* заполнить сектор */
    while (cfg_free_records() > 0) {
        n.addr++;
        CHECK(cfg_set_net(&n) == 0);
    }
    /* без разрешения на стирание — отказ, записи целы */
    CHECK(cfg_set_net(&n) == -2);
    struct cfg_net out;
    CHECK(cfg_get_net(&out) && out.addr == n.addr);
    /* Even in SAFE, never erase the only durable copy. */
    cfg_set_erase_allowed(true);
    unsigned ops = host_flash_ops();
    uint32_t saved_addr = n.addr;
    n.addr = 0x77;
    CHECK(cfg_set_net(&n) == -2);
    CHECK(cfg_compact() == -2);
    CHECK(host_flash_ops() == ops);
    cfg_init(); /* reboot: all durable values still present */
    CHECK(cfg_free_records() == 0);
    char serial[CFG_SERIAL_MAX + 1];
    CHECK(cfg_get_serial(serial) && strcmp(serial, "S1") == 0);
    CHECK(cfg_get_net(&out) && out.addr == saved_addr);
}

static void test_trial_record(void)
{
    host_reset_all();
    cfg_init();
    cfg_set_erase_allowed(true);
    struct cfg_trial t = {2, 0, 0}, out;
    CHECK(!cfg_get_trial(&out));
    CHECK(cfg_set_trial(&t) == 0);
    CHECK(cfg_get_trial(&out) && out.bank == 2 && out.boot_count == 0 && !out.confirmed);
}

static void test_snapshot(void)
{
    host_reset_all();
    cfg_init();
    cfg_set_erase_allowed(true);
    CHECK(cfg_set_serial("S2") == 0);
    uint8_t buf[3 * CFG_RECORD_SIZE];
    CHECK(cfg_snapshot(buf, sizeof buf) == CFG_RECORD_SIZE);
    CHECK(buf[2] == CFG_SERIAL);
}

int main(void)
{
    test_last_valid_wins();
    test_fill_and_compact();
    test_trial_record();
    test_snapshot();
    return test_result("test_cfg");
}
