/* Точка входа прошивки CONTR. Шаг 0: мигание LD1 и эхо консоли USART3.
 * На шаге 2 сюда приходят board_early_init() и core_run() (А2, А6). */
#include "blink_echo.h"
#include "hal.h"

int main(void)
{
    static struct blink_echo state;

    hal_init();
    blink_echo_init(&state, hal_millis());
    for (;;) {
        blink_echo_step(&state);
    }
}
