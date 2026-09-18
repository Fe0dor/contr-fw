/* Обновление во второй банк (А11). Образ принимается блоками base64 в неактивный банк,
 * COMMIT проверяет размер, CRC32 и заголовок, копирует CFG в хвост неактивного банка,
 * пишет запись «пробный запуск» и переключает BOOT_ADD0. Стартовый код (upd_boot_check)
 * откатывает образ без подтверждения. */
#include "update.h"

#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "config.h"
#include "core.h"
#include "errlog.h"
#include "hal.h"
#include "state.h"

#define IMAGE_MAX (HAL_BANK_SIZE - 128u * 1024u) /* хвост банка — CFG */
#define CFG_TAIL_OFFSET (HAL_BANK_SIZE - 128u * 1024u)

/* заголовок текущего образа — заполняет компоновщик и mkimage */
#ifdef CONTR_TARGET
__attribute__((section(".fw_header"), used))
#endif
const struct fw_header fw_header = {
    .magic = {'C', 'O', 'N', 'T', 'R', 'F', 'W', '1'},
    .model = FW_MODEL,
    .revision = BOARD_REV,
    .ver_major = FW_VERSION_MAJOR,
    .ver_minor = FW_VERSION_MINOR,
    .ver_patch = FW_VERSION_PATCH,
    .image_size = 0xFFFFFFFFu,
    .reserved = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
};

static struct {
    enum upd_state state;
    uint32_t expected_size, expected_crc, received;
    uint32_t crc;
    uint8_t carry[4];
    size_t carry_len;
    uint32_t trial_started_ms;
    bool trial_pending;
} upd;

/* ---- утилиты ---- */

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, uint8_t *out, size_t max)
{
    size_t length = strlen(in);
    if (!length || length % 4u != 0) return -1;
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    int pad = 0;
    for (; *in; in++) {
        if (*in == '=') {
            pad++;
            continue;
        }
        if (pad) {
            return -1;
        }
        int v = b64val(*in);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2 || bits != pad * 2 || (bits && (acc & ((1u << bits) - 1u)))) {
        return -1;
    }
    return (int)n;
}

bool version_parse(const char *s, uint32_t *out)
{
    unsigned a, b, c;
    char tail;
    if (sscanf(s, "%u.%u.%u%c", &a, &b, &c, &tail) != 3 || a > 255 || b > 255 || c > 255) {
        return false;
    }
    *out = (a << 16) | (b << 8) | c;
    return true;
}

const char *upd_state_name(enum upd_state s)
{
    switch (s) {
    case UPD_RECEIVING: return "RECEIVING";
    case UPD_TRIAL: return "TRIAL";
    case UPD_CONFIRMED: return "CONFIRMED";
    default: return "IDLE";
    }
}

enum upd_state upd_get_state(void)
{
    return upd.state;
}

/* ---- старт (роль загрузчика) ---- */

void upd_boot_check(void)
{
    struct cfg_trial t;
    uint8_t active = hal_bank_active();
    memset(&upd, 0, sizeof upd);
    if (!cfg_get_trial(&t)) {
        upd.state = UPD_IDLE;
        return;
    }
    if (t.bank != active) {
        /* стартовал другой банк: либо откат состоялся, либо старая запись */
        if (!t.confirmed) {
            err_push(ERR_UPD_SEQ, "update rolled back");
            log_event("update: rollback to bank %u", active);
            t.confirmed = 2; /* закрыть запись, чтобы не сообщать повторно */
            t.bank = active;
            cfg_set_trial(&t);
        }
        upd.state = UPD_IDLE;
        return;
    }
    if (t.confirmed) {
        upd.state = UPD_IDLE;
        return;
    }
    if (t.boot_count >= 1) {
        /* второй старт без подтверждения: откат */
        log_event("update: no confirm, rollback");
        hal_bank_set_boot(active == 1u ? 2u : 1u);
        hal_reset();
        return; /* хост */
    }
    t.boot_count++;
    cfg_set_trial(&t);
    upd.state = UPD_TRIAL;
    upd.trial_pending = true;
    upd.trial_started_ms = hal_millis();
    log_event("update: trial run bank %u", active);
}

void upd_poll(void)
{
    if (upd.state == UPD_TRIAL && upd.trial_pending
        && (uint32_t)(hal_millis() - upd.trial_started_ms) >= UPD_CONFIRM_TIMEOUT_MS) {
        upd.trial_pending = false;
        log_event("update: confirm timeout, rollback");
        uint8_t active = hal_bank_active();
        hal_bank_set_boot(active == 1u ? 2u : 1u);
        hal_reset();
    }
}

/* ---- команды ---- */

static void reset_receiving(void)
{
    upd.state = upd.state == UPD_RECEIVING ? UPD_IDLE : upd.state;
    upd.received = 0;
    upd.crc = 0;
    upd.carry_len = 0;
}

void cmd_upd_begin(struct cmdctx *c)
{
    if (upd.state == UPD_TRIAL) {
        resp_err(c, ERR_UPD_SEQ, "trial pending");
        return;
    }
    if (!observed.in_safe_state) {
        resp_err(c, ERR_NOT_SAFE, "STATE");
        return;
    }
    struct hal_optbytes ob = hal_optbytes_read();
    if (ob.ndbank != (OPT_NDBANK != 0) || ob.ndboot != (OPT_NDBOOT != 0) || ob.iwdg_sw != (OPT_IWDG_SW != 0)) {
        resp_err(c, ERR_NOT_SAFE, "OPTBYTES");
        return;
    }
    unsigned long size, crc;
    char tail;
    uint32_t ver;
    if (sscanf(c->argv[0], "%lu%c", &size, &tail) != 1 || sscanf(c->argv[1], "%lx%c", &crc, &tail) != 1
        || !version_parse(c->argv[2], &ver)) {
        resp_err(c, ERR_RANGE, "args");
        return;
    }
    if (size < FW_HEADER_OFFSET + sizeof(struct fw_header) || size > IMAGE_MAX) {
        resp_err(c, ERR_RANGE, "size");
        return;
    }
    uint32_t minv;
    version_parse(UPD_MIN_VERSION, &minv);
    if (ver < minv) {
        resp_err(c, ERR_UPD_HEADER, "version");
        return;
    }
    /* стирание неактивного банка посекторно; между секторами — служебный шаг */
    for (uint32_t s = 0; s < HAL_BANK_SECTORS; s++) {
        if (hal_bank_erase_inactive(s) != 0) {
            resp_err(c, ERR_UPD_SEQ, "erase");
            return;
        }
        svc_poll();
    }
    upd.state = UPD_RECEIVING;
    upd.expected_size = (uint32_t)size;
    upd.expected_crc = (uint32_t)crc;
    upd.received = 0;
    upd.crc = 0;
    upd.carry_len = 0;
    resp_ok(c);
}

static int write_bytes(const uint8_t *data, size_t len)
{
    /* накапливаем до кратности 4 */
    uint8_t buf[UPD_BLOCK_MAX_B + 4];
    size_t n = 0;
    memcpy(buf, upd.carry, upd.carry_len);
    n = upd.carry_len;
    memcpy(buf + n, data, len);
    n += len;
    size_t aligned = n & ~3u;
    if (aligned) {
        uint32_t offset = upd.received - (uint32_t)upd.carry_len;
        if (hal_bank_write(offset, buf, aligned) != 0) {
            return -1;
        }
    }
    upd.carry_len = n - aligned;
    memcpy(upd.carry, buf + aligned, upd.carry_len);
    return 0;
}

void cmd_upd_data(struct cmdctx *c)
{
    if (upd.state != UPD_RECEIVING) {
        resp_err(c, ERR_UPD_SEQ, NULL);
        return;
    }
    uint8_t block[UPD_BLOCK_MAX_B + 3];
    int n = base64_decode(c->argv[0], block, sizeof block);
    if (n < 0 || n > UPD_BLOCK_MAX_B) {
        resp_err(c, ERR_RANGE, "block");
        return;
    }
    if (upd.received + (uint32_t)n > upd.expected_size) {
        resp_err(c, ERR_RANGE, "size");
        return;
    }
    if (write_bytes(block, (size_t)n) != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_SEQ, "write");
        return;
    }
    upd.crc = crc32_update(upd.crc, block, (size_t)n);
    upd.received += (uint32_t)n;
    resp_ok(c);
}

static int flush_carry(void)
{
    if (upd.carry_len == 0) {
        return 0;
    }
    uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(buf, upd.carry, upd.carry_len);
    uint32_t offset = upd.received - (uint32_t)upd.carry_len;
    upd.carry_len = 0;
    return hal_bank_write(offset, buf, 4);
}

static int copy_cfg_to_inactive(void)
{
    uint8_t snap[3 * CFG_RECORD_SIZE];
    size_t n = cfg_snapshot(snap, sizeof snap);
    if (hal_bank_erase_inactive(HAL_BANK_SECTORS - 1u) != 0) {
        return -1;
    }
    return n ? hal_bank_write(CFG_TAIL_OFFSET, snap, n) : 0;
}

void cmd_upd_commit(struct cmdctx *c)
{
    if (upd.state != UPD_RECEIVING) {
        resp_err(c, ERR_UPD_SEQ, NULL);
        return;
    }
    if (upd.received != upd.expected_size) {
        resp_err(c, ERR_UPD_CRC, "size");
        return;
    }
    if (flush_carry() != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_SEQ, "write");
        return;
    }
    const uint8_t *img = hal_bank_inactive_base();
    uint32_t crc = crc32_update(0, img, upd.expected_size);
    if (crc != upd.expected_crc || crc != upd.crc) {
        reset_receiving();
        resp_err(c, ERR_UPD_CRC, NULL);
        return;
    }
    struct fw_header h;
    memcpy(&h, img + FW_HEADER_OFFSET, sizeof h);
    if (memcmp(h.magic, FW_HEADER_MAGIC, 8) != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "magic");
        return;
    }
    if (strncmp(h.model, FW_MODEL, 8) != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "model");
        return;
    }
    if (h.revision != BOARD_REV) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "revision");
        return;
    }
    if (h.image_size != upd.expected_size) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "size");
        return;
    }
    uint32_t minv, ver = ((uint32_t)h.ver_major << 16) | ((uint32_t)h.ver_minor << 8) | h.ver_patch;
    version_parse(UPD_MIN_VERSION, &minv);
    if (ver < minv) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "version");
        return;
    }
    /* Match the board linker map: 512 KiB RAM and code after the header.
     * Reset must select Thumb and point to a complete instruction in this image. */
    uint32_t sp, reset;
    memcpy(&sp, img, sizeof sp);
    memcpy(&reset, img + 4, sizeof reset);
    uint32_t entry = reset & ~1u;
    if (sp <= 0x20000000u || sp > 0x20080000u || (sp & 7u)
        || !(reset & 1u) || entry < 0x08000000u + FW_HEADER_OFFSET + sizeof(struct fw_header)
        || entry > 0x08000000u + upd.expected_size - 2u) {
        reset_receiving();
        resp_err(c, ERR_UPD_HEADER, "vectors");
        return;
    }
    /* место под дозапись на старте: не меньше двух свободных записей (А11) */
    if (cfg_free_records() < 2 && cfg_compact() != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_SEQ, "cfg");
        return;
    }
    uint8_t target = hal_bank_active() == 1u ? 2u : 1u;
    struct cfg_trial t = {.bank = target, .boot_count = 0, .confirmed = 0};
    if (cfg_set_trial(&t) != 0 || copy_cfg_to_inactive() != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_SEQ, "cfg");
        return;
    }
    log_event("update: commit %u.%u.%u to bank %u", h.ver_major, h.ver_minor, h.ver_patch, target);
    if (hal_bank_set_boot(target) != 0) {
        reset_receiving();
        resp_err(c, ERR_UPD_SEQ, "boot");
        return;
    }
    resp_ok(c);
    hal_reset();
}

void cmd_upd_abort(struct cmdctx *c)
{
    if (upd.state == UPD_RECEIVING) {
        reset_receiving();
    }
    resp_ok(c);
}

void cmd_upd_stat(struct cmdctx *c)
{
    resp_printf(c, "%s,%lu,%lu", upd_state_name(upd.state), (unsigned long)upd.received,
                (unsigned long)(upd.state == UPD_RECEIVING ? upd.expected_size : 0u));
    resp_end(c);
}

void cmd_upd_confirm(struct cmdctx *c)
{
    if (upd.state != UPD_TRIAL || !upd.trial_pending) {
        resp_err(c, ERR_UPD_SEQ, NULL);
        return;
    }
    struct cfg_trial t;
    if (!cfg_get_trial(&t)) {
        resp_err(c, ERR_UPD_SEQ, NULL);
        return;
    }
    t.confirmed = 1;
    if (cfg_set_trial(&t) != 0) {
        resp_err(c, ERR_UPD_SEQ, "cfg");
        return;
    }
    upd.trial_pending = false;
    upd.state = UPD_CONFIRMED;
    log_event("update: confirmed");
    resp_ok(c);
}
