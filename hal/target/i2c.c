/* I²C секций: A — I2C1 (PB8 SCL, PB9 SDA), B — I2C4 (PF14 SCL, PF15 SDA), AF4, 400 кГц.
 * Тактирование от PCLK1 54 МГц: PRESC 5 → 111 нс; SCLL 12, SCLH 9 тактов → ≈390 кГц;
 * SDADEL 2, SCLDEL 3. Ожидания ограничены 25 мс на операцию (правило А1). */
#include "hal.h"
#include "target.h"

#define I2C_TIMING 0x5032080Bu
#define I2C_OP_TIMEOUT_MS 25u

static I2C_TypeDef *const buses[2] = {I2C1, I2C4};
static uint32_t timing[2] = {I2C_TIMING, I2C_TIMING};
static const uint8_t pins[2][3] = {{1, 8, 9}, {5, 14, 15}}; /* порт, SCL, SDA */

static void bus_enable(uint8_t bus)
{
    I2C_TypeDef *i = buses[bus];
    i->CR1 = 0;
    i->TIMINGR = timing[bus];
    i->CR1 = I2C_CR1_PE;
}

static void bus_pins_af(uint8_t bus)
{
    target_gpio_af(pins[bus][0], pins[bus][1], 4, true, true);
    target_gpio_af(pins[bus][0], pins[bus][2], 4, true, true);
}

/* девять тактов SCL выходом с открытым стоком: ведомый, застрявший посреди байта, отпускает SDA */
static void bus_unstick(uint8_t bus)
{
    GPIO_TypeDef *g = target_port(pins[bus][0]);
    uint8_t scl = pins[bus][1];
    uint32_t shift = 2u * scl;
    g->OTYPER |= (1u << scl);
    g->BSRR = (1u << scl);
    g->MODER = (g->MODER & ~(3u << shift)) | (1u << shift); /* выход */
    for (int k = 0; k < 9; k++) {
        g->BSRR = (1u << (scl + 16u));
        hal_delay_us(5);
        g->BSRR = (1u << scl);
        hal_delay_us(5);
    }
    bus_pins_af(bus);
}

void hal_i2c_init(void)
{
    bus_pins_af(0);
    bus_pins_af(1);
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
    if (!hal_gpio_read(pins[bus][0], pins[bus][2])) {
        bus_unstick(bus);
    }
    bus_enable(bus);
}

uint32_t hal_i2c_status(uint8_t bus, bool *scl, bool *sda)
{
    if (bus > 1) {
        return 0;
    }
    *scl = hal_gpio_read(pins[bus][0], pins[bus][1]);
    *sda = hal_gpio_read(pins[bus][0], pins[bus][2]);
    return buses[bus]->ISR;
}

void hal_i2c_set_timing(uint8_t bus, uint32_t timingr)
{
    if (bus > 1) {
        return;
    }
    timing[bus] = timingr;
    bus_enable(bus);
}

#define I2C_WAIT(cond) \
    ({ bool ok=true; while(!(cond)) { \
        if((uint32_t)(hal_millis()-started)>=I2C_OP_TIMEOUT_MS){ok=false;break;} \
    } ok; })

static int finish(I2C_TypeDef *i, uint32_t started)
{
    /* Ждать именно STOPF: NACKF выставляется раньше, чем STOP ушёл на шину; запись в CR2
     * в этот момент снимает AUTOEND, и контроллер повисает с TC и SCL в нуле. */
    if (!I2C_WAIT(i->ISR & (I2C_ISR_STOPF | I2C_ISR_BERR | I2C_ISR_ARLO))) {
        i->CR1 &= ~I2C_CR1_PE;
        (void)i->CR1;
        hal_delay_us(2); /* PE в нуле не меньше трёх тактов APB */
        i->CR1 |= I2C_CR1_PE;
        return HAL_I2C_TIMEOUT;
    }
    uint32_t isr = i->ISR;
    i->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF | I2C_ICR_BERRCF | I2C_ICR_ARLOCF;
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
    uint32_t started=hal_millis();
    if (i->ISR & I2C_ISR_BUSY) {
        if (!I2C_WAIT(!(i->ISR & I2C_ISR_BUSY))) {
            return HAL_I2C_BUS_ERROR;
        }
    }
    if (wlen || !rlen) {
        i->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)wlen << I2C_CR2_NBYTES_Pos)
                 | (rlen ? 0u : I2C_CR2_AUTOEND) | I2C_CR2_START;
        for (size_t n = 0; n < wlen; n++) {
            if (!I2C_WAIT(i->ISR & (I2C_ISR_TXIS | I2C_ISR_NACKF | I2C_ISR_BERR))) {
                return finish(i, started);
            }
            if (i->ISR & (I2C_ISR_NACKF | I2C_ISR_BERR)) {
                return finish(i, started);
            }
            i->TXDR = w[n];
        }
        if (!rlen) {
            return finish(i, started);
        }
        if (!I2C_WAIT(i->ISR & (I2C_ISR_TC | I2C_ISR_NACKF))) {
            return finish(i, started);
        }
        if (i->ISR & I2C_ISR_NACKF) {
            return finish(i, started);
        }
    }
    i->CR2 = ((uint32_t)addr7 << 1) | ((uint32_t)rlen << I2C_CR2_NBYTES_Pos) | I2C_CR2_RD_WRN
             | I2C_CR2_AUTOEND | I2C_CR2_START;
    for (size_t n = 0; n < rlen; n++) {
        if (!I2C_WAIT(i->ISR & (I2C_ISR_RXNE | I2C_ISR_NACKF | I2C_ISR_BERR))) {
            return finish(i, started);
        }
        if (i->ISR & (I2C_ISR_NACKF | I2C_ISR_BERR)) {
            return finish(i, started);
        }
        r[n] = (uint8_t)i->RXDR;
    }
    return finish(i, started);
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
