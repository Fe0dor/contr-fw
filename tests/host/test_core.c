/* Первый тест хоста (план 22, шаг 0): логика мигания и эха над заглушками hal/host. */
#include <stdio.h>
#include <string.h>

#include "blink_echo.h"
#include "hal_host.h"

static int failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

static void test_led_toggles_on_schedule(void)
{
    struct blink_echo s;
    host_reset();
    blink_echo_init(&s, 0);
    CHECK(!host_led_state());

    host_advance_ms(BLINK_PERIOD_MS - 1);
    blink_echo_step(&s);
    CHECK(!host_led_state());

    host_advance_ms(1);
    blink_echo_step(&s);
    CHECK(host_led_state());

    host_advance_ms(BLINK_PERIOD_MS);
    blink_echo_step(&s);
    CHECK(!host_led_state());
    /* init + два переключения */
    CHECK(host_led_writes() == 3);
}

static void test_echo_returns_bytes(void)
{
    struct blink_echo s;
    uint8_t out[64];
    host_reset();
    blink_echo_init(&s, 0);

    host_console_push((const uint8_t *)"*IDN?\n", 6);
    blink_echo_step(&s);
    size_t n = host_console_take(out, sizeof out);
    CHECK(n == 6);
    CHECK(memcmp(out, "*IDN?\n", 6) == 0);

    blink_echo_step(&s);
    CHECK(host_console_take(out, sizeof out) == 0);
}

static void test_echo_survives_partial_write(void)
{
    struct blink_echo s;
    uint8_t out[64];
    host_reset();
    blink_echo_init(&s, 0);
    host_console_set_write_limit(2);

    host_console_push((const uint8_t *)"abcde", 5);
    size_t total = 0;
    for (int i = 0; i < 3; i++) {
        blink_echo_step(&s);
        total += host_console_take(out + total, sizeof out - total);
    }
    CHECK(total == 5);
    CHECK(memcmp(out, "abcde", 5) == 0);
}

int main(void)
{
    test_led_toggles_on_schedule();
    test_echo_returns_bytes();
    test_echo_survives_partial_write();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_core: OK");
    return 0;
}
