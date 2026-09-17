/* Flash STM32F767ZI в двухбанковом режиме (nDBANK=0): два банка по 1 МБ, в каждом
 * 4×16 КБ, 64 КБ, 7×128 КБ. Сектор CFG — последний сектор (128 КБ) активного банка по
 * адресу 0x080E0000; используется первые HAL_CFG_SIZE байт. Перед переключением банка
 * ядро копирует CFG в неактивный банк (core/update.c), поэтому после подмены BFB2 записи
 * на месте (FW-238).
 *
 * Нумерация секторов при активной подмене (SWP_FB) в RM0410 задана неоднозначно, поэтому
 * стирание сектора проверяется чтением: если по ожидаемому адресу не 0xFF, повторяется с
 * номером другого банка. Оба кандидата — зарезервированные хвосты банков, образ их не
 * занимает (образ ≤ 896 КБ), содержимое CFG на время стирания держится в ОЗУ ядром.
 */
#include "hal.h"
#include "target.h"

#define BANK1_BASE 0x08000000u
#define BANK2_BASE 0x08100000u
#define CFG_OFFSET (HAL_BANK_SIZE - 128u * 1024u) /* 0xE0000 */
#define SECTOR_LAST 11u                             /* последний сектор банка */
#define SNB_BANK2 16u
#ifndef FLASH_OPTCR_BFB2
#define FLASH_OPTCR_BFB2 (1u << 4) /* Dual-bank boot: в заголовке CMSIS F767 бита нет */
#endif

static bool swapped(void)
{
    return (SYSCFG->MEMRMP & SYSCFG_MEMRMP_SWP_FB) != 0;
}

static void unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static void lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static int wait_done(void)
{
    while (FLASH->SR & FLASH_SR_BSY) {
    }
    uint32_t err = FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_ERSERR | FLASH_SR_OPERR);
    if (err) {
        FLASH->SR = err;
        return -1;
    }
    return 0;
}

static bool blank(const uint8_t *p, size_t len)
{
    const uint32_t *w = (const uint32_t *)p;
    for (size_t i = 0; i < len / 4u; i++) {
        if (w[i] != 0xFFFFFFFFu) {
            return false;
        }
    }
    return true;
}

static int erase_snb(uint32_t snb)
{
    unlock();
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_SNB | FLASH_CR_PSIZE)) | FLASH_CR_SER | (snb << FLASH_CR_SNB_Pos)
                | (2u << FLASH_CR_PSIZE_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    int rc = wait_done();
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    lock();
    return rc;
}

/* Стереть сектор по отображённому адресу его начала: номер по отображению,
 * при несовпадении — по физической нумерации (см. заголовок). */
static int erase_sector_at(uint32_t mapped_addr, uint32_t sector, size_t check_len)
{
    bool in_bank2_mapped = mapped_addr >= BANK2_BASE;
    uint32_t snb_mapped = sector + (in_bank2_mapped ? SNB_BANK2 : 0u);
    uint32_t snb_other = sector + (in_bank2_mapped ? 0u : SNB_BANK2);
    if (erase_snb(snb_mapped) != 0) {
        return -1;
    }
    if (blank((const uint8_t *)mapped_addr, check_len)) {
        return 0;
    }
    if (erase_snb(snb_other) != 0) {
        return -1;
    }
    return blank((const uint8_t *)mapped_addr, check_len) ? 0 : -1;
}

static int program(uint32_t addr, const uint8_t *data, size_t len)
{
    if ((addr & 3u) || (len & 3u)) {
        return -1;
    }
    unlock();
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE) | (2u << FLASH_CR_PSIZE_Pos) | FLASH_CR_PG;
    int rc = 0;
    for (size_t i = 0; i < len && rc == 0; i += 4u) {
        uint32_t word = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) | ((uint32_t)data[i + 2] << 16)
                        | ((uint32_t)data[i + 3] << 24);
        *(volatile uint32_t *)(addr + i) = word;
        rc = wait_done();
        if (rc == 0 && *(volatile uint32_t *)(addr + i) != word) {
            rc = -1;
        }
    }
    FLASH->CR &= ~FLASH_CR_PG;
    lock();
    __DSB();
    __ISB();
    return rc;
}

/* ---- CFG ---- */

const uint8_t *hal_cfg_base(void)
{
    return (const uint8_t *)(BANK1_BASE + CFG_OFFSET); /* отображённый активный банк */
}

int hal_cfg_erase(void)
{
    return erase_sector_at(BANK1_BASE + CFG_OFFSET, SECTOR_LAST, HAL_CFG_SIZE);
}

int hal_cfg_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (offset + len > HAL_CFG_SIZE) {
        return -1;
    }
    return program(BANK1_BASE + CFG_OFFSET + offset, data, len);
}

/* ---- второй банк ---- */

const uint8_t *hal_bank_inactive_base(void)
{
    return (const uint8_t *)BANK2_BASE; /* при подмене здесь физический банк 1 */
}

int hal_bank_erase_inactive(uint32_t sector)
{
    /* сектора неактивного банка по отображению: 0…11 → SNB 16…27; при физической
     * нумерации во время подмены это 0…11 — проверка чтением ловит расхождение */
    static const uint32_t sizes[HAL_BANK_SECTORS] = {16u, 16u, 16u, 16u, 64u, 128u, 128u, 128u, 128u, 128u, 128u, 128u};
    if (sector >= HAL_BANK_SECTORS) {
        return -1;
    }
    uint32_t addr = BANK2_BASE;
    for (uint32_t s = 0; s < sector; s++) {
        addr += sizes[s] * 1024u;
    }
    return erase_sector_at(addr, sector, sizes[sector] * 1024u);
}

int hal_bank_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (offset + len > HAL_BANK_SIZE) {
        return -1;
    }
    return program(BANK2_BASE + offset, data, len);
}

uint8_t hal_bank_active(void)
{
    return swapped() ? 2u : 1u;
}

int hal_bank_set_boot(uint8_t bank)
{
    unlock();
    if (FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) {
        FLASH->OPTKEYR = 0x08192A3Bu;
        FLASH->OPTKEYR = 0x4C5D6E7Fu;
    }
    if (bank == 2u) {
        FLASH->OPTCR |= FLASH_OPTCR_BFB2;
    } else {
        FLASH->OPTCR &= ~FLASH_OPTCR_BFB2;
    }
    FLASH->OPTCR |= FLASH_OPTCR_OPTSTRT;
    int rc = wait_done();
    FLASH->OPTCR |= FLASH_OPTCR_OPTLOCK;
    lock();
    return rc;
}

struct hal_optbytes hal_optbytes_read(void)
{
    uint32_t optcr = FLASH->OPTCR;
    struct hal_optbytes ob = {
        .ndbank = (optcr & FLASH_OPTCR_nDBANK) != 0,
        .ndboot = (optcr & FLASH_OPTCR_nDBOOT) != 0,
        .iwdg_sw = (optcr & FLASH_OPTCR_IWDG_SW) != 0,
    };
    return ob;
}
