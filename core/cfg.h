/* Энергонезависимые данные (А10): сектор CFG как журнал записей фиксированного формата.
 * Действует последняя корректная запись своего типа. Уплотнение использует отдельный резервный сектор и восстановление после сброса. */
#ifndef CORE_CFG_H
#define CORE_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_RECORD_SIZE 64u
#define CFG_RECORDS (16u * 1024u / CFG_RECORD_SIZE)
#define CFG_SERIAL_MAX 32u

enum cfg_type {
    CFG_SERIAL = 1,
    CFG_NET = 2,
    CFG_TRIAL = 3,
    CFG_PSU_CAL = 4,
    CFG_TYPE_COUNT = 5,
};

#define PSU_CAL_POINTS 15u
#define CFG_SNAPSHOT_MAX ((CFG_TYPE_COUNT-1u)*CFG_RECORD_SIZE)
struct cfg_psu_cal { uint16_t code[PSU_CAL_POINTS]; };
bool cfg_get_psu_cal(struct cfg_psu_cal *out);
int cfg_set_psu_cal(const struct cfg_psu_cal *cal);
bool cfg_psu_cal_valid(const struct cfg_psu_cal *cal);

struct cfg_net {
    uint8_t dhcp;
    uint32_t addr, mask, gw; /* host order */
};

struct cfg_trial {
    uint8_t bank;        /* банк пробного запуска: 1 или 2 */
    uint8_t boot_count;  /* число стартов этого образа */
    uint8_t confirmed;   /* 1 — SYST:UPD:CONFIRM получен */
};

/* Просканировать сектор; вызывать один раз при старте. */
void cfg_init(void);

bool cfg_get_serial(char out[CFG_SERIAL_MAX + 1]);
bool cfg_get_net(struct cfg_net *out);
bool cfg_get_trial(struct cfg_trial *out);

/* Запись новой записи; -1 при ошибке flash, -2 если места нет и уплотнение невозможно. */
int cfg_set_serial(const char *serial);
int cfg_set_net(const struct cfg_net *net);
int cfg_set_trial(const struct cfg_trial *trial);

/* Свободные записи; 0 также означает незавершённое восстановление flash. */
size_t cfg_free_records(void);
int cfg_compact(void);

/* Копия действующих записей в буфер (для копирования CFG в другой банк): число байт. */
size_t cfg_snapshot(uint8_t *out, size_t max);

/* Разрешено ли стирать сектор сейчас (задаёт ядро: безопасное состояние). */
void cfg_set_erase_allowed(bool allowed);

#endif /* CORE_CFG_H */
