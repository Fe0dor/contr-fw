/* IWDG от LSI 32 кГц. При IWDG_SW=0 стартует железом с периодом ≈0,5 с; здесь период
 * перепрограммируется (FW-212), при IWDG_SW=1 — запускается программно. */
#include "hal.h"
#include "target.h"

void hal_wdt_start(uint32_t period_ms)
{
    if (period_ms > 4095u) {
        period_ms = 4095u;
    }
    IWDG->KR = 0xCCCCu; /* старт (без эффекта, если уже запущен опционным байтом) */
    IWDG->KR = 0x5555u; /* доступ к PR/RLR */
    IWDG->PR = 3u;      /* /32 → 1 кГц */
    IWDG->RLR = period_ms;
    while (IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) {
    }
    IWDG->KR = 0xAAAAu;
}

void hal_wdt_kick(void)
{
    IWDG->KR = 0xAAAAu;
}
