#include "errlog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hal.h"

static struct {
    struct err_entry items[ERR_STACK_DEPTH];
    size_t head, count;
} errs;

static struct {
    struct log_entry items[LOG_DEPTH];
    size_t head, count;
} logs;

void errlog_init(void)
{
    memset(&errs, 0, sizeof errs);
    memset(&logs, 0, sizeof logs);
}

void err_push(enum err_code code, const char *text)
{
    size_t idx = (errs.head + errs.count) % ERR_STACK_DEPTH;
    if (errs.count == ERR_STACK_DEPTH) {
        errs.head = (errs.head + 1) % ERR_STACK_DEPTH; /* вытеснить старейшую */
        idx = (errs.head + errs.count - 1) % ERR_STACK_DEPTH;
    } else {
        errs.count++;
    }
    errs.items[idx].code = code;
    strncpy(errs.items[idx].text, text ? text : "", ERR_TEXT_MAX - 1);
    errs.items[idx].text[ERR_TEXT_MAX - 1] = '\0';
}

size_t err_count(void)
{
    return errs.count;
}

bool err_pop(struct err_entry *out)
{
    if (errs.count == 0) {
        return false;
    }
    *out = errs.items[errs.head];
    errs.head = (errs.head + 1) % ERR_STACK_DEPTH;
    errs.count--;
    return true;
}

void log_event(const char *fmt, ...)
{
    size_t idx = (logs.head + logs.count) % LOG_DEPTH;
    if (logs.count == LOG_DEPTH) {
        logs.head = (logs.head + 1) % LOG_DEPTH;
        idx = (logs.head + logs.count - 1) % LOG_DEPTH;
    } else {
        logs.count++;
    }
    logs.items[idx].ms = hal_millis();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs.items[idx].text, LOG_TEXT_MAX, fmt, ap);
    va_end(ap);
}

size_t log_count(void)
{
    return logs.count;
}

const struct log_entry *log_at(size_t index)
{
    if (index >= logs.count) {
        return NULL;
    }
    return &logs.items[(logs.head + index) % LOG_DEPTH];
}
