/* Сетевые настройки: MAC из UID (FW-226), режим static/dhcp (FW-227), возврат к
 * стандартному адресу (FW-228), кнопка USER (FW-229), SYST:NET? (FW-230). */
#ifndef CORE_NETCFG_H
#define CORE_NETCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg.h"

/* Локально администрируемый MAC из 96-битного UID: детерминированная функция. */
void netcfg_mac_from_uid(const uint8_t uid[12], uint8_t mac[6]);

/* Разбор "a.b.c.d" → host order; false при ошибке. */
bool netcfg_parse_ip(const char *s, uint32_t *out);
/* Запись адреса в буфер не короче 16 байт. */
void netcfg_format_ip(uint32_t addr, char *out);

/* Стандартные настройки без записи. */
void netcfg_defaults(struct cfg_net *out);

/* Разбор аргументов SYST:PROV:NET: mode[,addr,mask,gw]; false при ошибке. */
bool netcfg_parse_prov(int argc, char *const *argv, struct cfg_net *out);

/* Запуск сети при старте по CFG или стандарту. */
void netcfg_start(void);

/* Служебный шаг: таймаут DHCP, удержание кнопки. */
void netcfg_poll(void);

/* Текущий режим для SYST:NET? и TEST:ALL?. */
const char *netcfg_mode_name(void);
void netcfg_mac_string(char *out); /* не короче 18 байт */

#endif /* CORE_NETCFG_H */
