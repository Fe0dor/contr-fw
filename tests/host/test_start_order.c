/* FW-209 порядок при подаче питания, FW-220 старт цепочек, FW-123 начальный образ. */
#include "check.h"
#include "signal_table.h"

static int idx_after(const char *prefix, int from)
{
    return host_journal_find(prefix, from < 0 ? 0 : (size_t)from);
}

static void test_fw209_order(void)
{
    host_reset_all();
    board_early_init();
    /* 1. PWRON_A/B выключено: выход 0 */
    int a = idx_after("gpio_config PC7 out none 0", 0);
    int b = idx_after("gpio_config PD15 out none 0", 0);
    CHECK(a >= 0 && b >= 0);
    /* 2. CS_PWR неактивен (1) после PWRON */
    int cs = idx_after("gpio_config PE4 out none 1", 0);
    CHECK(cs > a && cs > b);
    /* 3. JTAG на PB4 */
    int jt = idx_after("jtag_release", 0);
    CHECK(jt > cs);
    /* 4. C* входами с подтяжкой вниз */
    int c1 = idx_after("gpio_config PC8 in down", 0);
    int c19 = idx_after("gpio_config PB4 in down", (int)(size_t)jt + 1);
    CHECK(c1 > jt && c19 > jt);
    /* 5. цепочки после линий */
    int sr = idx_after("sr_ctrl 0 srclr 1", 0);
    CHECK(sr > c1);
    /* до разрешения выходов ни одной записи в линии реле на выводах с единицей */
    for (size_t i = 0; i < host_journal_count(); i++) {
        const char *e = host_journal_at(i);
        if (strncmp(e, "gpio_write", 10) == 0) {
            CHECK(e[strlen(e) - 1] == '0');
        }
    }
}

static void test_fw220_chain_start(void)
{
    host_reset_all();
    board_early_init();
    for (unsigned chain = 0; chain < 2; chain++) {
        char p[32];
        int i0, i1, i2, i3, i4, i5;
        snprintf(p, sizeof p, "sr_ctrl %u srclr 1", chain);
        i0 = idx_after(p, 0);
        snprintf(p, sizeof p, "sr_latch %u", chain);
        i1 = idx_after(p, i0);
        snprintf(p, sizeof p, "sr_ctrl %u srclr 0", chain);
        i2 = idx_after(p, i1);
        snprintf(p, sizeof p, "sr_shift %u", chain);
        i3 = idx_after(p, i2);
        snprintf(p, sizeof p, "sr_latch %u", chain);
        i4 = idx_after(p, i3);
        snprintf(p, sizeof p, "sr_ctrl %u oe 1", chain);
        i5 = idx_after(p, i4);
        CHECK(i0 >= 0 && i1 > i0 && i2 > i1 && i3 > i2 && i4 > i3 && i5 > i4);
        /* до OE не было других записей OE */
        snprintf(p, sizeof p, "sr_ctrl %u oe", chain);
        CHECK(idx_after(p, 0) == i5);
        CHECK(host_sr_oe_active((uint8_t)chain));
    }
    /* FW-123: первый образ — нули на реле, CS_LOAD (SR0.2.B) неактивен = 1, SR_TEST_OUT = 0 */
    const uint8_t *sr0 = host_sr_outputs(0);
    CHECK(sr0[0] == 0 && sr0[1] == 0 && sr0[2] == 0x02);
    const uint8_t *sr1 = host_sr_outputs(1);
    CHECK(sr1[0] == 0 && sr1[1] == 0 && sr1[2] == 0 && sr1[3] == 0);
}

static void test_pwron_semantics(void)
{
    host_reset_all();
    board_early_init();
    /* PWRON_A выключено = выход 0; включено = вход без подтяжки (плата тянет к 5 В) */
    struct host_pin *p = host_pin(2, 7);
    CHECK(p->mode == HAL_PIN_OUT && !p->level);
    /* PWRON_RSP по умолчанию 1 (выключен), CS_PWR 1 */
    CHECK(host_pin(6, 15)->level == true);
    CHECK(host_pin(4, 4)->level == true);
    /* выводы реле — выходы 0 */
    CHECK(host_pin(6, 8)->mode == HAL_PIN_OUT && !host_pin(6, 8)->level); /* M4 PG8 */
}

int main(void)
{
    test_fw209_order();
    test_fw220_chain_start();
    test_pwron_semantics();
    return test_result("test_start_order");
}
