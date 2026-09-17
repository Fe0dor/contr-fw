/* Стек ошибок (FW-108) и кольцевой журнал событий (FW-217). */
#ifndef CORE_ERRLOG_H
#define CORE_ERRLOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmd.h"

#define ERR_STACK_DEPTH 16
#define ERR_TEXT_MAX 48
#define LOG_TEXT_MAX 40

struct err_entry {
    enum err_code code;
    char text[ERR_TEXT_MAX];
};

struct log_entry {
    uint32_t ms;
    char text[LOG_TEXT_MAX];
};

void errlog_init(void);

/* В стек ошибок; text может быть NULL. При переполнении старые вытесняются. */
void err_push(enum err_code code, const char *text);
size_t err_count(void);
/* Забрать старейшую запись; false — стек пуст. */
bool err_pop(struct err_entry *out);

/* В журнал событий с меткой времени. */
void log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
size_t log_count(void);
/* Запись по индексу от старейшей (0) к новейшей. */
const struct log_entry *log_at(size_t index);

#endif /* CORE_ERRLOG_H */
