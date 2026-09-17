#include "signals.h"

#include <ctype.h>
#include <string.h>

#include "config.h"
#include "hal.h"
#include "sr.h"

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int signal_find(const char *name)
{
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (ieq(signal_table[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int relay_find(const char *name)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (ieq(relay_table[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static enum hal_pull pull_of(const struct signal_desc *s)
{
    return s->pull == SIGNAL_PULL_UP ? HAL_PULL_UP : s->pull == SIGNAL_PULL_DOWN ? HAL_PULL_DOWN : HAL_PULL_NONE;
}

void signals_configure_rest(void)
{
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        const struct signal_desc *s = &signal_table[i];
        if (i == SIG_PWRON_A || i == SIG_PWRON_B || i == SIG_CS_PWR || s->dir == SIGNAL_DUT) {
            continue; /* уже выставлены ранними шагами FW-209 */
        }
        if (s->dir == SIGNAL_OUT) {
            hal_gpio_config(s->port, s->pin, HAL_PIN_OUT, HAL_PULL_NONE, s->init != 0);
        } else {
            hal_gpio_config(s->port, s->pin, HAL_PIN_IN, pull_of(s), false);
        }
    }
}

void signal_set(enum signal_id id, bool on)
{
    const struct signal_desc *s = &signal_table[id];
    switch (id) {
    case SIG_PWRON_A:
    case SIG_PWRON_B:
#if PWRON_AB_ON_LEVEL == 1
        /* включено — вывод отпущен во вход (подтяжка платы к 5 В), выключено — выход 0 */
        if (on) {
            hal_gpio_config(s->port, s->pin, HAL_PIN_IN, HAL_PULL_NONE, false);
        } else {
            hal_gpio_config(s->port, s->pin, HAL_PIN_OUT, HAL_PULL_NONE, false);
        }
#else
        hal_gpio_config(s->port, s->pin, HAL_PIN_OUT, HAL_PULL_NONE, on);
#endif
        return;
    case SIG_PWRON_RSP:
        hal_gpio_write(s->port, s->pin, on ? (PWRON_RSP_ON_LEVEL != 0) : (PWRON_RSP_ON_LEVEL == 0));
        return;
    case SIG_CS_PWR:
        hal_gpio_write(s->port, s->pin, on ? (CS_ACTIVE_LOW == 0) : (CS_ACTIVE_LOW != 0));
        return;
    default:
        break;
    }
    if (s->dir == SIGNAL_DUT) {
        if (on) {
            hal_gpio_config(s->port, s->pin, HAL_PIN_OUT, HAL_PULL_NONE, true);
        } else {
            hal_gpio_config(s->port, s->pin, HAL_PIN_IN, HAL_PULL_DOWN, false);
        }
        return;
    }
    hal_gpio_write(s->port, s->pin, on);
}

bool signal_read(enum signal_id id)
{
    const struct signal_desc *s = &signal_table[id];
    return hal_gpio_read(s->port, s->pin);
}

void relay_pin_config_all_off(void)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        const struct relay_desc *r = &relay_table[i];
        if (r->addr.kind == RELAY_ADDR_PIN) {
            hal_gpio_config(r->addr.a, r->addr.b, HAL_PIN_OUT, HAL_PULL_NONE, false);
        }
    }
}

void relay_drive(const struct relay_desc *r, bool on)
{
    if (r->addr.kind == RELAY_ADDR_PIN) {
        hal_gpio_write(r->addr.a, r->addr.b, on);
    } else {
        sr_image_set(r->addr.a, r->addr.b, r->addr.c, on);
    }
}
