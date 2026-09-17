/* Интерфейс слоя железа (архитектура А2 ТЗ 19).
 *
 * Две реализации: hal/target — STM32F767ZI на плате CONTR, hal/host — заглушки
 * с журналом вызовов и виртуальным временем для тестов на хосте (FW-246).
 * Правило А1: ни одного блокирующего вызова дольше 50 мс, кроме операций с flash.
 * Логики здесь нет: адреса выводов приходят из gen/relay_table и gen/signal_table,
 * порядок операций задаёт core/.
 */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- старт и время */

/* Тактирование, тик, консоль, SPI, I²C. Вызывается после board_early_init() ядра. */
void hal_init(void);

/* Миллисекунды с момента старта; переполняется через 49 суток. */
uint32_t hal_millis(void);

/* Короткая активная пауза для импульсов RCLK, пауз CS и т. п. */
void hal_delay_us(uint32_t us);

/* Программный сброс микроконтроллера; не возвращается. */
void hal_reset(void);

/* Причина последнего сброса — битовая маска HAL_RST_*; флаги сбрасываются после чтения. */
#define HAL_RST_POR   0x01u  /* подача питания или BOR */
#define HAL_RST_PIN   0x02u  /* кнопка или вывод NRST */
#define HAL_RST_IWDG  0x04u  /* watchdog */
#define HAL_RST_SW    0x08u  /* hal_reset() */
#define HAL_RST_OTHER 0x10u
uint32_t hal_reset_cause(void);

/* 96-битный UID микроконтроллера. */
void hal_uid(uint8_t out[12]);

/* ---------------------------------------------------------------- GPIO */

enum hal_pin_mode { HAL_PIN_IN = 0, HAL_PIN_OUT = 1 };
enum hal_pull { HAL_PULL_NONE = 0, HAL_PULL_UP = 1, HAL_PULL_DOWN = 2 };

/* Порт 0 = A. Для выхода level — начальный уровень, выставляется до переключения в выход. */
void hal_gpio_config(uint8_t port, uint8_t pin, enum hal_pin_mode mode, enum hal_pull pull, bool level);
void hal_gpio_write(uint8_t port, uint8_t pin, bool level);
bool hal_gpio_read(uint8_t port, uint8_t pin);

/* Отключить JTAG на PB4 (NJTRST): SWD остаётся (FW-209 п. 3). */
void hal_jtag_release_pb4(void);

/* ---------------------------------------------------------------- цепочки 74HC595 */

#define HAL_SR_CHAINS 2u
#define HAL_SR0_REGS 3u
#define HAL_SR1_REGS 4u

enum hal_sr_line { HAL_SR_SRCLR = 0, HAL_SR_OE = 1 };

/* active = true — линия в активном (низком) уровне. */
void hal_sr_ctrl(uint8_t chain, enum hal_sr_line line, bool active);

/* Вдвинуть образ: image[0] — регистр 0 (ближайший к MCU), бит 0 — выход A.
 * Последним в цепочку уходит регистр 0, поэтому байты передаются от старшего регистра. */
void hal_sr_shift(uint8_t chain, const uint8_t *image, size_t len);

/* Импульс RCLK: сдвиговый регистр защёлкивается в выходной. */
void hal_sr_latch(uint8_t chain);

/* ---------------------------------------------------------------- SPI A: ЦАП и контроллер нагрузок */

/* Полнодуплексный обмен; CS активирует вызывающий (CS_PWR — вывод, CS_LOAD — бит SR0). */
void hal_spia_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

/* ---------------------------------------------------------------- I²C секций A (0) и B (1) */

#define HAL_I2C_OK 0
#define HAL_I2C_NACK (-1)
#define HAL_I2C_BUS_ERROR (-2)
#define HAL_I2C_TIMEOUT (-3)

int hal_i2c_write(uint8_t bus, uint8_t addr7, const uint8_t *data, size_t len);
int hal_i2c_read(uint8_t bus, uint8_t addr7, uint8_t *data, size_t len);
int hal_i2c_write_read(uint8_t bus, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);

/* Переинициализация контроллера шины: ступень лестницы восстановления (FW-158).
 * Если SDA удерживается низким, шина освобождается девятью тактами SCL. */
void hal_i2c_reset(uint8_t bus);

/* Наладка: флаги состояния контроллера (ISR) и уровни SCL/SDA; смена TIMINGR. */
uint32_t hal_i2c_status(uint8_t bus, bool *scl, bool *sda);
void hal_i2c_set_timing(uint8_t bus, uint32_t timingr);

/* ---------------------------------------------------------------- watchdog */

void hal_wdt_start(uint32_t period_ms);
void hal_wdt_kick(void);

/* ---------------------------------------------------------------- консоль USART3 */

size_t hal_console_read(uint8_t *buf, size_t max);
size_t hal_console_write(const uint8_t *buf, size_t len);

/* ---------------------------------------------------------------- сеть */

struct hal_net_config {
    uint8_t mac[6];
    bool dhcp;
    uint32_t addr, mask, gw; /* сетевой порядок байтов (как в памяти: 192.168.0.20 → 0xC0A80014 в host order) */
    uint32_t keepalive_idle_s, keepalive_intvl_s, keepalive_cnt;
    uint16_t port;
};

void hal_net_init(const struct hal_net_config *cfg);
void hal_net_poll(void);
bool hal_net_link_up(void);
uint32_t hal_net_addr(void);          /* текущий адрес в host order, 0 — нет */
bool hal_net_dhcp_bound(void);
void hal_net_set_static(uint32_t addr, uint32_t mask, uint32_t gw); /* возврат к стандартному адресу (FW-228) */

/* Активный клиент: запись без блокировки, возвращает принятое; закрытие. */
size_t hal_net_send(const uint8_t *buf, size_t len);
void hal_net_close_client(void);
bool hal_net_client_connected(void);

/* Колбэки ядра (реализованы в core/session.c): только очередь событий, без логики. */
bool core_net_on_accept(void);                          /* false → HAL отвечает ERR:BUSY и закрывает */
void core_net_on_data(const uint8_t *buf, size_t len);  /* байты активного клиента */
void core_net_on_closed(void);                          /* активный клиент пропал */

/* ---------------------------------------------------------------- flash: сектор CFG и второй банк */

#define HAL_CFG_SIZE (16u * 1024u)

const uint8_t *hal_cfg_base(void);
int hal_cfg_erase(void);
int hal_cfg_write(uint32_t offset, const uint8_t *data, size_t len); /* offset и len кратны 4 */

#define HAL_BANK_SIZE (1024u * 1024u)
#define HAL_BANK_SECTORS 12u
const uint8_t *hal_bank_inactive_base(void);
int hal_bank_erase_inactive(uint32_t sector); /* 0…11, по одному: между секторами ядро крутит svc_poll */
int hal_bank_write(uint32_t offset, const uint8_t *data, size_t len);
uint8_t hal_bank_active(void);             /* 1 или 2 */
int hal_bank_set_boot(uint8_t bank);       /* переключить BFB2; сброс делает вызывающий */

struct hal_optbytes {
    bool ndbank;  /* 1 = один банк */
    bool ndboot;
    bool iwdg_sw; /* 1 = watchdog программный */
};
struct hal_optbytes hal_optbytes_read(void);

/* ---------------------------------------------------------------- кнопка USER */

bool hal_button_pressed(void);

#endif /* HAL_H */
