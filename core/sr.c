#include "sr.h"

#include <string.h>

#include "config.h"
#include "hal.h"
#include "state.h"
#include "signals.h"

void sr_image_initial(uint8_t image[2][4])
{
    memset(image, 0, 2 * 4);
#if CS_LOAD_ON_SR && CS_ACTIVE_LOW
    image[0][SR0_CS_LOAD_REG] |= (uint8_t)(1u << SR0_CS_LOAD_BIT);
#endif
}

void sr_write(uint8_t chain)
{
    hal_sr_shift(chain, desired.sr_image[chain], (size_t)SR_REGS(chain));
    hal_sr_latch(chain);
}

void sr_start(void)
{
    sr_image_initial(desired.sr_image);
    for (uint8_t chain = 0; chain < SR_CHAINS; chain++) {
        hal_sr_ctrl(chain, HAL_SR_SRCLR, true);   /* 1. очистка */
        hal_sr_latch(chain);                       /* 2. очищенный регистр в выходной */
        hal_sr_ctrl(chain, HAL_SR_SRCLR, false);  /* 3. снять очистку */
        hal_sr_shift(chain, desired.sr_image[chain], (size_t)SR_REGS(chain)); /* 4. начальный образ */
        hal_sr_latch(chain);                       /* 5. защёлка */
        hal_sr_ctrl(chain, HAL_SR_OE, true);       /* 6. разрешить выходы */
    }
}

void sr_image_set(uint8_t chain, uint8_t reg, uint8_t bit, bool value)
{
    if (value) {
        desired.sr_image[chain][reg] |= (uint8_t)(1u << bit);
    } else {
        desired.sr_image[chain][reg] &= (uint8_t)~(1u << bit);
    }
}

bool sr_image_get(uint8_t chain, uint8_t reg, uint8_t bit)
{
    return (desired.sr_image[chain][reg] >> bit) & 1u;
}

bool sr_test_loop(void)
{
    bool saved = sr_image_get(0, SR0_TEST_OUT_REG, SR0_TEST_OUT_BIT);
    bool ok = true;
    for (unsigned level = 0; level < 2; ++level) {
        sr_image_set(0, SR0_TEST_OUT_REG, SR0_TEST_OUT_BIT, level != 0);
        sr_write(0);
        hal_delay_us(10);
        if (signal_read(SIG_SR_TEST_IN) != (level != 0)) ok = false;
    }
    sr_image_set(0, SR0_TEST_OUT_REG, SR0_TEST_OUT_BIT, saved);
    sr_write(0);
    return ok;
}
