/* SPI A на SPI4: SCK PE2, MISO PE5, MOSI PE6 (AF5).
 * PE3 занят V3.6 и не должен перенастраиваться при инициализации SPI.
 * CS_PWR — вывод PE4 (сигнал CS_PWR),
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

/* 3PEAK TPC116S1, Table 5: six don't-care bits, PD1/PD0=00, D15..D0.
 * Mode 1 puts MOSI setup before each falling edge; LOAD keeps mode 0.
 * No service callbacks while CS is asserted (shared-bus transaction).
 */
bool hal_psu_dac_write(uint16_t code)
{
    uint8_t tx[3]={0,(uint8_t)(code>>8),(uint8_t)code};
    uint32_t saved=SPI4->CR1, start=hal_millis();
    bool ok=false;
    SPI4->CR1=saved & ~SPI_CR1_SPE;
    SPI4->CR1=(saved & ~(SPI_CR1_CPOL|SPI_CR1_CPHA)) | SPI_CR1_CPHA;
    hal_gpio_write(4,4,false); /* CS_PWR PE4 */
    hal_delay_us(1);
    for(unsigned i=0;i<3;i++) {
        while(!(SPI4->SR & SPI_SR_TXE)) if((uint32_t)(hal_millis()-start)>=10)goto done;
        *(volatile uint8_t *)&SPI4->DR=tx[i];
        while(!(SPI4->SR & SPI_SR_RXNE)) if((uint32_t)(hal_millis()-start)>=10)goto done;
        (void)*(volatile uint8_t *)&SPI4->DR;
    }
    while(SPI4->SR & SPI_SR_BSY) if((uint32_t)(hal_millis()-start)>=10)goto done;
    ok=true;
done:
    hal_delay_us(1);hal_gpio_write(4,4,true);hal_delay_us(1);
    SPI4->CR1 &= ~SPI_CR1_SPE;
    if(!ok) {RCC->APB2RSTR|=RCC_APB2RSTR_SPI4RST;RCC->APB2RSTR&=~RCC_APB2RSTR_SPI4RST;hal_spia_init();}
    SPI4->CR1=saved;
    return ok;
}
