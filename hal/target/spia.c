/* SPI A на SPI4: SCK PE2, MISO PE5, MOSI PE6 (AF5). CS_PWR — вывод PE4 (сигнал CS_PWR),
 * CS_LOAD — бит SR0.2.B; оба активирует вызывающий. Режим 0, /32 = 3,4 МГц. */
#include "hal.h"
#include "target.h"

void hal_spia_init(void)
{
    target_gpio_af(4, 2, 5, false, false);
    target_gpio_af(4, 5, 5, false, false);
    target_gpio_af(4, 6, 5, false, false);
    RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;
    (void)RCC->APB2ENR;
    SPI4->CR1 = 0;
    SPI4->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (4u << SPI_CR1_BR_Pos);
    SPI4->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH; /* 8 бит, порог FIFO 8 бит */
    SPI4->CR1 |= SPI_CR1_SPE;
}

void hal_spia_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while (!(SPI4->SR & SPI_SR_TXE)) {
        }
        *(volatile uint8_t *)&SPI4->DR = tx ? tx[i] : 0xFFu;
        while (!(SPI4->SR & SPI_SR_RXNE)) {
        }
        uint8_t byte = *(volatile uint8_t *)&SPI4->DR;
        if (rx) {
            rx[i] = byte;
        }
    }
    while (SPI4->SR & SPI_SR_BSY) {
    }
}
