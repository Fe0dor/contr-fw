/* Интерфейс слоя железа (архитектура А2 ТЗ 19).
 *
 * Две реализации: hal/target — STM32F767ZI на плате CONTR, hal/host — заглушки
 * с журналом вызовов и виртуальным временем для тестов на хосте (FW-246).
 * Шаг 0 покрывает только тик, светодиод LD1 и консоль USART3; остальные
 * интерфейсы (выводы реле, цепочки SR, SPI, I²C, сеть) добавляются по шагам.
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Тактирование, миллисекундный тик, светодиод, консоль. Вызывается один раз. */
void hal_init(void);

/* Миллисекунды с момента hal_init(); переполняется через 49 суток. */
uint32_t hal_millis(void);

/* Светодиод LD1 платы NUCLEO. */
void hal_led_set(bool on);

/* Консоль USART3: неблокирующее чтение принятых байтов, возвращает число байтов. */
size_t hal_console_read(uint8_t *buf, size_t max);

/* Консоль USART3: неблокирующая запись, возвращает число принятых к передаче байтов. */
size_t hal_console_write(const uint8_t *buf, size_t len);

#endif /* HAL_H */
