#include "check.h"
#include "psu.h"
#include "cfg.h"
#include "update.h"
#include "safe.h"
static const char *table="PSU:CAL:SET 38000,39001,40004,41009,42016,43025,44036,45049,46064,47081,48100,49121,50144,51169,52196";
static void validation(void){
 device_boot();CHECK_PREFIX(con_cmd("PSU:CAL?"),"1,40,54,NONE,OFF");
 CHECK_PREFIX(con_cmd("PSU:VOLT 48"),"ERR:CAL_REQUIRED");CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:CAL_REQUIRED");
 CHECK_STR(con_cmd(table),"OK");CHECK(observed.in_safe_state);CHECK_PREFIX(con_cmd("PSU:CAL?"),"1,40,54,RAM,OFF");
 CHECK_STR(con_cmd("PSU:CAL:SAVE"),"OK");size_t free=cfg_free_records();CHECK_STR(con_cmd("PSU:CAL:SAVE"),"OK");CHECK(cfg_free_records()==free);
 struct cfg_psu_cal c;CHECK(cfg_get_psu_cal(&c));uint16_t prev=0;
 for(unsigned mv=40000;mv<=54000;mv++){
  char line[64];snprintf(line,sizeof line,"PSU:VOLT %u.%03u",mv/1000,mv%1000);CHECK_STR(con_cmd(line),"OK");
  unsigned i=(mv-40000)/1000,f=mv%1000;unsigned want=c.code[i];if(f)want+=((unsigned)(c.code[i+1]-c.code[i])*f+500)/1000;
  CHECK(desired.psu_dac==want);CHECK(desired.psu_dac>=prev);prev=desired.psu_dac;
 }
 CHECK(cfg_free_records()==free);CHECK_PREFIX(con_cmd("PSU:VOLT 39.999"),"ERR:RANGE");CHECK_PREFIX(con_cmd("PSU:VOLT 54.001"),"ERR:RANGE");
 CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:NOT_SAFE");CHECK_STR(con_cmd("SAFE"),"OK");CHECK(desired.psu_dac==38000);
 CHECK_PREFIX(con_cmd("PSU:CAL:SET 1,1,2,3,4,5,6,7,8,9,10,11,12,13,14"),"ERR:RANGE");
 CHECK_PREFIX(con_cmd("PSU:CAL:SET 1,2,3,4,5,6,7,8,9,10,11,12,13,14,65536"),"ERR:RANGE");
 CHECK_PREFIX(con_cmd("PSU:CAL:SET 1,2"),"ERR:RANGE");CHECK_PREFIX(con_cmd("PSU:CAL?"),"1,40,54,SAVED,OFF");
 CHECK_STR(con_cmd("PSU:CAL:MODE ON"),"OK");CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:CAL_REQUIRED");
 CHECK_PREFIX(con_cmd("PSU:VOLT 48"),"ERR:CAL_MODE");CHECK_STR(con_cmd("PSU:CAL:CODE 47765"),"OK");CHECK(desired.psu_dac==47765);
 CHECK_STR(con_cmd("PSU:CAL:MODE OFF"),"OK");CHECK_PREFIX(con_cmd("PSU:STAT?"),"OFF,OFF,40,38000");
 CHECK_PREFIX(con_cmd("PSU:CAL:CODE 0"),"ERR:CAL_MODE");
 CHECK_STR(con_cmd("SAFE"),"OK");CHECK_STR(con_cmd("PSU:CAL:MODE ON"),"OK");CHECK_STR(con_cmd("PSU:CAL:CODE 65535"),"OK");CHECK_STR(con_cmd("SAFE"),"OK");CHECK_PREFIX(con_cmd("PSU:CAL?"),"1,40,54,SAVED,OFF");
 CHECK_STR(con_cmd("PSU:CAL:SET 38001,39002,40005,41010,42017,43026,44037,45050,46065,47082,48101,49122,50145,51170,52197"),"OK");
 host_cfg_fail_writes(1);CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:CAL_STORAGE");CHECK(cfg_get_psu_cal(&c)&&c.code[0]==38000);
 CHECK_STR(con_cmd("PSU:CAL:LOAD"),"OK");CHECK(desired.psu_dac==38000);
 cfg_init();CHECK(psu_init());CHECK_PREFIX(con_cmd("PSU:CAL?"),"1,40,54,SAVED,OFF");
 uint8_t buf[CFG_RECORD_SIZE-1];CHECK(cfg_snapshot(buf,sizeof buf)==SIZE_MAX);
}
int main(void){validation();return test_result("psu_cal");}
