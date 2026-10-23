/* Автомат обновления с моделью flash (А11, FW-232…FW-238). */
#include <stdio.h>
#include <stdlib.h>

#include "cfg.h"
#include "check.h"
#include "config.h"
#include "update.h"

static uint8_t image[4096];
static size_t image_len;

static void build_image(uint8_t revision, const char *version, size_t len)
{
    image_len = len;
    for (size_t i = 0; i < len; i++) {
        image[i] = (uint8_t)(i * 7u + 3u);
    }
    uint32_t vectors[2] = {0x20080000u, 0x08000225u};
    memcpy(image, vectors, sizeof vectors);
    struct fw_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "CONTRFW1", 8);
    memcpy(h.model, "CONTR", 5);
    h.revision = revision;
    unsigned a, b, c;
    sscanf(version, "%u.%u.%u", &a, &b, &c);
    h.ver_major = (uint8_t)a;
    h.ver_minor = (uint8_t)b;
    h.ver_patch = (uint8_t)c;
    h.image_size = (uint32_t)len;
    h.reserved[0] = FW_STORAGE_LAYOUT;
    memcpy(image + FW_HEADER_OFFSET, &h, sizeof h);
}

static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode(const uint8_t *in, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[o] = '\0';
}

static const char *send_image(size_t block)
{
    static char line[600];
    static char enc[400];
    for (size_t off = 0; off < image_len; off += block) {
        size_t n = image_len - off < block ? image_len - off : block;
        encode(image + off, n, enc);
        snprintf(line, sizeof line, "SYST:UPD:DATA %s", enc);
        const char *r = con_cmd(line);
        if (strcmp(r, "OK") != 0) {
            return r;
        }
    }
    return "OK";
}

static const char *begin(void)
{
    static char line[128];
    uint32_t crc = crc32_update(0, image, image_len);
    snprintf(line, sizeof line, "SYST:UPD:BEGIN %lu,%08lx,%s", (unsigned long)image_len, (unsigned long)crc, "0.6.2");
    return con_cmd(line);
}

static void test_happy_path_and_confirm(void)
{
    device_boot();
    CHECK_STR(con_cmd("SYST:PROV:SERIAL MODEL-ONLY"), "OK");
    CHECK_STR(con_cmd("SYST:PROV:NET dhcp"), "OK");
    struct cfg_psu_cal cal, restored;
    for(unsigned i=0;i<15;i++)cal.code[i]=(uint16_t)(38000+i*1000);
    CHECK(cfg_set_psu_cal(&cal)==0);
    const uint8_t *old_cfg = hal_cfg_base();
    build_image(1, "0.6.2", 1000);
    CHECK_STR(con_cmd("SYST:UPD:DATA AAAA"), "ERR:UPD_SEQ");      /* FW-234 */
    CHECK_STR(con_cmd("SYST:UPD:CONFIRM"), "ERR:UPD_SEQ");
    CHECK_STR(begin(), "OK");
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "RECEIVING,0,1000");
    CHECK_STR(send_image(256), "OK");
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "RECEIVING,1000,1000");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "OK");
    /* образ записан в неактивный банк, банк переключён, сброс запрошен */
    CHECK(memcmp(host_bank_memory(), image, image_len) == 0);
    CHECK(host_boot_bank() == 2);
    CHECK(host_reset_requested());
    struct cfg_trial t;
    CHECK(cfg_get_trial(&t) && t.bank == 2 && t.boot_count == 0 && !t.confirmed);
    /* копия CFG в хвосте неактивного банка (FW-238) */
    CHECK(host_bank_memory()[HAL_BANK_SIZE - 128u * 1024u + 2] == CFG_TRIAL
          || host_bank_memory()[HAL_BANK_SIZE - 128u * 1024u + 2] == CFG_SERIAL);

    /* первый старт нового образа: счётчик 1, состояние TRIAL, CONFIRM → OK */
    host_set_active_bank(2);
    board_early_init();
    core_init();
    CHECK(hal_cfg_base() != old_cfg);
    CHECK(upd_get_state() == UPD_TRIAL);
    CHECK(cfg_get_trial(&t) && t.boot_count == 1);
    char serial[CFG_SERIAL_MAX + 1];
    struct cfg_net net;
    CHECK(cfg_get_serial(serial) && strcmp(serial, "MODEL-ONLY") == 0);
    CHECK(cfg_get_net(&net) && net.dhcp == 1);
    CHECK(cfg_get_psu_cal(&restored) && memcmp(&cal,&restored,sizeof cal)==0);
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "TRIAL,0,0");
    CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:BUSY");
    CHECK_PREFIX(con_cmd("PSU:CAL:MODE ON"),"ERR:BUSY");
    CHECK_STR(con_cmd("SYST:UPD:CONFIRM"), "OK");
    CHECK(cfg_get_trial(&t) && t.confirmed == 1);
    CHECK_STR(con_cmd("SYST:UPD:CONFIRM"), "ERR:UPD_SEQ");
    /* Confirm modified only the new physical CFG; rollback copy is independent. */
    host_set_active_bank(1);
    cfg_init();
    CHECK(cfg_get_trial(&t) && t.boot_count == 0 && !t.confirmed);
    CHECK(cfg_get_serial(serial) && strcmp(serial, "MODEL-ONLY") == 0);
    CHECK(cfg_get_net(&net) && net.dhcp == 1);
    CHECK(cfg_get_psu_cal(&restored) && memcmp(&cal,&restored,sizeof cal)==0);
}

static void test_rollback_by_timeout_and_second_boot(void)
{
    device_boot();
    build_image(1, "0.6.2", 1024);
    CHECK_STR(begin(), "OK");
    CHECK_STR(send_image(200), "OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "OK");
    /* старт без подтверждения: таймер → откат (FW-237) */
    host_set_active_bank(2);
    board_early_init();
    core_init();
    CHECK(upd_get_state() == UPD_TRIAL);
    host_advance_ms(UPD_CONFIRM_TIMEOUT_MS - 1);
    device_run(1);
    CHECK(host_boot_bank() == 2);
    host_advance_ms(2);
    device_run(1);
    CHECK(host_boot_bank() == 1);
    /* зависший образ: второй старт без подтверждения → откат при старте */
    struct cfg_trial t = {2, 1, 0};
    cfg_set_trial(&t);
    host_set_active_bank(2);
    board_early_init();
    core_init();
    CHECK(host_boot_bank() == 1);
    /* после отката старый банк сообщает об откате в SYST:ERR? */
    host_set_active_bank(1);
    board_early_init();
    core_init();
    CHECK(strstr(con_cmd("SYST:ERR?"), "update rolled back") != NULL);
}

static void test_rejections(void)
{
    device_boot();
    build_image(1, "0.6.2", 700);
    /* FW-233: не в безопасном состоянии */
    observed.in_safe_state = false;
    CHECK_STR(begin(), "ERR:NOT_SAFE,STATE");
    CHECK_STR(con_cmd("SAFE"), "OK");
    /* FW-233: опционные байты */
    host_set_optbytes(true, false, false);
    CHECK_STR(begin(), "ERR:NOT_SAFE,OPTBYTES");
    host_set_optbytes(false, false, false);
    /* FW-232: блок длиннее допустимого */
    CHECK_STR(begin(), "OK");
    char big[400];
    memset(big, 'A', 348);
    big[348] = '\0';
    char line[420];
    snprintf(line, sizeof line, "SYST:UPD:DATA %s", big);
    CHECK_STR(con_cmd(line), "ERR:RANGE,block");
    /* FW-235: испорченный байт → UPD_CRC */
    image[100] ^= 1;
    CHECK_STR(send_image(256), "OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "ERR:UPD_CRC");
    CHECK(host_boot_bank() == 1);
    image[100] ^= 1;
    /* FW-235: другая ревизия → UPD_HEADER,revision */
    build_image(2, "0.6.2", 700);
    CHECK_STR(begin(), "OK");
    CHECK_STR(send_image(256), "OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "ERR:UPD_HEADER,revision");
    CHECK(host_boot_bank() == 1);
    /* версия ниже минимальной */
    build_image(1, "0.1.0", 700);
    uint32_t crc = crc32_update(0, image, image_len);
    snprintf(line, sizeof line, "SYST:UPD:BEGIN %lu,%08lx,0.1.0", (unsigned long)image_len, (unsigned long)crc);
    CHECK_STR(con_cmd(line), "ERR:UPD_HEADER,version");
    CHECK_STR(con_cmd("SYST:UPD:BEGIN 700,0,0.6.0"), "ERR:UPD_HEADER,version");
    /* ABORT возвращает в IDLE */
    build_image(1, "0.6.2", 700);
    CHECK_STR(begin(), "OK");
    CHECK_STR(con_cmd("SYST:UPD:ABORT"), "OK");
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "IDLE,0,0");
}

static void test_bad_vectors(void)
{
    static const uint32_t invalid[][2] = {
        {0x20000000u, 0x08000225u}, {0x20080008u, 0x08000225u},
        {0x20080001u, 0x08000225u}, {0x20080000u, 0x08000224u},
        {0x20080000u, 0x08000001u}, {0x20080000u, 0x08000401u},
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        device_boot();
        build_image(1, "0.6.2", 1024);
        memcpy(image, invalid[i], sizeof invalid[i]);
        CHECK_STR(begin(), "OK"); /* CRC includes the invalid vectors */
        CHECK_STR(send_image(256), "OK");
        CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "ERR:UPD_HEADER,vectors");
        CHECK(host_boot_bank() == 1 && !host_reset_requested());
    }
}

static bool erase_interrupt_sent;
static bool erase_disconnect;
static void interrupt_erase(void)
{
    if (erase_interrupt_sent || host_journal_find("bank_erase ", 0) < 0) return;
    erase_interrupt_sent = true;
    if (erase_disconnect) host_net_disconnect();
    else host_console_push_line("SAFE");
}

static void test_begin_interruption(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        device_boot();
        CHECK(tcp_connect());
        erase_disconnect = mode != 0;
        erase_interrupt_sent = false;
        host_journal_clear();
        host_net_set_poll_hook(interrupt_erase);
        host_net_push_line("SYST:UPD:BEGIN 1000,12345678,0.6.2");
        device_run(6);
        host_net_set_poll_hook(NULL);
        CHECK(erase_interrupt_sent);
        CHECK(host_journal_find("bank_erase 1", 0) < 0);
        CHECK(upd_get_state() == UPD_IDLE);
        CHECK(host_boot_bank() == 1 && !host_reset_requested());
        if (!erase_disconnect) {
            CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
            CHECK_STR(reply_buf, "G1;ERR:ABORTED");
            CHECK(host_console_take_line(reply_buf, sizeof reply_buf));
            CHECK_STR(reply_buf, "OK");
        }
        CHECK_STR(con_cmd("SYST:UPD:STAT?"), "IDLE,0,0");
    }
}

static void test_utils(void)
{
    /* crc32 как zlib: "123456789" → 0xCBF43926 */
    CHECK(crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    uint8_t out[8];
    CHECK(base64_decode("aGVsbG8=", out, sizeof out) == 5 && memcmp(out, "hello", 5) == 0);
    CHECK(base64_decode("a*", out, sizeof out) < 0);
    CHECK(base64_decode("A", out, sizeof out) < 0);
    CHECK(base64_decode("AAAA=", out, sizeof out) < 0);
    CHECK(base64_decode("AB==", out, sizeof out) < 0);
    CHECK(base64_decode("AA==", out, sizeof out) == 1);
    uint32_t v;
    CHECK(version_parse("1.2.3", &v) && v == 0x010203u);
    CHECK(!version_parse("1.2", &v));
}

static void test_trial_counter_write_failure(void)
{
    for (unsigned active=1;active<=2;++active) {
        device_boot();
        host_set_active_bank((uint8_t)active);CHECK(hal_bank_set_boot((uint8_t)active)==0);cfg_init();
        struct cfg_trial t={(uint8_t)active,0,0}, got;
        CHECK(cfg_set_trial(&t)==0);
        host_cfg_fail_writes(1);
        board_early_init();core_init();
        CHECK(host_reset_requested());
        CHECK(host_boot_bank()==(active==1?2:1));
        CHECK(upd_get_state()!=UPD_TRIAL);
        CHECK(cfg_get_trial(&got) && got.boot_count==0);
        CHECK(strstr(con_cmd("SYST:ERR?"),"trial counter write failed"));
    }
}

static void test_layout_and_full_cfg(void)
{
    device_boot();build_image(1,"0.6.2",1024);
    uint32_t old_layout=UINT32_MAX;
    memcpy(image+FW_HEADER_OFFSET+24,&old_layout,4);
    CHECK_STR(begin(),"OK");CHECK_STR(send_image(256),"OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"),"ERR:UPD_HEADER,layout");
    CHECK(!host_reset_requested());
    CHECK_STR(con_cmd("SYST:UPD:BEGIN 786433,0,0.6.2"),"ERR:RANGE,size");
    CHECK(cfg_set_serial("FULL-CFG")==0);
    struct cfg_net n={0,0xc0a80014,0xffffff00,0};
    while(cfg_free_records())CHECK(cfg_set_net(&n)==0);
    build_image(1,"0.6.2",1024);
    CHECK_STR(begin(),"OK");CHECK_STR(send_image(256),"OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"),"OK");
    CHECK(host_boot_bank()==2);
    host_set_active_bank(2);board_early_init();core_init();
    char serial[CFG_SERIAL_MAX+1];CHECK(cfg_get_serial(serial)&&strcmp(serial,"FULL-CFG")==0);
    CHECK(upd_get_state()==UPD_TRIAL);CHECK_STR(con_cmd("SYST:UPD:CONFIRM"),"OK");
}

int main(void)
{
    test_layout_and_full_cfg();
    test_trial_counter_write_failure();
    test_begin_interruption();
    test_bad_vectors();
    test_utils();
    test_happy_path_and_confirm();
    test_rollback_by_timeout_and_second_boot();
    test_rejections();
    return test_result("test_update");
}
