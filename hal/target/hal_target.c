/* hal/target для NUCLEO-F767ZI на плате CONTR: шаг 0 — тик, LD1, консоль USART3.
 *
 * Тактирование после сброса: HSI 16 МГц без PLL (SystemInit из CMSIS), APB1 = 16 МГц.
 * LD1 — PB0. USART3 — PD8 (TX) / PD9 (RX), AF7, 115200 8N1; на NUCLEO эти выводы
 * идут на виртуальный COM-порт отладчика. Приём — в прерывании в кольцевой буфер,
 * логики в прерывании нет (А1); передача — по готовности TXE, без блокировки.
 */
#include "hal.h"

#include "stm32f7xx.h"

#define CONSOLE_BAUD 115200u
#define RX_RING_SIZE 256u

static volatile uint32_t tick_ms;

static struct {
    volatile uint8_t data[RX_RING_SIZE];
    volatile size_t head; /* читает главный контекст */
    volatile size_t tail; /* пишет прерывание */
} rx;

void SysTick_Handler(void)
{
    tick_ms++;
}

void USART3_IRQHandler(void)
{
    uint32_t isr = USART3->ISR;
    if (isr & USART_ISR_RXNE) {
        uint8_t byte = (uint8_t)USART3->RDR;
        size_t next = (rx.tail + 1) % RX_RING_SIZE;
        if (next != rx.head) {
            rx.data[rx.tail] = byte;
            rx.tail = next;
        }
        /* при переполнении кольца байт теряется: на шаге 0 это допустимо */
    }
    if (isr & USART_ISR_ORE) {
        USART3->ICR = USART_ICR_ORECF;
    }
}

static void gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIODEN;
    (void)RCC->AHB1ENR;

    /* PB0 — LD1: выход push-pull, низкая скорость, выключен */
    GPIOB->BSRR = GPIO_BSRR_BR0;
    GPIOB->MODER = (GPIOB->MODER & ~GPIO_MODER_MODER0) | GPIO_MODER_MODER0_0;

    /* PD8, PD9 — USART3 AF7 */
    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~(0xFu << 0) & ~(0xFu << 4)) | (7u << 0) | (7u << 4);
    GPIOD->OSPEEDR |= GPIO_OSPEEDER_OSPEEDR8_1 | GPIO_OSPEEDER_OSPEEDR9_1;
    GPIOD->MODER = (GPIOD->MODER & ~(GPIO_MODER_MODER8 | GPIO_MODER_MODER9))
                   | GPIO_MODER_MODER8_1 | GPIO_MODER_MODER9_1;
}

static void usart_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;

    USART3->CR1 = 0;
    /* тактирование USART3 по умолчанию — PCLK1 = SystemCoreClock без делителя APB1 */
    USART3->BRR = (SystemCoreClock + CONSOLE_BAUD / 2u) / CONSOLE_BAUD;
    USART3->CR2 = 0;
    USART3->CR3 = 0;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_SetPriority(USART3_IRQn, 6);
    NVIC_EnableIRQ(USART3_IRQn);
}

void hal_init(void)
{
    SystemCoreClockUpdate();
    gpio_init();
    usart_init();
    SysTick_Config(SystemCoreClock / 1000u);
    NVIC_SetPriority(SysTick_IRQn, 0);
}

uint32_t hal_millis(void)
{
    return tick_ms;
}

void hal_led_set(bool on)
{
    GPIOB->BSRR = on ? GPIO_BSRR_BS0 : GPIO_BSRR_BR0;
}

size_t hal_console_read(uint8_t *buf, size_t max)
{
    size_t n = 0;
    while (n < max && rx.head != rx.tail) {
        buf[n++] = rx.data[rx.head];
        rx.head = (rx.head + 1) % RX_RING_SIZE;
    }
    return n;
}

size_t hal_console_write(const uint8_t *buf, size_t len)
{
    size_t n = 0;
    while (n < len && (USART3->ISR & USART_ISR_TXE)) {
        USART3->TDR = buf[n++];
    }
    return n;
}
