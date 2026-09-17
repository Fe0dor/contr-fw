/* Управление заглушками hal/host из тестов: виртуальное время, ввод консоли,
 * перехват вывода и состояния светодиода (А2, А13). */
#ifndef HAL_HOST_H
#define HAL_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Сброс всех заглушек в начальное состояние; вызывать в начале каждого теста. */
void host_reset(void);

/* Виртуальное время: продвинуть на ms миллисекунд. */
void host_advance_ms(uint32_t ms);

/* Положить байты во входное кольцо консоли, как будто их прислал терминал. */
void host_console_push(const uint8_t *buf, size_t len);

/* Забрать всё, что прошивка записала в консоль; возвращает число байтов. */
size_t host_console_take(uint8_t *buf, size_t max);

/* Предел байтов, которые hal_console_write принимает за один вызов (0 — без предела). */
void host_console_set_write_limit(size_t limit);

bool host_led_state(void);

/* Число вызовов hal_led_set с момента host_reset. */
unsigned host_led_writes(void);

#endif /* HAL_HOST_H */
