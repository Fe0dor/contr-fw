#include "check.h"
#include "psu.h"
#include "signals.h"
#include "safe.h"
#include "config.h"
#include "cfg.h"
static void psu_boot(void){device_boot();struct cfg_psu_cal c;for(unsigned i=0;i<PSU_CAL_POINTS;i++)c.code[i]=(uint16_t)(i*4000);CHECK(cfg_set_psu_cal(&c)==0);CHECK(psu_init());}
static unsigned ticks,event;
static void input(enum signal_id id,bool value){host_gpio_set_input(signal_table[id].port,signal_table[id].pin,
    id==SIG_RSP_DC_OK ? value==(DCOK_ACTIVE_HIGH!=0) : value==(ALARM_ACTIVE_HIGH!=0));}
static void tick(void){
    host_advance_ms(1);++ticks;
    if(ticks!=50)return;
    if(event==1)input(SIG_RSP_DC_OK,true);
    if(event==2)input(SIG_RSP_ALARM,true);
    if(event==3)host_console_push_line("SAFE");
    if(event==4)host_net_disconnect();
    if(event==5)input(SIG_RSP_DC_OK,false);
}
static void clock_start(unsigned e){ticks=0;event=e;host_net_set_poll_hook(tick);}
static void validation(void){
    psu_boot();
    CHECK_STR(con_cmd("PSU:STAT?"),"OFF,OFF,40,0,0,0");
    CHECK(signal_read(SIG_RSP_DC_OK)); /* Physical high is inactive on RSP-2000. */
    CHECK(strstr(con_cmd("TEST:ALL?"),";DCOK:1;")!=NULL);
    CHECK(host_journal_find("spia 000000",0)>=0);
    CHECK_STR(con_cmd("PSU:VOLT 47"),"OK");CHECK(desired.psu_dac==28000);
    CHECK_STR(con_cmd("PSU:VOLT 54"),"OK");CHECK(desired.psu_dac==56000);
    CHECK_STR(con_cmd("PSU:VOLT 40"),"OK");CHECK(desired.psu_dac==0);
    CHECK_STR(con_cmd("PSU:VOLT 48.125"),"OK");CHECK(desired.psu_dac==32500);
    const char *bad[]={"PSU:VOLT 39.999","PSU:VOLT 54.001","PSU:VOLT NaN","PSU:VOLT 48e0","PSU:VOLT -42","PSU:VOLT 42.","PSU:VOLT 42.0000","PSU:VOLT 99999999999999999999","PSU:OUTP RSP,1","PSU:OUTP BAD,ON","PSU:STAT? 1"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){host_journal_clear();CHECK_PREFIX(con_cmd(bad[i]),"ERR:RANGE");CHECK(desired.psu_dac==32500);CHECK(host_journal_count()==0);}
    CHECK_STR(con_cmd("PSU:OUTP HLG,ON"),"OK");CHECK(desired.psu_hlg_on);
    CHECK_STR(con_cmd("SAFE"),"OK");CHECK(!desired.psu_hlg_on&&desired.psu_dac==0);
}
static void startup_and_faults(void){
    psu_boot();clock_start(1);host_journal_clear();
    CHECK_STR(con_cmd("PSU:OUTP RSP,ON"),"OK");CHECK(desired.psu_rsp_on);
    CHECK(!signal_read(SIG_RSP_DC_OK));CHECK(psu_dcok());
    int dac=host_journal_find("spia 000000",0),on=host_journal_find("gpio_write PG15 0",0);
    CHECK(dac>=0&&on>dac);CHECK(host_journal_find("spia_mode 1",0)<dac);CHECK(host_journal_find("spia_mode 0",0)>dac);CHECK(ticks>=50&&ticks<100);
    CHECK_STR(con_cmd("PSU:OUTP HLG,ON"),"OK");CHECK_STR(con_cmd("DUT:SET C1,ON"),"OK");
    CHECK_STR(con_cmd("ROUT:HIGH M4"),"OK");
    CHECK_STR(con_cmd("PSU:VOLT 48"),"OK");CHECK(desired.psu_rsp_on);
    uint32_t g=observed.generation;input(SIG_RSP_DC_OK,false);psu_poll();
    CHECK(!desired.psu_rsp_on&&desired.psu_hlg_on&&desired.dut_lines==1);CHECK(observed.generation==g);CHECK_STR(con_cmd("ROUT:STAT?"),"M4");
    CHECK_PREFIX(con_cmd("PSU:STAT?"),"OFF:PSU_FAULT,ON");
    input(SIG_RSP_DC_OK,true);host_advance_ms(10000);psu_poll();CHECK(!desired.psu_rsp_on);
    CHECK_STR(con_cmd("PSU:OUTP RSP,ON"),"OK");g=observed.generation;input(SIG_RSP_ALARM,true);psu_poll();
    CHECK(!desired.psu_rsp_on&&desired.psu_hlg_on);CHECK(observed.generation==g);CHECK_STR(con_cmd("ROUT:STAT?"),"M4");
    CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:PSU_ALARM");
    input(SIG_RSP_ALARM,false);psu_poll();CHECK(!desired.psu_rsp_on);
    psu_boot();clock_start(0);CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:PSU_FAULT,DCOK timeout");
    CHECK(!desired.psu_rsp_on);CHECK(ticks>=2000&&ticks<2020);
    psu_boot();clock_start(2);CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:PSU_ALARM");CHECK(!desired.psu_rsp_on&&ticks<100);
    psu_boot();host_set_ms(UINT32_MAX-20);clock_start(1);CHECK_STR(con_cmd("PSU:OUTP RSP,ON"),"OK");
}
static void safe_and_abort(void){
    psu_boot();clock_start(3);CHECK_STR(con_cmd("PSU:OUTP RSP,ON"),"ERR:ABORTED");device_run(4);
    CHECK(!desired.psu_rsp_on&&observed.safe.complete);
    psu_boot();CHECK(tcp_connect());clock_start(4);(void)tcp_cmd("PSU:OUTP RSP,ON");device_run(4);CHECK(!desired.psu_rsp_on);
    psu_boot();CHECK_STR(con_cmd("DUT:SET C1,ON"),"OK");CHECK_STR(con_cmd("PSU:OUTP HLG,ON"),"OK");
    input(SIG_RSP_DC_OK,true);clock_start(0);CHECK_PREFIX(con_cmd("SAFE"),"ERR:SAFE_INCOMPLETE");
    CHECK(observed.safe.steps[3]==STEP_FAILED&&!observed.safe.complete);
    CHECK_PREFIX(con_cmd("SYST:UPD:BEGIN 1024,0,0.6.1"),"ERR:NOT_SAFE");
    CHECK(observed.safe.steps[4]==STEP_OK&&observed.safe.steps[5]==STEP_OK);
    CHECK(!desired.psu_hlg_on&&!desired.psu_rsp_on&&!desired.dut_lines&&desired.psu_dac==0);
    CHECK(observed.safe.duration_ms>=2000&&observed.safe.duration_ms<2020);
    clock_start(5);CHECK_STR(con_cmd("SAFE"),"OK");CHECK(observed.safe.complete);
    host_psu_dac_fail(true);CHECK_PREFIX(con_cmd("PSU:VOLT 48"),"ERR:PSU_FAULT");
    CHECK_PREFIX(con_cmd("PSU:STAT?"),"OFF:PSU_FAULT,OFF,40,?");
    CHECK(!observed.safe.complete && observed.safe.steps[0]==STEP_FAILED);
    CHECK_PREFIX(con_cmd("SYST:UPD:BEGIN 1024,0,0.6.4"),"ERR:NOT_SAFE");
    CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:NOT_SAFE");
    CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:PSU_FAULT");CHECK(!desired.psu_rsp_on);
    CHECK_PREFIX(con_cmd("SAFE"),"ERR:SAFE_INCOMPLETE");CHECK(observed.safe.steps[0]==STEP_FAILED);
    host_psu_dac_fail(false);CHECK_STR(con_cmd("SAFE"),"OK");
    host_psu_dac_fail(true);
    CHECK_PREFIX(con_cmd("PSU:OUTP RSP,ON"),"ERR:PSU_FAULT");
    CHECK(!observed.safe.complete && observed.safe.steps[0]==STEP_FAILED);
    CHECK_PREFIX(con_cmd("SYST:UPD:BEGIN 1024,0,0.6.4"),"ERR:NOT_SAFE");
    CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:NOT_SAFE");
    host_reset_all();host_psu_dac_fail(true);board_early_init();core_init();CHECK(!observed.safe.complete);CHECK(observed.safe.steps[0]==STEP_FAILED);
}
static void boot_dcok(void){
    host_reset_all();board_early_init();input(SIG_RSP_DC_OK,true);clock_start(0);core_init();
    CHECK(psu_dcok());CHECK(!observed.safe.complete);
    CHECK(observed.safe.steps[3]==STEP_FAILED);
    CHECK(observed.safe.duration_ms>=2000);
    CHECK_PREFIX(con_cmd("SYST:UPD:BEGIN 1024,0,0.6.4"),"ERR:NOT_SAFE");
    CHECK_PREFIX(con_cmd("PSU:CAL:SAVE"),"ERR:NOT_SAFE");
    input(SIG_RSP_DC_OK,false);CHECK_STR(con_cmd("SAFE"),"OK");
    CHECK(observed.safe.complete);
    host_reset_all();board_early_init();input(SIG_RSP_DC_OK,true);clock_start(5);core_init();
    CHECK(observed.safe.complete && observed.safe.steps[3]==STEP_OK);
    CHECK(observed.safe.duration_ms>=50 && observed.safe.duration_ms<2000);
}
int main(void){validation();startup_and_faults();safe_and_abort();boot_dcok();return test_result("psu");}
