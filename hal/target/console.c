/* Консоль USART3: PD8 TX / PD9 RX (AF7), 115200 8N1, виртуальный COM-порт отладчика.
 * Приём — в прерывании в кольцо, логики в прерывании нет (А1); передача по TXE без блокировки. */
#include "hal.h"
#include "target.h"
#include "cmd.h" /* shared maximum accepted line length; no core calls in ISR */

#define CONSOLE_BAUD 115200u
#define RX_RING_SIZE 512u

static struct {
    volatile uint8_t data[RX_RING_SIZE];
    volatile size_t head; /* читает главный контекст */
    volatile size_t tail; /* пишет прерывание */
    bool safe_end[RX_RING_SIZE]; /* completed, valid SAFE at this queued LF */
    uint16_t queued_safe;
    volatile uint16_t recovered_safe;
    uint8_t recovery_pos;
    uint8_t safe_state; /* 0 leading whitespace, 1..4 SAFE prefix, 255 invalid */
    uint16_t line_len;
    bool discard_line; /* only ISR writes; discard through LF after lost/corrupt data */
} rx;

/* A tiny receive-stream recognizer survives ring discard. It never executes
 * commands, allocates, or calls core. Corrupt/overlong lines cannot become SAFE. */
static bool safe_byte(uint8_t byte)
{
    if (byte == '\n') {
        bool complete = rx.safe_state == 4 && rx.line_len <= CMD_LINE_MAX;
        rx.safe_state = 0; rx.line_len = 0;
        return complete;
    }
    if (byte == '\r') return false; /* core ignores CR anywhere in a line */
    if (rx.line_len < CMD_LINE_MAX + 1) rx.line_len++;
    if (rx.line_len > CMD_LINE_MAX) rx.safe_state = 255;
    bool space = byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f';
    if ((rx.safe_state == 0 || rx.safe_state == 4) && space) return false;
    if (byte >= 'a' && byte <= 'z') byte -= 'a' - 'A';
    if (rx.safe_state < 4 && byte == (uint8_t)"SAFE"[rx.safe_state]) rx.safe_state++;
    else rx.safe_state = 255;
    return false;
}
static void recover_safe(unsigned count)
{
    unsigned room = UINT16_MAX - rx.recovered_safe;
    rx.recovered_safe += (uint16_t)(count < room ? count : room);
}
/* Preserve completed SAFE lines before invalidating discarded ordinary input.
 * The consumer masks interrupts, so metadata and indices change atomically. */
static void rx_reject(void)
{
    recover_safe(rx.queued_safe);
    rx.queued_safe = 0;
    rx.head = rx.tail;
    rx.data[rx.tail] = 0;
    rx.safe_end[rx.tail] = false;
    rx.tail = (rx.tail + 1) % RX_RING_SIZE;
    rx.discard_line = true;
}

void USART3_IRQHandler(void)
{
    uint32_t isr = USART3->ISR;
    bool received = (isr & USART_ISR_RXNE) != 0;
    uint8_t byte = received ? (uint8_t)USART3->RDR : 0;
    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF;
        rx_reject();
        rx.safe_state = 255; /* even a corrupt LF is not a trusted boundary */
        return;
    }
    if (!received) return;
    bool safe = safe_byte(byte);
    size_t next = (rx.tail + 1) % RX_RING_SIZE;
    if (!rx.discard_line && next == rx.head) {
        rx_reject();
        next = (rx.tail + 1) % RX_RING_SIZE;
    }
    if (rx.discard_line) {
        if (byte != '\n') return;
        if (safe) recover_safe(1); /* its prefix was discarded */
        safe = false;
        rx.discard_line = false;
    }
    rx.data[rx.tail] = byte;
    rx.safe_end[rx.tail] = safe;
    if (safe) rx.queued_safe++;
    rx.tail = next;
}

void hal_console_init(void)
{
    target_gpio_af(3, 8, 7, false, false);
    target_gpio_af(3, 9, 7, false, true);
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;
    USART3->CR1 = 0;
    rx.head = rx.tail = 0;
    rx.discard_line = false;
    rx.queued_safe = rx.recovered_safe = rx.line_len = 0;
    rx.recovery_pos = rx.safe_state = 0;
    USART3->BRR = (PCLK1_HZ + CONSOLE_BAUD / 2u) / CONSOLE_BAUD;
    USART3->CR2 = 0;
    USART3->CR3 = 0;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;
    NVIC_SetPriority(USART3_IRQn, 6);
    NVIC_EnableIRQ(USART3_IRQn);
}

size_t hal_console_read(uint8_t *buf, size_t max)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    size_t n = 0;
    /* NUL+LF first poisons any prefix already delivered to core. Recovery is
     * resumable even for max=1, and each saved SAFE is delivered exactly once. */
    static const uint8_t recovered[] = {0, '\n', 'S', 'A', 'F', 'E', '\n'};
    while (n < max && rx.recovered_safe) {
        buf[n++] = recovered[rx.recovery_pos++];
        if (rx.recovery_pos == sizeof recovered) {
            rx.recovery_pos = 0;
            rx.recovered_safe--;
        }
    }
    while (n < max && rx.head != rx.tail) {
        if (rx.safe_end[rx.head]) rx.queued_safe--;
        buf[n++] = rx.data[rx.head];
        rx.head = (rx.head + 1) % RX_RING_SIZE;
    }
    __set_PRIMASK(primask);
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
