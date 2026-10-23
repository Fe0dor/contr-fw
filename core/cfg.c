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
#define BACKUP_DATA 32u
#define BACKUP_COMMIT 0x314b4243u
static const uint8_t backup_magic[8] = {'C','F','G','B','K','0','0','2'};

static size_t used_records;      /* число занятых слотов (включая испорченные) */
static int last_of_type[CFG_TYPE_COUNT];      /* индекс последней корректной записи типа, -1 нет */
static bool erase_allowed;
static bool recovery_pending;
static const uint8_t *records_base;

static const uint8_t *rec(size_t i)
{
    return records_base + i * CFG_RECORD_SIZE;
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
    if (r[0] != CFG_MAGIC0 || r[1] != CFG_MAGIC1 || r[3] > CFG_DATA_MAX || r[2] == 0 || r[2] >= CFG_TYPE_COUNT) {
        return false;
    }
    uint32_t crc = crc32_update(0, r, 60);
    uint32_t stored = (uint32_t)r[60] | ((uint32_t)r[61] << 8) | ((uint32_t)r[62] << 16) | ((uint32_t)r[63] << 24);
    if (crc != stored) return false;
    if (r[2]==CFG_PSU_CAL) {
        if(r[3]!=32 || r[4]!=1 || r[5]!=PSU_CAL_POINTS) return false;
        for(unsigned i=1;i<PSU_CAL_POINTS;i++) {
            unsigned prev=r[6+2*(i-1)]|((unsigned)r[7+2*(i-1)]<<8);
            unsigned next=r[6+2*i]|((unsigned)r[7+2*i]<<8);
            if(next<=prev)return false;
        }
    }
    return true;
}

static uint32_t word(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static const uint8_t generation_magic[8]={'C','F','G','G','E','N','0','1'};
static uint32_t primary_generation(void)
{
    const uint8_t *p=hal_cfg_base()+HAL_CFG_SIZE;
    if(memcmp(p,generation_magic,8) || word(p+16)!=BACKUP_COMMIT
        || crc32_update(0,p,12)!=word(p+12)) return 0; /* legacy journal */
    return word(p+8);
}
static bool backup_valid(void)
{
    const uint8_t *b=hal_cfg_backup_base();
    uint32_t len=word(b+8);
    if (memcmp(b,backup_magic,8) || len>CFG_SNAPSHOT_MAX || len%CFG_RECORD_SIZE
        || word(b+16)!=BACKUP_COMMIT || word(b+20)!=UINT32_MAX) return false;
    if (!word(b+24) || word(b+24)==UINT32_MAX
        || crc32_update(crc32_update(0,b,16),b+24,4)!=word(b+28)) return false;
    if (crc32_update(0,b+BACKUP_DATA,len)!=word(b+12)) return false;
    for (size_t i=0;i<len;i+=CFG_RECORD_SIZE) if (!rec_valid(b+BACKUP_DATA+i)) return false;
    return true;
}

static bool backup_needed(void) {
    return backup_valid() && word(hal_cfg_backup_base()+24)>primary_generation();
}

/* The committed backup stays valid until a verified primary copy exists.
 * Any reset/error before retirement replays this operation on the next boot. */
static int restore_backup(void)
{
    const uint8_t *b=hal_cfg_backup_base();
    size_t len=word(b+8);
    uint8_t snapshot[CFG_SNAPSHOT_MAX];
    memcpy(snapshot,b+BACKUP_DATA,len);
    if (hal_cfg_erase()!=0) return -1;
    if (len && hal_cfg_write(0,snapshot,len)!=0) return -1;
    if (memcmp(hal_cfg_base(),snapshot,len)) return -1;
    uint8_t stamp[16];uint32_t generation=word(b+24), commit=BACKUP_COMMIT;
    memcpy(stamp,generation_magic,8);memcpy(stamp+8,&generation,4);
    uint32_t crc=crc32_update(0,stamp,12);memcpy(stamp+12,&crc,4);
    if(hal_cfg_write(HAL_CFG_SIZE,stamp,sizeof stamp)!=0
        || hal_cfg_write(HAL_CFG_SIZE+16,(const uint8_t *)&commit,4)!=0) return -1;
    if(primary_generation()!=generation) return -1;
    uint32_t retired=0;
    return hal_cfg_backup_write(20,(const uint8_t *)&retired,4);
}

void cfg_init(void)
{
    erase_allowed=false;
    records_base=hal_cfg_base();
    recovery_pending=false;
    if (backup_needed() && restore_backup()!=0 && backup_needed()) {
        /* Persistent flash failure: retain readable provisioning, reject writes. */
        records_base=hal_cfg_backup_base()+BACKUP_DATA;
        recovery_pending=true;
    }
    used_records = 0;
    for (int t = 0; t < CFG_TYPE_COUNT; t++) last_of_type[t] = -1;
    for (size_t i = 0; i < CFG_RECORDS; i++) {
        const uint8_t *r = rec(i);
        if (rec_blank(r)) continue; /* failed writes can leave erased holes */
        used_records = i + 1;
        if (rec_valid(r)) last_of_type[r[2]] = (int)i;
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
    if (recovery_pending) return -1;
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
    return recovery_pending ? 0 : CFG_RECORDS - used_records;
}

size_t cfg_snapshot(uint8_t *out, size_t max)
{
    size_t n = 0;
    for (int t = 1; t < CFG_TYPE_COUNT; t++) {
        int i = last_of_type[t];
        if (i < 0) {
            continue;
        }
        if (n + CFG_RECORD_SIZE > max) {
            return SIZE_MAX;
        }
        memcpy(out + n, rec((size_t)i), CFG_RECORD_SIZE);
        n += CFG_RECORD_SIZE;
    }
    return n;
}

int cfg_compact(void)
{
    if (!erase_allowed) return -2;
    if (recovery_pending) {
        cfg_init();
        erase_allowed=true;
        return recovery_pending ? -1 : 0;
    }
    uint8_t snapshot[CFG_SNAPSHOT_MAX], header[32];
    memset(header,0xff,sizeof header);
    uint32_t generation=primary_generation();
    if(generation>=UINT32_MAX-1u)return -2;
    generation++;
    size_t len=cfg_snapshot(snapshot,sizeof snapshot);
    if(len==SIZE_MAX)return -1;
    uint32_t size=(uint32_t)len, crc=crc32_update(0,snapshot,len), commit=BACKUP_COMMIT;
    memcpy(header,backup_magic,8);memcpy(header+8,&size,4);memcpy(header+12,&crc,4);
    memcpy(header+24,&generation,4);
    uint32_t meta_crc=crc32_update(crc32_update(0,header,16),header+24,4);
    memcpy(header+28,&meta_crc,4);
    /* No erase of primary until the entire backup is durable and verified. */
    if (hal_cfg_backup_erase()!=0 || hal_cfg_backup_write(0,header,sizeof header)!=0
        || (len && hal_cfg_backup_write(BACKUP_DATA,snapshot,len)!=0)) return -1;
    if (memcmp(hal_cfg_backup_base(),header,sizeof header)
        || memcmp(hal_cfg_backup_base()+BACKUP_DATA,snapshot,len)) return -1;
    if (hal_cfg_backup_write(16,(const uint8_t *)&commit,4)!=0) {
        /* A write may fail after programming the commit. Recover before any
         * later append, otherwise a boot could replay an older snapshot. */
        if (backup_valid()) { cfg_init(); erase_allowed=true; }
        return -1;
    }
    if (!backup_valid()) return -1;
    cfg_init(); /* restore primary and retire backup, or leave read-only fallback */
    erase_allowed=true;
    return recovery_pending ? -1 : 0;
}

void cfg_set_erase_allowed(bool allowed)
{
    erase_allowed = allowed;
}

bool cfg_psu_cal_valid(const struct cfg_psu_cal *cal) {
    for(unsigned i=1;i<PSU_CAL_POINTS;i++)if(cal->code[i]<=cal->code[i-1])return false;
    return true;
}
bool cfg_get_psu_cal(struct cfg_psu_cal *out) {
    uint8_t d[32];if(!get(CFG_PSU_CAL,d,sizeof d))return false;
    if(d[0]!=1||d[1]!=PSU_CAL_POINTS)return false;
    for(unsigned i=0;i<PSU_CAL_POINTS;i++)out->code[i]=(uint16_t)(d[2+2*i]|((unsigned)d[3+2*i]<<8));
    return cfg_psu_cal_valid(out);
}
int cfg_set_psu_cal(const struct cfg_psu_cal *cal) {
    if(!cfg_psu_cal_valid(cal))return -1;
    uint8_t d[32]={1,PSU_CAL_POINTS};
    for(unsigned i=0;i<PSU_CAL_POINTS;i++){d[2+2*i]=(uint8_t)cal->code[i];d[3+2*i]=(uint8_t)(cal->code[i]>>8);}
    return write_record(CFG_PSU_CAL,d,sizeof d);
}
