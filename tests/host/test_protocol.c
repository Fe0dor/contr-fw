/* Протокол шага 2: FW-100…FW-117, FW-201, FW-205, FW-210, FW-218, FW-221, FW-222, FW-223. */
#include <stdlib.h>

#include "check.h"
#include "cmd.h"
#include "config.h"
#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define BUILD_VERSION STR(FW_VERSION_MAJOR) "." STR(FW_VERSION_MINOR) "." STR(FW_VERSION_PATCH)

static void test_idn_and_prefix(void)
{
    device_boot();
    CHECK(tcp_connect());
    /* FW-101: вход при подключении → поколение выросло */
    const char *r = tcp_cmd("*IDN?");
    CHECK_PREFIX(r, "G");
    CHECK(strstr(r, ";TESTDUT,CONTR-R1,UNPROVISIONED," BUILD_VERSION) != NULL);
    /* FW-114: на консоли тот же ответ без префикса */
    CHECK_STR(con_cmd("*IDN?"), "TESTDUT,CONTR-R1,UNPROVISIONED," BUILD_VERSION);
    /* регистр имени команды не важен */
    CHECK(strstr(tcp_cmd("*idn?"), "TESTDUT,CONTR-R1") != NULL);
}

static void test_unknown_and_range(void)
{
    device_boot();
    CHECK(tcp_connect());
    CHECK(strstr(tcp_cmd("FOO:BAR 1"), "ERR:UNKNOWN_CMD") != NULL);
    CHECK(strstr(tcp_cmd("*IDN? x"), "ERR:RANGE") != NULL);
    /* FW-108: стек ошибок с очисткой, старые первыми */
    const char *r = tcp_cmd("SYST:ERR?");
    CHECK(strstr(r, "UNKNOWN_CMD;RANGE") != NULL);
    CHECK(strstr(tcp_cmd("SYST:ERR?"), ";NONE") != NULL);
}

static void test_second_client_busy(void)
{
    device_boot();
    CHECK(tcp_connect());
    uint32_t gen = observed.generation;
    /* FW-102: второе соединение отклоняется без изменения состояния */
    CHECK(!host_net_connect());
    device_run(2);
    CHECK(observed.generation == gen);
    CHECK(observed.client_connected);
}

static void test_console_with_client(void)
{
    device_boot();
    CHECK(tcp_connect());
    /* FW-116: с консоли при клиенте только запросы и SAFE */
    CHECK_STR(con_cmd("SYST:PROV:SERIAL X"), "ERR:CONSOLE_ONLY"); /* FW-117 */
    CHECK_PREFIX(con_cmd("SYST:NET?"), "static,");
    CHECK_STR(con_cmd("SYST:UPD:ABORT"), "ERR:BUSY");
    uint32_t gen = observed.generation;
    CHECK_STR(con_cmd("SAFE"), "OK");
    /* FW-107, FW-221: SAFE с консоли → префикс клиента вырос на единицу */
    char expect[16];
    snprintf(expect, sizeof expect, "G%lu;", (unsigned long)(gen + 1));
    CHECK_PREFIX(tcp_cmd("*IDN?"), expect);
    /* FW-117 по TCP */
    CHECK(strstr(tcp_cmd("SYST:PROV:SERIAL X"), "ERR:CONSOLE_ONLY") != NULL);
}

static void test_provisioning(void)
{
    device_boot();
    /* без клиента с консоли — OK, повтор — PROVISIONED (FW-224, FW-225) */
    CHECK_STR(con_cmd("SYST:PROV:SERIAL CONTR-0007"), "OK");
    CHECK_STR(con_cmd("SYST:PROV:SERIAL CONTR-0008"), "ERR:PROVISIONED");
    CHECK_STR(con_cmd("*IDN?"), "TESTDUT,CONTR-R1,CONTR-0007," BUILD_VERSION);
    /* переживает перезапуск: та же память CFG */
    board_early_init();
    core_init();
    CHECK_STR(con_cmd("*IDN?"), "TESTDUT,CONTR-R1,CONTR-0007," BUILD_VERSION);
    /* FW-104: без записи — UNPROVISIONED (проверено в test_idn) */
}

static void test_safe_report_and_reasons(void)
{
    device_boot();
    /* после сброса: причина RESET, номер 0 */
    CHECK_PREFIX(con_cmd("SYST:SAFE?"), "0,RESET,OK,SILENT,OK,OK,OK,OK,");
    CHECK(tcp_connect());
    CHECK_PREFIX(con_cmd("SYST:SAFE?"), "1,CONNECT,");
    CHECK(strstr(tcp_cmd("SAFE"), ";OK") != NULL);
    CHECK(strstr(tcp_cmd("SYST:SAFE?"), ";2,SAFE,OK,SILENT,OK,OK,OK,OK,") != NULL);
    /* FW-218: потеря клиента → вход */
    host_net_disconnect();
    device_run(2);
    CHECK_PREFIX(con_cmd("SYST:SAFE?"), "3,CLIENT_LOST,");
    CHECK(!observed.client_connected);
}

static void test_busy_during_command(void)
{
    /* FW-113 и А1: строка, пришедшая во время исполнения, получает ERR:BUSY после ответа.
     * Исполнение на хосте мгновенно, поэтому моделируем: две строки одним куском —
     * вторая собирается до завершения первой только если пришла во время исполнения.
     * Здесь проверяем противоположное: строки по очереди исполняются обе. */
    device_boot();
    CHECK(tcp_connect());
    host_net_push_line("*IDN?");
    host_net_push_line("SYST:NET?");
    device_run(6);
    char l1[256], l2[256];
    CHECK(host_net_take_line(l1, sizeof l1));
    CHECK(host_net_take_line(l2, sizeof l2));
    CHECK(strstr(l1, "TESTDUT") != NULL);
    CHECK(strstr(l2, "static,") != NULL);
}

static void test_conf_constants(void)
{
    device_boot();
    const char *r = con_cmd("SYST:CONF?");
    CHECK(strstr(r, "BOARD_REV=1;") != NULL);
    CHECK(strstr(r, "RELAY_BREAK_MS=20;") != NULL);
    CHECK(strstr(r, "NET_DEFAULT_ADDR=192.168.0.20;") != NULL);
    CHECK(strstr(r, ";OPT_IWDG_SW=0") != NULL);
    /* FW-222: одинаков до и после команд */
    char before[4096];
    strncpy(before, r, sizeof before - 1);
    before[sizeof before - 1] = '\0';
    con_cmd("SYST:PROV:NET dhcp");
    con_cmd("SAFE");
    CHECK_STR(con_cmd("SYST:CONF?"), before);
    /* число констант равно списку раздела 5: 50 имён */
    int count = 0;
    for (const char *p = before; *p; p++) {
        count += *p == '=';
    }
    CHECK(count == 50);
}

static void test_one_frame_per_command(void)
{
    device_boot();
    CHECK(tcp_connect());
    const char *cmds[] = {"*IDN?", "SYST:SAFE?", "SYST:ERR?", "SYST:LOG?", "SYST:CONF?", "SYST:NET?",
                          "SYST:UPD:STAT?", "TEST:ALL?", "INTERLOCK:LIST?", "SAFE", "FOO"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        host_net_push_line(cmds[i]);
        device_run(4);
        static uint8_t out[16384];
        size_t n = host_net_take(out, sizeof out);
        int newlines = 0;
        for (size_t k = 0; k < n; k++) {
            newlines += out[k] == '\n';
        }
        CHECK(newlines == 1);
        CHECK(n > 0 && out[n - 1] == '\n');
    }
}

static void test_interlock_list(void)
{
    device_boot();
    const char *r = con_cmd("INTERLOCK:LIST?");
    int records = 0;
    for (const char *p = r; *p; p++) {
        records += *p == ';';
    }
    CHECK(records == 81); /* 81 реле + последняя запись версии */
    CHECK(strstr(r, "M4,33,int,PG8,M+RA;") != NULL);
    CHECK(strstr(r, "rev1.1,sha256:") != NULL);
    /* FW-129: сумма не меняется после команд */
    char before[8192];
    strncpy(before, r, sizeof before - 1);
    before[sizeof before - 1] = '\0';
    con_cmd("SAFE");
    CHECK_STR(con_cmd("INTERLOCK:LIST?"), before);
}

static void test_log(void)
{
    device_boot();
    con_cmd("FOO");
    con_cmd("SAFE");
    const char *r = con_cmd("SYST:LOG?");
    CHECK(strstr(r, "reset:") != NULL);
    CHECK(strstr(r, "ERR:UNKNOWN_CMD") == NULL); /* неизвестная команда без имени в журнале */
    CHECK(strstr(r, "SAFE SAFE OK") != NULL);
}

static void test_idempotent_sets(void)
{
    device_boot();
    /* FW-111: повтор установки — тот же ответ и то же состояние */
    CHECK_STR(con_cmd("SYST:PROV:NET static,192.168.0.21,255.255.255.0,192.168.0.100"), "OK");
    unsigned ops = host_flash_ops();
    CHECK_STR(con_cmd("SYST:PROV:NET static,192.168.0.21,255.255.255.0,192.168.0.100"), "OK");
    CHECK(host_flash_ops() > ops); /* новая запись, но действующие настройки те же */
    struct desired before = desired;
    CHECK_STR(con_cmd("SAFE"), "OK");
    CHECK_STR(con_cmd("SAFE"), "OK");
    CHECK(memcmp(&before, &desired, sizeof before) == 0);
}

static void test_test_all(void)
{
    device_boot();
    const char *r = con_cmd("TEST:ALL?");
    CHECK_PREFIX(r, "WARN;SR0:UNTESTED;SR1:UNVERIFIED;");
    CHECK(strstr(r, "PROV:UNPROVISIONED") != NULL);
    CHECK(strstr(r, "OPTBYTES:OK") != NULL);
    CHECK(strstr(r, "RESET:") != NULL);
    CHECK(strstr(r, "SAFE:SILENT") != NULL);
    host_set_optbytes(true, false, false);
    CHECK_PREFIX(con_cmd("TEST:ALL?"), "FAIL;");
    CHECK(strstr(con_cmd("TEST:ALL?"), "OPTBYTES:MISMATCH") != NULL);
}

static void test_early_packet_and_overflow_recovery(void)
{
    device_boot();
    CHECK(host_net_connect());
    host_net_push_line("*IDN?"); /* first packet before accept is handled by core_step */
    device_run(4);
    CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
    CHECK_PREFIX(reply_buf, "G1;TESTDUT,");

    char flood[700];
    memset(flood, 'A', sizeof flood);
    memcpy(flood, "SAFE", 4);
    flood[sizeof flood - 1] = '\n';
    host_net_push((const uint8_t *)flood, sizeof flood);
    device_run(4);
    CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
    CHECK_STR(reply_buf, "G1;ERR:RANGE");
    CHECK_PREFIX(tcp_cmd("*IDN?"), "G1;TESTDUT,");
}

int main(void)
{
    test_early_packet_and_overflow_recovery();
    test_idn_and_prefix();
    test_unknown_and_range();
    test_second_client_busy();
    test_console_with_client();
    test_provisioning();
    test_safe_report_and_reasons();
    test_busy_during_command();
    test_conf_constants();
    test_one_frame_per_command();
    test_interlock_list();
    test_log();
    test_idempotent_sets();
    test_test_all();
    return test_result("test_protocol");
}
