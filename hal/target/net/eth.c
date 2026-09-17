/* Драйвер Ethernet MAC STM32F7 в RMII с PHY LAN8742A (NUCLEO-F767ZI, адрес PHY 0).
 *
 * Без прерываний: дескрипторы опрашиваются из hal_net_poll() (А1). D-cache выключен,
 * дескрипторы и буферы лежат в обычном SRAM (А14). Выводы RMII: PA1 REF_CLK, PA2 MDIO,
 * PA7 CRS_DV, PB13 TXD1, PC1 MDC, PC4 RXD0, PC5 RXD1, PG11 TX_EN, PG13 TXD0 — AF11.
 */
#include "eth.h"

#include <string.h>

#include "hal.h"
#include "target.h"

#define RX_DESC_N 4u
#define TX_DESC_N 4u
#define BUF_SIZE 1536u

#define PHY_ADDR 0u
#define PHY_BCR 0u
#define PHY_BSR 1u
#define PHY_SCSR 31u /* LAN8742: PHY Special Control/Status */
#define PHY_BCR_RESET 0x8000u
#define PHY_BCR_ANEG 0x1000u
#define PHY_BCR_ANEG_RESTART 0x0200u
#define PHY_BSR_LINK 0x0004u
#define PHY_BSR_ANEG_DONE 0x0020u

/* нормальные дескрипторы DMA (без расширенных полей) */
struct desc {
    volatile uint32_t status;
    volatile uint32_t ctrl;
    volatile uint32_t buf;
    volatile uint32_t next;
};

#define DES0_OWN (1u << 31)
#define TDES0_IC (1u << 30)
#define TDES0_LS (1u << 29)
#define TDES0_FS (1u << 28)
#define TDES0_TCH (1u << 20)
#define RDES0_FL_Pos 16u
#define RDES0_FL_Msk (0x3FFFu << RDES0_FL_Pos)
#define RDES0_ES (1u << 15)
#define RDES0_LS (1u << 8)
#define RDES0_FS (1u << 9)
#define RDES1_RCH (1u << 14)

static struct desc rx_desc[RX_DESC_N] __attribute__((aligned(4)));
static struct desc tx_desc[TX_DESC_N] __attribute__((aligned(4)));
static uint8_t rx_buf[RX_DESC_N][BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buf[TX_DESC_N][BUF_SIZE] __attribute__((aligned(4)));
static uint32_t rx_idx, tx_idx;
static bool link_up, link_100, link_full;

static bool phy_read(uint32_t reg, uint16_t *val)
{
    ETH->MACMIIAR = (ETH->MACMIIAR & ETH_MACMIIAR_CR) | (PHY_ADDR << 11) | (reg << 6) | ETH_MACMIIAR_MB;
    if (!TARGET_WAIT_UNTIL(!(ETH->MACMIIAR & ETH_MACMIIAR_MB), 20u)) {
        return false;
    }
    *val = (uint16_t)ETH->MACMIIDR;
    return true;
}

static bool phy_write(uint32_t reg, uint16_t val)
{
    ETH->MACMIIDR = val;
    ETH->MACMIIAR = (ETH->MACMIIAR & ETH_MACMIIAR_CR) | (PHY_ADDR << 11) | (reg << 6) | ETH_MACMIIAR_MW | ETH_MACMIIAR_MB;
    return TARGET_WAIT_UNTIL(!(ETH->MACMIIAR & ETH_MACMIIAR_MB), 20u);
}

static void pins_init(void)
{
    static const uint8_t pins[][2] = {{0, 1}, {0, 2}, {0, 7}, {1, 13}, {2, 1}, {2, 4}, {2, 5}, {6, 11}, {6, 13}};
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        target_gpio_af(pins[i][0], pins[i][1], 11, false, false);
    }
}

static void desc_init(void)
{
    for (uint32_t i = 0; i < RX_DESC_N; i++) {
        rx_desc[i].status = DES0_OWN;
        rx_desc[i].ctrl = RDES1_RCH | BUF_SIZE;
        rx_desc[i].buf = (uint32_t)rx_buf[i];
        rx_desc[i].next = (uint32_t)&rx_desc[(i + 1) % RX_DESC_N];
    }
    for (uint32_t i = 0; i < TX_DESC_N; i++) {
        tx_desc[i].status = TDES0_TCH;
        tx_desc[i].ctrl = 0;
        tx_desc[i].buf = (uint32_t)tx_buf[i];
        tx_desc[i].next = (uint32_t)&tx_desc[(i + 1) % TX_DESC_N];
    }
    rx_idx = tx_idx = 0;
    ETH->DMARDLAR = (uint32_t)rx_desc;
    ETH->DMATDLAR = (uint32_t)tx_desc;
}

bool eth_init(const uint8_t mac[6])
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL; /* RMII до включения тактирования MAC */
    pins_init();
    RCC->AHB1ENR |= RCC_AHB1ENR_ETHMACEN | RCC_AHB1ENR_ETHMACTXEN | RCC_AHB1ENR_ETHMACRXEN;
    (void)RCC->AHB1ENR;
    RCC->AHB1RSTR |= RCC_AHB1RSTR_ETHMACRST;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETHMACRST;

    ETH->DMABMR |= ETH_DMABMR_SR;
    if (!TARGET_WAIT_UNTIL(!(ETH->DMABMR & ETH_DMABMR_SR), 100u)) {
        return false; /* без тактирования REF_CLK от PHY сброс DMA не завершается */
    }
    /* MDC = HCLK/102 при 150…216 МГц */
    ETH->MACMIIAR = ETH_MACMIIAR_CR_Div102;

    phy_write(PHY_BCR, PHY_BCR_RESET);
    uint16_t bcr = PHY_BCR_RESET;
    uint32_t t0 = hal_millis();
    while (phy_read(PHY_BCR, &bcr) && (bcr & PHY_BCR_RESET) && (uint32_t)(hal_millis() - t0) < 50u) {
    }
    phy_write(PHY_BCR, PHY_BCR_ANEG | PHY_BCR_ANEG_RESTART);

    ETH->MACA0HR = ((uint32_t)mac[5] << 8) | mac[4];
    ETH->MACA0LR = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[1] << 8) | mac[0];
    ETH->MACFFR = 0; /* свой адрес и широковещание */
    ETH->MACCR = ETH_MACCR_FES | ETH_MACCR_DM; /* до автосогласования: 100 полный дуплекс */
    ETH->DMABMR = ETH_DMABMR_AAB | ETH_DMABMR_USP | (32u << 17) | (32u << 8);
    ETH->DMAOMR = ETH_DMAOMR_RSF | ETH_DMAOMR_TSF;
    desc_init();

    ETH->MACCR |= ETH_MACCR_TE | ETH_MACCR_RE;
    ETH->DMAOMR |= ETH_DMAOMR_FTF;
    ETH->DMAOMR |= ETH_DMAOMR_ST | ETH_DMAOMR_SR;
    return true;
}

bool eth_poll_link(void)
{
    uint16_t bsr = 0;
    if (!phy_read(PHY_BSR, &bsr)) {
        return link_up;
    }
    bool up = (bsr & PHY_BSR_LINK) != 0;
    if (up && !link_up) {
        uint16_t scsr = 0;
        if (phy_read(PHY_SCSR, &scsr)) {
            /* биты 4:2 — 001 10 half, 101 10 full, 010 100 half, 110 100 full */
            uint32_t speed = (scsr >> 2) & 7u;
            link_100 = (speed & 2u) != 0;
            link_full = (speed & 4u) != 0;
            uint32_t maccr = ETH->MACCR & ~(ETH_MACCR_FES | ETH_MACCR_DM);
            if (link_100) {
                maccr |= ETH_MACCR_FES;
            }
            if (link_full) {
                maccr |= ETH_MACCR_DM;
            }
            ETH->MACCR = maccr;
        }
    }
    link_up = up;
    return link_up;
}

bool eth_link_up(void)
{
    return link_up;
}

bool eth_send(const uint8_t *frame, size_t len)
{
    struct desc *d = &tx_desc[tx_idx];
    if (d->status & DES0_OWN) {
        return false; /* все дескрипторы заняты */
    }
    if (len > BUF_SIZE) {
        return false;
    }
    memcpy(tx_buf[tx_idx], frame, len);
    d->ctrl = (uint32_t)len;
    d->status = DES0_OWN | TDES0_TCH | TDES0_FS | TDES0_LS;
    __DSB();
    ETH->DMASR = ETH_DMASR_TBUS;
    ETH->DMATPDR = 0; /* разбудить передачу */
    tx_idx = (tx_idx + 1) % TX_DESC_N;
    return true;
}

size_t eth_receive(const uint8_t **frame)
{
    struct desc *d = &rx_desc[rx_idx];
    if (d->status & DES0_OWN) {
        return 0;
    }
    uint32_t status = d->status;
    size_t len = 0;
    if (!(status & RDES0_ES) && (status & RDES0_FS) && (status & RDES0_LS)) {
        len = (status & RDES0_FL_Msk) >> RDES0_FL_Pos;
        if (len >= 4u) {
            len -= 4u; /* без CRC */
        }
        *frame = rx_buf[rx_idx];
    }
    if (len == 0) {
        eth_receive_done();
    }
    return len;
}

void eth_receive_done(void)
{
    struct desc *d = &rx_desc[rx_idx];
    d->status = DES0_OWN;
    __DSB();
    rx_idx = (rx_idx + 1) % RX_DESC_N;
    if (ETH->DMASR & ETH_DMASR_RBUS) {
        ETH->DMASR = ETH_DMASR_RBUS;
        ETH->DMARPDR = 0;
    }
}
