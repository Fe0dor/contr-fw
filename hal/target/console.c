/* Консоль USART3: PD8 TX / PD9 RX (AF7), 115200 8N1, виртуальный COM-порт отладчика.
 * Приём — в прерывании в кольцо, логики в прерывании нет (А1); передача по TXE без блокировки. */
#include "hal.h"
#include "target.h"

#define CONSOLE_BAUD 115200u
#define RX_RING_SIZE 512u

static struct {
    volatile uint8_t data[RX_RING_SIZE];
    volatile size_t head; /* читает главный контекст */
    volatile size_t tail; /* пишет прерывание */
} rx;

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
    }
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
    }
}

void hal_console_init(void)
{
    target_gpio_af(3, 8, 7, false, false);
    target_gpio_af(3, 9, 7, false, true);
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;
    USART3->CR1 = 0;
    USART3->BRR = (PCLK1_HZ + CONSOLE_BAUD / 2u) / CONSOLE_BAUD;
    USART3->CR2 = 0;
    USART3->CR3 = 0;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
    NVIC_SetPriority(USART3_IRQn, 6);
    NVIC_EnableIRQ(USART3_IRQn);
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
