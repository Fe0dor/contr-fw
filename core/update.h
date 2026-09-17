/* Обновление по Ethernet во второй банк с пробным запуском и откатом (А11, FW-232…FW-238). */
#ifndef CORE_UPDATE_H
#define CORE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmd.h"

/* Заголовок образа: секция .fw_header по смещению FW_HEADER_OFFSET от начала образа. */
#define FW_HEADER_OFFSET 0x200u
#define FW_HEADER_MAGIC "CONTRFW1"

struct fw_header {
    char magic[8];       /* "CONTRFW1" */
    char model[8];       /* "CONTR" с нулями */
    uint8_t revision;    /* BOARD_REV */
    uint8_t ver_major, ver_minor, ver_patch;
    uint32_t image_size; /* заполняет tools/mkimage.py; 0xFFFFFFFF в сыром .bin */
    uint32_t reserved[3];
};

enum upd_state {
    UPD_IDLE = 0,
    UPD_RECEIVING,
    UPD_TRIAL,     /* образ запущен, ждёт CONFIRM */
    UPD_CONFIRMED,
};

const char *upd_state_name(enum upd_state s);
enum upd_state upd_get_state(void);

/* Старт: логика загрузчика — откат, счётчик стартов, таймер подтверждения. */
void upd_boot_check(void);

/* Служебный шаг: истечение таймера подтверждения. */
void upd_poll(void);

/* Обработчики команд. */
void cmd_upd_begin(struct cmdctx *c);
void cmd_upd_data(struct cmdctx *c);
void cmd_upd_commit(struct cmdctx *c);
void cmd_upd_abort(struct cmdctx *c);
void cmd_upd_stat(struct cmdctx *c);
void cmd_upd_confirm(struct cmdctx *c);

/* CRC32 (IEEE 802.3, как zlib.crc32). */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* base64 → байты; возвращает длину или -1. */
int base64_decode(const char *in, uint8_t *out, size_t max);

/* Версия строкой "a.b.c" → число для сравнения; false при ошибке. */
bool version_parse(const char *s, uint32_t *out);

#endif /* CORE_UPDATE_H */
