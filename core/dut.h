#ifndef CORE_DUT_H
#define CORE_DUT_H
#include "cmd.h"
void dut_init(void);
void dut_poll(void);
void dut_off_all(void);
void cmd_dut_set(struct cmdctx *c);
void cmd_dut_off(struct cmdctx *c);
void cmd_dut_stat(struct cmdctx *c);
#endif
