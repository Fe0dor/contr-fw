/* Минимальный каркас тестов хоста: CHECK, прогон ядра над заглушками, обмен строками. */
#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <stdio.h>
#include <string.h>

#include "core.h"
#include "hal_host.h"
#include "state.h"

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_STR(actual, expected)                                                            \
    do {                                                                                       \
        const char *_a = (actual), *_e = (expected);                                           \
        if (strcmp(_a, _e) != 0) {                                                             \
            failures++;                                                                        \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, _e, _a); \
        }                                                                                      \
    } while (0)

#define CHECK_PREFIX(actual, prefix)                                                             \
    do {                                                                                         \
        const char *_a = (actual), *_p = (prefix);                                               \
        if (strncmp(_a, _p, strlen(_p)) != 0) {                                                  \
            failures++;                                                                          \
            fprintf(stderr, "%s:%d: expected prefix \"%s\", got \"%s\"\n", __FILE__, __LINE__, _p, _a); \
        }                                                                                        \
    } while (0)

/* Свежее устройство: заглушки, ранний старт, инициализация. */
static inline void device_boot(void)
{
    host_reset_all();
    board_early_init();
    core_init();
}

/* Несколько проходов суперцикла: события каналов, команды. */
static inline void device_run(int steps)
{
    for (int i = 0; i < steps; i++) {
        core_step();
    }
}

static char reply_buf[8192];

/* Команда по TCP → строка ответа (без '\n'); "" если ответа нет. */
static inline const char *tcp_cmd(const char *line)
{
    host_net_push_line(line);
    device_run(4);
    if (!host_net_take_line(reply_buf, sizeof reply_buf)) {
        reply_buf[0] = '\0';
    }
    return reply_buf;
}

/* Команда с консоли → строка ответа. */
static inline const char *con_cmd(const char *line)
{
    host_console_push_line(line);
    device_run(4);
    if (!host_console_take_line(reply_buf, sizeof reply_buf)) {
        reply_buf[0] = '\0';
    }
    return reply_buf;
}

/* Подключить клиента и прокрутить вход в безопасное состояние. */
static inline bool tcp_connect(void)
{
    if (!host_net_connect()) {
        return false;
    }
    device_run(2);
    return true;
}

static inline int test_result(const char *name)
{
    if (failures) {
        fprintf(stderr, "%s: %d failure(s)\n", name, failures);
        return 1;
    }
    printf("%s: OK\n", name);
    return 0;
}

#endif /* TESTS_CHECK_H */
