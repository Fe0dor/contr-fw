/* Цепочки 74HC595: SR0 на SPI3 (SCK PC10, SER PC12; RCLK PF7, \SRCLR PF6, \OE PC11),
 * SR1 на SPI2 (SCK PB10, SER PB15; RCLK PB1, \SRCLR PB11, \OE PB12).
 *
 * Инициализируется лениво при первом обращении: порядок старта FW-220 идёт из
 * board_early_init() до hal_init(), на HSI 16 МГц. SPI только на передачу, режим 0,
 * MSB первым: первый переданный бит доходит до выхода H самого дальнего регистра.
 */
#include "hal.h"
#include "target.h"

struct chain {
    SPI_TypeDef *spi;
    uint8_t sck_port, sck_pin, ser_port, ser_pin, af;
    uint8_t rclk_port, rclk_pin;
    uint8_t srclr_port, srclr_pin;
    uint8_t oe_port, oe_pin;
    bool ready;
};

static struct chain chains[HAL_SR_CHAINS] = {
    {SPI3, 2, 10, 2, 12, 6, 5, 7, 5, 6, 2, 11, false},
    {SPI2, 1, 10, 1, 15, 5, 1, 1, 1, 11, 1, 12, false},
};

static void chain_init(struct chain *c)
{
    /* управляющие линии: RCLK 0, \SRCLR и \OE — как аппаратная подтяжка, неактивны (1) */
    hal_gpio_config(c->rclk_port, c->rclk_pin, HAL_PIN_OUT, HAL_PULL_NONE, false);
    hal_gpio_config(c->srclr_port, c->srclr_pin, HAL_PIN_OUT, HAL_PULL_NONE, true);
    hal_gpio_config(c->oe_port, c->oe_pin, HAL_PIN_OUT, HAL_PULL_NONE, true);
    target_gpio_af(c->sck_port, c->sck_pin, c->af, false, false);
    target_gpio_af(c->ser_port, c->ser_pin, c->af, false, false);

    if (c->spi == SPI3) {
        RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;
    } else {
        RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    }
    (void)RCC->APB1ENR;
    c->spi->CR1 = 0;
    /* мастер, /16 (1 МГц на HSI, 3,4 МГц на PLL), программный NSS, только передача */
    c->spi->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE
                  | (3u << SPI_CR1_BR_Pos);
    c->spi->CR2 = (7u << SPI_CR2_DS_Pos); /* 8 бит */
    c->spi->CR1 |= SPI_CR1_SPE;
    c->ready = true;
}

static struct chain *get(uint8_t chain)
{
    struct chain *c = &chains[chain < HAL_SR_CHAINS ? chain : 0];
    if (!c->ready) {
        chain_init(c);
    }
    return c;
}

void hal_sr_ctrl(uint8_t chain, enum hal_sr_line line, bool active)
{
    struct chain *c = get(chain);
    if (line == HAL_SR_SRCLR) {
        hal_gpio_write(c->srclr_port, c->srclr_pin, !active);
    } else {
        hal_gpio_write(c->oe_port, c->oe_pin, !active);
    }
}

void hal_sr_shift(uint8_t chain, const uint8_t *image, size_t len)
{
    struct chain *c = get(chain);
    for (size_t i = len; i > 0; i--) {
        while (!(c->spi->SR & SPI_SR_TXE)) {
        }
        *(volatile uint8_t *)&c->spi->DR = image[i - 1];
    }
    while (c->spi->SR & SPI_SR_BSY) {
    }
}

void hal_sr_latch(uint8_t chain)
{
    struct chain *c = get(chain);
    hal_gpio_write(c->rclk_port, c->rclk_pin, true);
    hal_delay_us(1);
    hal_gpio_write(c->rclk_port, c->rclk_pin, false);
}
