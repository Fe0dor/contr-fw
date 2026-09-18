/* Управление заглушками hal/host из тестов: виртуальное время, журнал вызовов,
 * модели GPIO, I²C, flash, каналы в памяти (А2, А13). */
#ifndef HAL_HOST_H
#define HAL_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

/* Сброс всех заглушек; вызывать в начале каждого теста. */
void host_reset_all(void);

/* ---- время ---- */
void host_advance_ms(uint32_t ms);
void host_set_ms(uint32_t ms);

/* ---- журнал вызовов HAL: строки вида "gpio_config C7 out none 0", "sr_shift 0 ..." ---- */
#define HOST_JOURNAL_MAX 512
size_t host_journal_count(void);
const char *host_journal_at(size_t i);
void host_journal_clear(void);
/* Индекс первой записи, начинающейся с prefix (после from); -1 если нет. */
int host_journal_find(const char *prefix, size_t from);

/* ---- GPIO ---- */
struct host_pin {
    bool configured;
    enum hal_pin_mode mode;
    enum hal_pull pull;
    bool level;   /* выход: уровень выхода; вход: значение, заданное host_gpio_set_input */
};
struct host_pin *host_pin(uint8_t port, uint8_t pin);
void host_gpio_set_input(uint8_t port, uint8_t pin, bool level);

/* ---- цепочки SR: последнее защёлкнутое содержимое выходов ---- */
const uint8_t *host_sr_outputs(uint8_t chain);
bool host_sr_oe_active(uint8_t chain);

/* ---- I²C: обработчик модели; возвращает код HAL_I2C_* ---- */
typedef int (*host_i2c_fn)(uint8_t bus, uint8_t addr7, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen);
void host_i2c_set_handler(host_i2c_fn fn);

/* ---- SPI A: ответ модели на следующий обмен ---- */
void host_spia_set_reply(const uint8_t *rx, size_t len);

/* ---- сброс и flash ---- */
bool host_reset_requested(void);   /* hal_reset() вызван; флаг снимается host_reset_all */
void host_set_reset_cause(uint32_t cause);
uint8_t *host_cfg_memory(void);    /* HAL_CFG_SIZE байт */
uint8_t *host_bank_memory(void);   /* HAL_BANK_SIZE байт неактивного банка */
void host_set_active_bank(uint8_t bank);
uint8_t host_boot_bank(void);      /* последний hal_bank_set_boot */
void host_set_optbytes(bool ndbank, bool ndboot, bool iwdg_sw);
void host_set_uid(const uint8_t uid[12]);
void host_set_button(bool pressed);
unsigned host_wdt_kicks(void);
/* Число стёртых секторов банка и записей (для проверки, что flash не трогали). */
unsigned host_flash_ops(void);

/* ---- консоль в памяти ---- */
void host_console_push(const uint8_t *buf, size_t len);
void host_console_push_line(const char *line);
size_t host_console_take(uint8_t *buf, size_t max);
/* Забрать одну строку ответа (без '\n'); false — нет строки. */
bool host_console_take_line(char *out, size_t max);

/* ---- сеть в памяти ---- */
bool host_net_connect(void);       /* → core_net_on_accept; false = устройство ответило BUSY */
void host_net_push_line(const char *line);
void host_net_push(const uint8_t *buf, size_t len);
size_t host_net_take(uint8_t *buf, size_t max);
bool host_net_take_line(char *out, size_t max);
void host_net_disconnect(void);    /* клиент ушёл → core_net_on_closed */
bool host_net_device_closed(void); /* устройство само закрыло клиента */
void host_net_set_link(bool up);
void host_net_set_dhcp_bound(bool bound);
uint32_t host_net_static_addr(void); /* последний hal_net_set_static */
bool host_net_initialized(void);
void host_net_set_poll_hook(void (*hook)(void)); /* deterministic service-step injection */
void host_net_set_send_limit(size_t limit); /* предел hal_net_send за вызов, 0 — нет */

#endif /* HAL_HOST_H */
