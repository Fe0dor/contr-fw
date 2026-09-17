/* GPIO: конфигурация по (порт, вывод), альтернативные функции, кнопка USER, JTAG. */
#include "hal.h"
#include "target.h"

static void port_clock_on(uint8_t port)
{
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN << port);
    (void)RCC->AHB1ENR;
}

void hal_gpio_config(uint8_t port, uint8_t pin, enum hal_pin_mode mode, enum hal_pull pull, bool level)
{
    GPIO_TypeDef *g = target_port(port);
    uint32_t shift = 2u * pin;
    port_clock_on(port);
    /* уровень выставляется до переключения в выход: без импульса на линии */
    g->BSRR = level ? (1u << pin) : (1u << (pin + 16u));
    g->PUPDR = (g->PUPDR & ~(3u << shift)) | ((uint32_t)pull << shift);
    g->OTYPER &= ~(1u << pin);
    g->OSPEEDR &= ~(3u << shift); /* низкая скорость: реле и линии изделия */
    g->MODER = (g->MODER & ~(3u << shift)) | ((mode == HAL_PIN_OUT ? 1u : 0u) << shift);
}

void hal_gpio_write(uint8_t port, uint8_t pin, bool level)
{
    target_port(port)->BSRR = level ? (1u << pin) : (1u << (pin + 16u));
}

bool hal_gpio_read(uint8_t port, uint8_t pin)
{
    return (target_port(port)->IDR >> pin) & 1u;
}

void target_gpio_af(uint8_t port, uint8_t pin, uint8_t af, bool open_drain, bool pull_up)
{
    GPIO_TypeDef *g = target_port(port);
    uint32_t shift = 2u * pin;
    port_clock_on(port);
    g->AFR[pin / 8u] = (g->AFR[pin / 8u] & ~(0xFu << (4u * (pin % 8u)))) | ((uint32_t)af << (4u * (pin % 8u)));
    if (open_drain) {
        g->OTYPER |= (1u << pin);
    } else {
        g->OTYPER &= ~(1u << pin);
    }
    g->OSPEEDR = (g->OSPEEDR & ~(3u << shift)) | (2u << shift); /* высокая скорость для шин */
    g->PUPDR = (g->PUPDR & ~(3u << shift)) | ((pull_up ? 1u : 0u) << shift);
    g->MODER = (g->MODER & ~(3u << shift)) | (2u << shift);
}

void hal_jtag_release_pb4(void)
{
    /* На Cortex-M7 JTAG-выводы после сброса стоят в AF0 с подтяжкой: PB4 (NJTRST) вверх.
     * Переводим PB4 во вход с подтяжкой вниз — SWD на PA13/PA14 не затрагивается. */
    hal_gpio_config(1, 4, HAL_PIN_IN, HAL_PULL_DOWN, false);
}

bool hal_button_pressed(void)
{
    return hal_gpio_read(2, 13); /* PC13, нажата = 1 */
}
