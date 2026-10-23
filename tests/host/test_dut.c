#include "check.h"
#include "dut.h"
#include "signals.h"
#include "config.h"
static void advance(unsigned ms) { for (unsigned i=0;i<ms;i++) { host_advance_ms(1); dut_poll(); } }
static void button(enum signal_id id,bool level) { host_gpio_set_input(signal_table[id].port,signal_table[id].pin,level); }
static struct host_pin *pin(enum signal_id id) {return host_pin(signal_table[id].port,signal_table[id].pin);}
static void commands(void) {
    device_boot();
    CHECK(strstr(con_cmd("DUT:STAT?"),"C20,OFF;INTERVENTION,0;PWROK,NA,NA,NA"));
    CHECK_STR(con_cmd("dut:set c1,c7,on"),"OK");
    CHECK(desired.dut_lines==65);CHECK(pin(SIG_C1)->mode==HAL_PIN_OUT&&pin(SIG_C1)->level);
    uint32_t before=desired.dut_lines;
    const char *bad[]={"DUT:SET C2,C99,ON","DUT:SET C2,C2,ON","DUT:SET C01,ON","DUT:SET C2,1","DUT:SET ON","DUT:OFF:ALL C1","DUT:STAT? C1","DUT:SET C2,,OFF"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){host_journal_clear();CHECK_PREFIX(con_cmd(bad[i]),"ERR:RANGE");CHECK(desired.dut_lines==before);CHECK(host_journal_count()==0);}
    CHECK_STR(con_cmd("DUT:SET C1,C2,C3,C4,C5,C6,C7,C8,C9,C10,C11,C12,C13,C14,C15,C16,C17,C18,C19,C20,ON"),"OK");
    CHECK(desired.dut_lines==0xfffff);
    host_journal_clear();CHECK_STR(con_cmd("DUT:SET C1,ON"),"OK");CHECK(host_journal_count()==0);
    CHECK_STR(con_cmd("DUT:OFF:ALL"),"OK");CHECK(desired.dut_lines==0);
    for(int i=0;i<SIGNAL_COUNT;i++)if(signal_table[i].dir==SIGNAL_DUT){CHECK(pin(i)->mode==HAL_PIN_IN);CHECK(pin(i)->pull==HAL_PULL_DOWN);}
    CHECK(tcp_connect());CHECK_STR(con_cmd("DUT:SET C1,ON"),"ERR:BUSY");
    CHECK(strstr(tcp_cmd("DUT:SET C1,ON"),";OK"));
    host_net_disconnect();device_run(2);CHECK(desired.dut_lines==0);CHECK(pin(SIG_C1)->mode==HAL_PIN_IN);
}
static void manual(void) {
    device_boot();uint32_t g=observed.generation;
    button(SIG_C3,true);advance(20);button(SIG_C3,false);advance(50);
    CHECK(observed.generation==g);CHECK(strstr(con_cmd("DUT:STAT?"),"INTERVENTION,0"));
    button(SIG_C3,true);advance(39);CHECK(strstr(con_cmd("DUT:STAT?"),"C3,OFF"));
    advance(1);CHECK(observed.generation==g+1);CHECK(desired.dut_lines==0);
    CHECK(pin(SIG_C3)->mode==HAL_PIN_IN);CHECK(strstr(con_cmd("DUT:STAT?"),"C3,MANUAL"));
    CHECK(strstr(con_cmd("DUT:STAT?"),"INTERVENTION,0"));
    button(SIG_C3,false);advance(40);CHECK(observed.generation==g+2);
    CHECK(strstr(con_cmd("DUT:STAT?"),"INTERVENTION,1"));CHECK(strstr(con_cmd("DUT:STAT?"),"INTERVENTION,0"));
    CHECK(strstr(con_cmd("SYST:LOG?"),"DUT C3 MANUAL"));CHECK(strstr(con_cmd("SYST:LOG?"),"DUT C3 OFF"));
    /* SAFE does not clear physical MANUAL or read-clear evidence. */
    button(SIG_C3,true);advance(40);CHECK_STR(con_cmd("SAFE"),"OK");
    CHECK(strstr(con_cmd("DUT:STAT?"),"C3,MANUAL"));
    CHECK_STR(con_cmd("DUT:SET C3,ON"),"OK");advance(100);CHECK(strstr(con_cmd("DUT:STAT?"),"C3,ON"));
    CHECK_STR(con_cmd("DUT:SET C3,OFF"),"OK");advance(40);CHECK(strstr(con_cmd("DUT:STAT?"),"C3,MANUAL"));
    /* Debounce elapsed arithmetic survives millis wrap. */
    device_boot();host_set_ms(UINT32_MAX-20);dut_init();button(SIG_C20,true);advance(40);
    CHECK(strstr(con_cmd("DUT:STAT?"),"C20,MANUAL"));
}
static void during_wait(void) {
    device_boot();button(SIG_C4,true);
    host_net_set_poll_hook(NULL);
    for(int i=0;i<50;i++){host_advance_ms(1);svc_poll();}
    CHECK(strstr(con_cmd("DUT:STAT?"),"C4,MANUAL"));
}
static void every_line(void) {
    device_boot();
    for (unsigned n=1;n<=20;n++) {
        char name[8],command[32];snprintf(name,sizeof name,"C%u",n);
        enum signal_id id=(enum signal_id)signal_find(name);
        snprintf(command,sizeof command,"DUT:SET C%u,ON",n);CHECK_STR(con_cmd(command),"OK");
        CHECK(pin(id)->mode==HAL_PIN_OUT && pin(id)->level);
        button(id,true);CHECK_STR(con_cmd("DUT:OFF:ALL"),"OK");advance(40);
        snprintf(name,sizeof name,"C%u,",n);
        const char *r=strstr(con_cmd("DUT:STAT?"),name);CHECK(r && strstr(r,"MANUAL")==r+strlen(name));
        CHECK(pin(id)->mode==HAL_PIN_IN && pin(id)->pull==HAL_PULL_DOWN);
        button(id,false);advance(40);CHECK_STR(con_cmd("DUT:OFF:ALL"),"OK");
    }
}
int main(void){commands();manual();during_wait();every_line();return test_result("dut");}
