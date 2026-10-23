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
    cfg_set_erase_allowed(true);
    CHECK(cfg_compact()==0);
    CHECK(cfg_free_records()==CFG_RECORDS-2);
    n.addr=0x77;CHECK(cfg_set_net(&n)==0);
    cfg_init();
    char serial[CFG_SERIAL_MAX+1];
    CHECK(cfg_get_serial(serial) && strcmp(serial,"S1")==0);
    CHECK(cfg_get_net(&out) && out.addr==n.addr);

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

static void test_empty_gap_after_write_failure(void)
{
    device_boot();
    host_cfg_fail_writes(1);
    CHECK_STR(con_cmd("SYST:PROV:SERIAL FIRST"), "ERR:RANGE,flash");
    CHECK_STR(con_cmd("SYST:PROV:SERIAL SECOND"), "OK");
    size_t remaining=cfg_free_records();
    board_early_init();core_init();
    char serial[CFG_SERIAL_MAX+1];
    CHECK(cfg_get_serial(serial) && strcmp(serial,"SECOND")==0);
    CHECK(cfg_free_records()==remaining);
    CHECK_STR(con_cmd("SYST:PROV:SERIAL THIRD"), "ERR:PROVISIONED");
    struct cfg_net a={0,1,2,3}, b={1,4,5,6}, out;
    CHECK(cfg_set_net(&a)==0);
    host_cfg_fail_writes(2);
    CHECK(cfg_set_net(&b)!=0);CHECK(cfg_set_net(&b)!=0);
    CHECK(cfg_set_net(&b)==0);
    struct cfg_trial t={2,1,1}, got;
    CHECK(cfg_set_trial(&t)==0);
    remaining=cfg_free_records();cfg_init();
    CHECK(cfg_free_records()==remaining);
    CHECK(cfg_get_net(&out) && out.addr==b.addr && out.dhcp==b.dhcp);
    CHECK(cfg_get_trial(&got) && got.bank==2 && got.confirmed==1);
    CHECK(cfg_set_net(&a)==0);cfg_init();
    CHECK(cfg_get_serial(serial) && strcmp(serial,"SECOND")==0);
    CHECK(cfg_get_trial(&got) && got.confirmed==1);
    CHECK(cfg_get_net(&out) && out.addr==a.addr);
}

int main(void)
{
    test_empty_gap_after_write_failure();
    test_last_valid_wins();
    test_fill_and_compact();
    test_trial_record();
    test_snapshot();
    return test_result("test_cfg");
}
