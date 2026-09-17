/* Логика образа шага 0: мигание LD1 и эхо консоли USART3 (план 22, шаг 0).
 *
 * Временный «core» до появления суперцикла и таблицы команд на шаге 2 (А1, А4):
 * показывает, что сборка, загрузка и консоль работают. Логика не зависит от
 * железа и тестируется на хосте через hal/host.
 */
#ifndef CORE_BLINK_ECHO_H
#define CORE_BLINK_ECHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLINK_PERIOD_MS 500u

struct blink_echo {
    uint32_t next_toggle_ms;
    bool led_on;
    uint8_t pending[64];   /* принятые, но ещё не отданные в консоль байты */
    size_t pending_len;
};

void blink_echo_init(struct blink_echo *s, uint32_t now_ms);

/* Один шаг суперцикла: переключить LD1 по расписанию, вернуть принятые байты. */
void blink_echo_step(struct blink_echo *s);

#endif /* CORE_BLINK_ECHO_H */
