/* Имена сигналов и реле → выводы; запись с учётом полярностей констант сборки. */
#ifndef CORE_SIGNALS_H
#define CORE_SIGNALS_H

#include <stdbool.h>
#include <stdint.h>

#include "relay_table.h"
#include "signal_table.h"

/* Поиск без учёта регистра; -1 если нет. */
int signal_find(const char *name);
int relay_find(const char *name);

/* Настроить все сигналы signal_table по их описанию (dir, pull, init), кроме тех,
 * что уже выставлены ранними шагами FW-209. */
void signals_configure_rest(void);

/* Сигнал-выход: логическое «включено» с учётом PWRON_AB_ON_LEVEL, PWRON_RSP_ON_LEVEL,
 * CS_ACTIVE_LOW; линии C*: включено — выход 1, выключено — вход с подтяжкой вниз. */
void signal_set(enum signal_id id, bool on);
/* Уровень на выводе (вход или выход). */
bool signal_read(enum signal_id id);

/* Реле: вывод MCU или бит образа SR (без записи цепочки). */
void relay_pin_config_all_off(void);
void relay_drive(const struct relay_desc *r, bool on);

#endif /* CORE_SIGNALS_H */
