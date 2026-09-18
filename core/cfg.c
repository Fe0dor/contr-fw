/* Журнал записей в секторе CFG (А10). Запись 64 байта:
 *   [0..1] магия 0xC0F6, [2] тип, [3] длина данных, [4..59] данные, [60..63] CRC32
 * (по байтам 0..59). Незаписанная область — 0xFF. Действует последняя корректная
 * запись каждого типа. */
#include "cfg.h"

#include <string.h>

#include "hal.h"
#include "update.h" /* crc32_update */

#define CFG_MAGIC0 0xC0u
#define CFG_MAGIC1 0xF6u
#define CFG_DATA_MAX 56u
#define CFG_RESERVE 2u /* свободных записей, ниже которых уплотняем при COMMIT */

static size_t used_records;      /* число занятых слотов (включая испорченные) */
static int last_of_type[4];      /* индекс последней корректной записи типа, -1 нет */
static bool erase_allowed;

static const uint8_t *rec(size_t i)
{
    return hal_cfg_base() + i * CFG_RECORD_SIZE;
}

static bool rec_blank(const uint8_t *r)
{
    for (size_t i = 0; i < CFG_RECORD_SIZE; i++) {
        if (r[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static bool rec_valid(const uint8_t *r)
{
    if (r[0] != CFG_MAGIC0 || r[1] != CFG_MAGIC1 || r[3] > CFG_DATA_MAX || r[2] == 0 || r[2] > 3) {
        return false;
    }
    uint32_t crc = crc32_update(0, r, 60);
    uint32_t stored = (uint32_t)r[60] | ((uint32_t)r[61] << 8) | ((uint32_t)r[62] << 16) | ((uint32_t)r[63] << 24);
    return crc == stored;
}

void cfg_init(void)
{
    used_records = 0;
    for (int t = 0; t < 4; t++) {
        last_of_type[t] = -1;
    }
    for (size_t i = 0; i < CFG_RECORDS; i++) {
        const uint8_t *r = rec(i);
        if (rec_blank(r)) {
            break;
        }
        used_records = i + 1;
        if (rec_valid(r)) {
            last_of_type[r[2]] = (int)i;
        }
    }
}

static bool get(enum cfg_type type, uint8_t *data, size_t len)
{
    int i = last_of_type[type];
    if (i < 0) {
        return false;
    }
    const uint8_t *r = rec((size_t)i);
    if (r[3] < len) {
        return false;
    }
    memcpy(data, r + 4, len);
    return true;
}

static int write_record(enum cfg_type type, const uint8_t *data, size_t len)
{
    if (len > CFG_DATA_MAX) {
        return -1;
    }
    if (used_records >= CFG_RECORDS) {
        if (!erase_allowed || cfg_compact() != 0) {
            return -2;
        }
        if (used_records >= CFG_RECORDS) {
            return -2;
        }
    }
    uint8_t r[CFG_RECORD_SIZE];
    memset(r, 0xFF, sizeof r);
    r[0] = CFG_MAGIC0;
    r[1] = CFG_MAGIC1;
    r[2] = (uint8_t)type;
    r[3] = (uint8_t)len;
    memcpy(r + 4, data, len);
    uint32_t crc = crc32_update(0, r, 60);
    r[60] = (uint8_t)crc;
    r[61] = (uint8_t)(crc >> 8);
    r[62] = (uint8_t)(crc >> 16);
    r[63] = (uint8_t)(crc >> 24);
    size_t slot = used_records;
    if (hal_cfg_write((uint32_t)(slot * CFG_RECORD_SIZE), r, sizeof r) != 0) {
        used_records = slot + 1; /* слот испорчен, дальше не используем */
        return -1;
    }
    used_records = slot + 1;
    last_of_type[type] = (int)slot;
    return 0;
}

bool cfg_get_serial(char out[CFG_SERIAL_MAX + 1])
{
    int i = last_of_type[CFG_SERIAL];
    if (i < 0) {
        return false;
    }
    const uint8_t *r = rec((size_t)i);
    size_t len = r[3] > CFG_SERIAL_MAX ? CFG_SERIAL_MAX : r[3];
    memcpy(out, r + 4, len);
    out[len] = '\0';
    return true;
}

bool cfg_get_net(struct cfg_net *out)
{
    uint8_t d[13];
    if (!get(CFG_NET, d, sizeof d)) {
        return false;
    }
    out->dhcp = d[0];
    memcpy(&out->addr, d + 1, 4);
    memcpy(&out->mask, d + 5, 4);
    memcpy(&out->gw, d + 9, 4);
    return true;
}

bool cfg_get_trial(struct cfg_trial *out)
{
    uint8_t d[3];
    if (!get(CFG_TRIAL, d, sizeof d)) {
        return false;
    }
    out->bank = d[0];
    out->boot_count = d[1];
    out->confirmed = d[2];
    return true;
}

int cfg_set_serial(const char *serial)
{
    size_t len = strlen(serial);
    if (len == 0 || len > CFG_SERIAL_MAX) {
        return -1;
    }
    return write_record(CFG_SERIAL, (const uint8_t *)serial, len);
}

int cfg_set_net(const struct cfg_net *net)
{
    uint8_t d[13];
    d[0] = net->dhcp;
    memcpy(d + 1, &net->addr, 4);
    memcpy(d + 5, &net->mask, 4);
    memcpy(d + 9, &net->gw, 4);
    return write_record(CFG_NET, d, sizeof d);
}

int cfg_set_trial(const struct cfg_trial *trial)
{
    uint8_t d[3] = {trial->bank, trial->boot_count, trial->confirmed};
    return write_record(CFG_TRIAL, d, sizeof d);
}

size_t cfg_free_records(void)
{
    return CFG_RECORDS - used_records;
}

size_t cfg_snapshot(uint8_t *out, size_t max)
{
    size_t n = 0;
    for (int t = 1; t <= 3; t++) {
        int i = last_of_type[t];
        if (i < 0) {
            continue;
        }
        if (n + CFG_RECORD_SIZE > max) {
            break;
        }
        memcpy(out + n, rec((size_t)i), CFG_RECORD_SIZE);
        n += CFG_RECORD_SIZE;
    }
    return n;
}

int cfg_compact(void)
{
    /* One-sector compaction is not power-fail safe. Until a versioned durable
     * backup/recovery protocol exists, preserve provisioning and fail closed. */
    return -2;
}

void cfg_set_erase_allowed(bool allowed)
{
    erase_allowed = allowed;
}
