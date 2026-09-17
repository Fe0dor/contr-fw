/* Заглушки железа для хост-сборки: виртуальное время и кольца консоли (А2). */
#include "hal_host.h"

#include <string.h>

#include "hal.h"

#define RING_SIZE 1024u

struct ring {
    uint8_t data[RING_SIZE];
    size_t head;
    size_t len;
};

static struct {
    uint32_t now_ms;
    bool led_on;
    unsigned led_writes;
    struct ring in;   /* терминал → прошивка */
    struct ring out;  /* прошивка → терминал */
    size_t write_limit;
} h;

static size_t ring_push(struct ring *r, const uint8_t *buf, size_t len)
{
    size_t n = 0;
    while (n < len && r->len < RING_SIZE) {
        r->data[(r->head + r->len) % RING_SIZE] = buf[n++];
        r->len++;
    }
    return n;
}

static size_t ring_pop(struct ring *r, uint8_t *buf, size_t max)
{
    size_t n = 0;
    while (n < max && r->len > 0) {
        buf[n++] = r->data[r->head];
        r->head = (r->head + 1) % RING_SIZE;
        r->len--;
    }
    return n;
}

void host_reset(void)
{
    memset(&h, 0, sizeof h);
}

void host_advance_ms(uint32_t ms)
{
    h.now_ms += ms;
}

void host_console_push(const uint8_t *buf, size_t len)
{
    ring_push(&h.in, buf, len);
}

size_t host_console_take(uint8_t *buf, size_t max)
{
    return ring_pop(&h.out, buf, max);
}

void host_console_set_write_limit(size_t limit)
{
    h.write_limit = limit;
}

bool host_led_state(void)
{
    return h.led_on;
}

unsigned host_led_writes(void)
{
    return h.led_writes;
}

/* ---- реализация hal.h ---- */

void hal_init(void)
{
}

uint32_t hal_millis(void)
{
    return h.now_ms;
}

void hal_led_set(bool on)
{
    h.led_on = on;
    h.led_writes++;
}

size_t hal_console_read(uint8_t *buf, size_t max)
{
    return ring_pop(&h.in, buf, max);
}

size_t hal_console_write(const uint8_t *buf, size_t len)
{
    if (h.write_limit && len > h.write_limit) {
        len = h.write_limit;
    }
    return ring_push(&h.out, buf, len);
}
