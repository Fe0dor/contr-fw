/* Сборка hal_init(): тактирование, тик, консоль, SPI A, I²C. Цепочки SR и GPIO
 * работают до этого (board_early_init ядра, FW-209). */
#include "hal.h"

void hal_clock_init(void);
void hal_console_init(void);
void hal_spia_init(void);
void hal_i2c_init(void);

void hal_init(void)
{
    hal_clock_init();
    hal_console_init();
    hal_spia_init();
    hal_i2c_init();
}
