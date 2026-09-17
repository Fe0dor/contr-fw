/* Суперцикл, каналы, ожидания (А1), порядок старта (А6, FW-209). */
#ifndef CORE_CORE_H
#define CORE_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum channel_id { CH_TCP = 0, CH_CONSOLE = 1, CH_COUNT = 2 };

enum wait_result { WAIT_DONE = 0, WAIT_ABORTED = 1 };

/* Первые строки main(): только выводы в порядке FW-209, без ожиданий. */
void board_early_init(void);

/* Всё после board_early_init(): hal_init, watchdog, CFG, сеть, стартовые записи. */
void core_init(void);

/* Один проход суперцикла: события каналов, одна команда, служебный шаг. */
void core_step(void);

/* Бесконечный суперцикл. */
void core_run(void);

/* Служебный шаг: watchdog, сеть, консоль, кнопка, таймеры. */
void svc_poll(void);

/* Ожидание до момента deadline (мс hal_millis) с прокруткой svc_poll(); ABORTED по SAFE. */
enum wait_result wait_until(uint32_t deadline_ms);

/* Ожидание условия: cond() истинно либо deadline. *timed_out выставляется при таймауте. */
enum wait_result wait_for(bool (*cond)(void *), void *arg, uint32_t deadline_ms, bool *timed_out);

/* Текущее ожидание должно прерваться (пришёл SAFE). */
bool core_abort_requested(void);

/* Запись в канал с ожиданием места; false — канал закрыт. */
bool channel_write(enum channel_id ch, const uint8_t *buf, size_t len);

/* Активный TCP-клиент подключён. */
bool core_client_connected(void);

#endif /* CORE_CORE_H */
