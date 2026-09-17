#include "blink_echo.h"

#include <string.h>

#include "hal.h"

void blink_echo_init(struct blink_echo *s, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->next_toggle_ms = now_ms + BLINK_PERIOD_MS;
    hal_led_set(false);
}

static void blink(struct blink_echo *s)
{
    uint32_t now = hal_millis();
    /* сравнение через разность: корректно при переполнении счётчика */
    if ((int32_t)(now - s->next_toggle_ms) >= 0) {
        s->led_on = !s->led_on;
        hal_led_set(s->led_on);
        s->next_toggle_ms += BLINK_PERIOD_MS;
    }
}

static void echo(struct blink_echo *s)
{
    if (s->pending_len == 0) {
        s->pending_len = hal_console_read(s->pending, sizeof s->pending);
    }
    if (s->pending_len == 0) {
        return;
    }
    size_t sent = hal_console_write(s->pending, s->pending_len);
    if (sent < s->pending_len) {
        memmove(s->pending, s->pending + sent, s->pending_len - sent);
    }
    s->pending_len -= sent;
}

void blink_echo_step(struct blink_echo *s)
{
    blink(s);
    echo(s);
}
