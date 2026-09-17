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
    snprintf(line, sizeof line, "SYST:UPD:BEGIN %lu,%08lx,%s", (unsigned long)image_len, (unsigned long)crc, "0.3.0");
    return con_cmd(line);
}

static void test_happy_path_and_confirm(void)
{
    device_boot();
    build_image(1, "0.3.0", 1000);
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
    CHECK(upd_get_state() == UPD_TRIAL);
    CHECK(cfg_get_trial(&t) && t.boot_count == 1);
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "TRIAL,0,0");
    CHECK_STR(con_cmd("SYST:UPD:CONFIRM"), "OK");
    CHECK(cfg_get_trial(&t) && t.confirmed == 1);
    CHECK_STR(con_cmd("SYST:UPD:CONFIRM"), "ERR:UPD_SEQ");
}

static void test_rollback_by_timeout_and_second_boot(void)
{
    device_boot();
    build_image(1, "0.3.0", 1024);
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
    build_image(1, "0.3.0", 700);
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
    build_image(2, "0.3.0", 700);
    CHECK_STR(begin(), "OK");
    CHECK_STR(send_image(256), "OK");
    CHECK_STR(con_cmd("SYST:UPD:COMMIT"), "ERR:UPD_HEADER,revision");
    CHECK(host_boot_bank() == 1);
    /* версия ниже минимальной */
    build_image(1, "0.1.0", 700);
    uint32_t crc = crc32_update(0, image, image_len);
    snprintf(line, sizeof line, "SYST:UPD:BEGIN %lu,%08lx,0.1.0", (unsigned long)image_len, (unsigned long)crc);
    CHECK_STR(con_cmd(line), "ERR:UPD_HEADER,version");
    /* ABORT возвращает в IDLE */
    build_image(1, "0.3.0", 700);
    CHECK_STR(begin(), "OK");
    CHECK_STR(con_cmd("SYST:UPD:ABORT"), "OK");
    CHECK_STR(con_cmd("SYST:UPD:STAT?"), "IDLE,0,0");
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

int main(void)
{
    test_utils();
    test_happy_path_and_confirm();
    test_rollback_by_timeout_and_second_boot();
    test_rejections();
    return test_result("test_update");
}
