#ifndef CORE_PSU_H
#define CORE_PSU_H
#include "cmd.h"
bool psu_init(void);
void psu_poll(void);
bool psu_safe_off(void);
bool psu_wait_dcok_off(void);
bool psu_dcok(void);
bool psu_alarm(void);
void cmd_psu_outp(struct cmdctx *c);
void cmd_psu_volt(struct cmdctx *c);
void cmd_psu_stat(struct cmdctx *c);
void cmd_psu_cal_stat(struct cmdctx *c);
void cmd_psu_cal_set(struct cmdctx *c);
void cmd_psu_cal_save(struct cmdctx *c);
void cmd_psu_cal_load(struct cmdctx *c);
void cmd_psu_cal_mode(struct cmdctx *c);
void cmd_psu_cal_code(struct cmdctx *c);
#endif
