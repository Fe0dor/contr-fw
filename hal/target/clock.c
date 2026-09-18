/* Тактирование 216 МГц от HSI через PLL, миллисекундный тик, сброс, причина сброса, UID.
 *
 * HSI вместо HSE: отладчик NUCLEO перепрошит в J-Link OB, и выход MCO 8 МГц от ST-LINK
 * не гарантирован; точности HSI (±1 %) хватает для UART и SPI, тактирование RMII
 * задаёт PHY. HCLK 216, APB1 54, APB2 108 МГц; latency 7 WS, over-drive включён.
 */
#include "hal.h"
#include "target.h"

static volatile uint32_t tick_ms;
static uint32_t reset_cause_cache;
static bool reset_cause_read;

void SysTick_Handler(void)
{
    tick_ms++;
}

static void clock_init(void)
{
    /* ROM dual-boot may leave PLL selected and its own SysTick configuration.
     * Switch to HSI before disabling PLL; PLL cannot stop while it is SYSCLK. */
    SysTick->CTRL = 0;
    /* регулятор напряжения: шкала 1 и over-drive для 216 МГц */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR1 |= PWR_CR1_VOS;

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) {
    }
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) {
    }
    /* HSI 16 / M 8 = 2 МГц; ×N 216 = 432 МГц; /P 2 = 216 МГц; /Q 9 = 48 МГц */
    RCC->PLLCFGR = (8u << RCC_PLLCFGR_PLLM_Pos) | (216u << RCC_PLLCFGR_PLLN_Pos)
                   | (0u << RCC_PLLCFGR_PLLP_Pos) | (9u << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }

    PWR->CR1 |= PWR_CR1_ODEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODRDY)) {
    }
    PWR->CR1 |= PWR_CR1_ODSWEN;
    while (!(PWR->CSR1 & PWR_CSR1_ODSWRDY)) {
    }

    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ARTEN | FLASH_ACR_LATENCY_7WS;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_7WS) {
    }

    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2))
                | RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    SystemCoreClock = SYSCLK_HZ;
}

void hal_clock_init(void)
{
    clock_init();
    SysTick_Config(SYSCLK_HZ / 1000u);
    NVIC_SetPriority(SysTick_IRQn, 0);
}

uint32_t hal_millis(void)
{
    return tick_ms;
}

void hal_delay_us(uint32_t us)
{
    /* цикл ~4 такта на итерацию при 216 МГц */
    volatile uint32_t n = us * (SYSCLK_HZ / 4000000u);
    while (n--) {
    }
}

void hal_reset(void)
{
    NVIC_SystemReset();
    for (;;) {
    }
}

uint32_t hal_reset_cause(void)
{
    if (!reset_cause_read) {
        uint32_t csr = RCC->CSR;
        uint32_t cause = 0;
        if (csr & RCC_CSR_IWDGRSTF) {
            cause |= HAL_RST_IWDG;
        } else if (csr & RCC_CSR_SFTRSTF) {
            cause |= HAL_RST_SW;
        } else if (csr & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF)) {
            cause |= HAL_RST_POR;
        } else if (csr & RCC_CSR_PINRSTF) {
            cause |= HAL_RST_PIN;
        } else if (csr & (RCC_CSR_WWDGRSTF | RCC_CSR_LPWRRSTF)) {
            cause |= HAL_RST_OTHER;
        }
        RCC->CSR |= RCC_CSR_RMVF;
        reset_cause_cache = cause;
        reset_cause_read = true;
    }
    return reset_cause_cache;
}

void hal_uid(uint8_t out[12])
{
    const uint8_t *uid = (const uint8_t *)UID_BASE;
    for (int i = 0; i < 12; i++) {
        out[i] = uid[i];
    }
}
