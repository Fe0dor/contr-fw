#ifndef CORE_SECTIONS_H
#define CORE_SECTIONS_H
#include "cmd.h"
/* Element order per section: odd POT, odd MTX, even POT, even POTH, even MTX. */
void section_off(unsigned bus);
bool section_test(unsigned bus);
void cmd_res_pwr(struct cmdctx *c);
void cmd_res_set(struct cmdctx *c);
void cmd_res_stat(struct cmdctx *c);
#endif
