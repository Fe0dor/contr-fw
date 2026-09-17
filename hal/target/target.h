/* Внутренние помощники hal/target: доступ к портам, альтернативные функции, ожидания. */
#ifndef HAL_TARGET_H
#define HAL_TARGET_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32f7xx.h"

#define SYSCLK_HZ 216000000u
#define HCLK_HZ SYSCLK_HZ
#define PCLK1_HZ (SYSCLK_HZ / 4u)
#define PCLK2_HZ (SYSCLK_HZ / 2u)

static inline GPIO_TypeDef *target_port(uint8_t port)
{
    return (GPIO_TypeDef *)(GPIOA_BASE + 0x400u * port);
}

/* Включить тактирование порта и настроить вывод под альтернативную функцию. */
void target_gpio_af(uint8_t port, uint8_t pin, uint8_t af, bool open_drain, bool pull_up);

/* Ожидание условия с таймаутом в мс; возвращает false по таймауту. */
#define TARGET_WAIT_UNTIL(cond, timeout_ms)                                    \
    ({                                                                         \
        uint32_t _t0 = hal_millis();                                           \
        bool _ok = true;                                                       \
        while (!(cond)) {                                                      \
            if ((uint32_t)(hal_millis() - _t0) > (timeout_ms)) {               \
                _ok = false;                                                   \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        _ok;                                                                   \
    })

#endif /* HAL_TARGET_H */
