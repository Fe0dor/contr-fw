/* Таблица команд 3.0, парсер и форматтер ответа (А4). */
#ifndef CORE_CMD_H
#define CORE_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"

#define CMD_MAX_ARGS 81
#define CMD_LINE_MAX 512 /* SYST:UPD:DATA: 256 байт → 344 символа base64 плюс имя */

/* Коды ошибок раздела 4; строки — cmd_err_name(). */
enum err_code {
    ERR_NONE = 0,
    ERR_UNKNOWN_CMD,
    ERR_RANGE,
    ERR_INTERLOCK,
    ERR_PSU_FAULT,
    ERR_PSU_ALARM,
    ERR_I2C_FAIL,
    ERR_SECTION_OFF,
    ERR_SECTION_FAIL,
    ERR_LOADBOARD_FAIL,
    ERR_LOADBOARD_VERSION,
    ERR_SAFE_INCOMPLETE,
    ERR_ABORTED,
    ERR_UPD_SEQ,
    ERR_UPD_CRC,
    ERR_UPD_HEADER,
    ERR_NOT_SAFE,
    ERR_PROVISIONED,
    ERR_CONSOLE_ONLY,
    ERR_BUSY,
};

const char *cmd_err_name(enum err_code code);

/* Флаги команды. */
#define CMDF_QUERY 0x01u        /* не меняет состояние: разрешена с консоли при клиенте */
#define CMDF_CONSOLE_ONLY 0x02u /* SYST:PROV:* */
#define CMDF_SAFE 0x04u         /* SAFE: принимается всегда */
#define CMDF_SET 0x08u          /* установка: снимает признак безопасного состояния */
#define CMDF_BRINGUP 0x10u      /* DBG:* наладочной сборки */

struct cmdctx;

typedef void (*cmd_handler)(struct cmdctx *c);

struct cmd_desc {
    const char *name;
    uint8_t min_args, max_args;
    uint8_t flags;
    cmd_handler fn;
};

struct cmdctx {
    enum channel_id ch;
    int argc;
    char *argv[CMD_MAX_ARGS];
    const struct cmd_desc *cmd;
    /* ответ */
    char buf[256];
    size_t len;
    bool started, ended, failed, closed;
    enum err_code err;
};

/* Ответ. Обработчик обязан завершить кадр одним из: resp_ok, resp_err, resp_end. */
void resp_puts(struct cmdctx *c, const char *s);
void resp_putc(struct cmdctx *c, char ch);
void resp_printf(struct cmdctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void resp_ok(struct cmdctx *c);
void resp_err(struct cmdctx *c, enum err_code code, const char *fields); /* fields может быть NULL */
void resp_end(struct cmdctx *c); /* завершить кадр данных */

/* Исполнить строку из канала: разбор, арбитраж, обработчик, один кадр ответа. */
void cmd_execute(enum channel_id ch, char *line);

/* Ответить ERR:BUSY на строку, пришедшую во время исполнения (FW-113). */
void cmd_reply_busy(enum channel_id ch);

/* Ответить ERR:RANGE на слишком длинную строку. */
void cmd_reply_overflow(enum channel_id ch);

/* Таблица для самопроверки и автодополнения. */
const struct cmd_desc *cmd_table(size_t *count);

/* true, если строка (без пробелов) — команда SAFE. */
bool cmd_is_safe_line(const char *line);

#endif /* CORE_CMD_H */
