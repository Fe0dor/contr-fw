/* I²C секций: A — I2C1 (PB8 SCL, PB9 SDA), B — I2C4 (PF14 SCL, PF15 SDA), AF4, 400 кГц.
 * Тактирование от PCLK1 54 МГц: PRESC 5 → 111 нс; SCLL 12, SCLH 9 тактов → ≈390 кГц;
 * SDADEL 2, SCLDEL 3. Ожидания ограничены 25 мс на операцию (правило А1). */
#include "hal.h"
#include "target.h"

#define I2C_TIMING 0x5032080Bu
#define I2C_OP_TIMEOUT_MS 25u

static I2C_TypeDef *const buses[2] = {I2C1, I2C4};

static void bus_enable(uint8_t bus)
{
    I2C_TypeDef *i = buses[bus];
    i->CR1 = 0;
    i->TIMINGR = I2C_TIMING;
    i->CR1 = I2C_CR1_PE;
}

void hal_i2c_init(void)
{
    target_gpio_af(1, 8, 4, true, false);
    target_gpio_af(1, 9, 4, true, false);
    target_gpio_af(5, 14, 4, true, false);
    target_gpio_af(5, 15, 4, true, false);
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN | RCC_APB1ENR_I2C4EN;
    (void)RCC->APB1ENR;
    bus_enable(0);
    bus_enable(1);
}

void hal_i2c_reset(uint8_t bus)
{
    if (bus > 1) {
        return;
    }
    buses[bus]->CR1 = 0;
    hal_delay_us(10);
    bus_enable(bus);
}

static int finish(I2C_TypeDef *i)
{
    if (!TARGET_WAIT_UNTIL(i->ISR & (I2C_ISR_STOPF | I2C_ISR_NACKF | I2C_ISR_BERR | I2C_ISR_ARLO), I2C_OP_TIMEOUT_MS)) {
        i->CR1 &= ~I2C_CR1_PE;
        i->CR1 |= I2C_CR1_PE;
        return HAL_I2C_TIMEOUT;
    }
    uint32_t isr = i->ISR;
    i->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF;
    i->CR2 = 0;
    if (isr & (I2C_ISR_BERR | I2C_ISR_ARLO)) {
        return HAL_I2C_BUS_ERROR;
    }
    if (isr & I2C_ISR_NACKF) {
        return HAL_I2C_NACK;
    }
    return HAL_I2C_OK;
}

static int xfer(I2C_TypeDef *i, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen)
{
    if (i->ISR & I2C_ISR_BUSY) {
        if (!TARGET_WAIT_UNTIL(!(i->ISR & I2C_ISR_BUSY), I2C_OP_TIMEOUT_MS)) {
            return HAL_I2C_BUS_ERROR;
        }
    }
    if (wlen) {
        i->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)wlen << I2C_CR2_NBYTES_Pos)
                 | (rlen ? 0u : I2C_CR2_AUTOEND) | I2C_CR2_START;
        for (size_t n = 0; n < wlen; n++) {
            if (!TARGET_WAIT_UNTIL(i->ISR & (I2C_ISR_TXIS | I2C_ISR_NACKF | I2C_ISR_BERR), I2C_OP_TIMEOUT_MS)) {
                return finish(i);
            }
            if (i->ISR & (I2C_ISR_NACKF | I2C_ISR_BERR)) {
                return finish(i);
            }
            i->TXDR = w[n];
        }
        if (!rlen) {
            return finish(i);
        }
        if (!TARGET_WAIT_UNTIL(i->ISR & (I2C_ISR_TC | I2C_ISR_NACKF), I2C_OP_TIMEOUT_MS)) {
            return finish(i);
        }
        if (i->ISR & I2C_ISR_NACKF) {
            return finish(i);
        }
    }
    i->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)rlen << I2C_CR2_NBYTES_Pos) | I2C_CR2_RD_WRN
             | I2C_CR2_AUTOEND | I2C_CR2_START;
    for (size_t n = 0; n < rlen; n++) {
        if (!TARGET_WAIT_UNTIL(i->ISR & (I2C_ISR_RXNE | I2C_ISR_NACKF | I2C_ISR_BERR), I2C_OP_TIMEOUT_MS)) {
            return finish(i);
        }
        if (i->ISR & (I2C_ISR_NACKF | I2C_ISR_BERR)) {
            return finish(i);
        }
        r[n] = (uint8_t)i->RXDR;
    }
    return finish(i);
}

int hal_i2c_write(uint8_t bus, uint8_t addr7, const uint8_t *data, size_t len)
{
    return bus > 1 ? HAL_I2C_BUS_ERROR : xfer(buses[bus], addr7, data, len, NULL, 0);
}

int hal_i2c_read(uint8_t bus, uint8_t addr7, uint8_t *data, size_t len)
{
    return bus > 1 ? HAL_I2C_BUS_ERROR : xfer(buses[bus], addr7, NULL, 0, data, len);
}

int hal_i2c_write_read(uint8_t bus, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen)
{
    return bus > 1 ? HAL_I2C_BUS_ERROR : xfer(buses[bus], addr7, w, wlen, r, rlen);
}
