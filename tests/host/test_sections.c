#include "check.h"
#include "config.h"
static void tick(void){host_advance_ms(1);}
static void boot(void){device_boot();host_net_set_poll_hook(tick);}
static void normal(void){
    boot();CHECK_STR(con_cmd("RES:SET R1,POT,128"),"ERR:SECTION_OFF");
    for(unsigned bus=0;bus<2;bus++){
        char line[64];snprintf(line,sizeof line,"RES:PWR %c,ON",'A'+bus);
        uint32_t start=hal_millis();CHECK_STR(con_cmd(line),"OK");CHECK(hal_millis()-start<1000);
        CHECK(desired.section_on[bus]);CHECK(desired.section_code[bus][0]==128);CHECK(desired.section_code[bus][3]==512);
        CHECK(host_pin(bus?3:2,bus?15:7)->mode==HAL_PIN_IN);
        host_journal_clear();CHECK_STR(con_cmd(line),"OK");CHECK(host_journal_count()==0);
        for(unsigned channel=bus*2+1;channel<=bus*2+2;channel++){
            const char *element[]={"POT","MTX","POTH"};unsigned max[]={256,channel%2?31:7,1023};
            for(unsigned e=0;e<(channel%2?2u:3u);e++){
                for(unsigned v=0;v<=max[e];v++){
                    snprintf(line,sizeof line,"RES:SET R%u,%s,%u",channel,element[e],v);CHECK_STR(con_cmd(line),"OK");
                }
                host_journal_clear();snprintf(line,sizeof line,"RES:SET R%u,%s,%u",channel,element[e],max[e]+1);
                CHECK_PREFIX(con_cmd(line),"ERR:RANGE,0..");CHECK(host_journal_count()==0);
            }
        }
    }
    CHECK_STR(con_cmd("RES:SET R2,MTX,7"),"OK");CHECK_STR(con_cmd("RES:SET R1,MTX,0"),"OK");CHECK(host_section_value(0,0x27)==224);
    CHECK_STR(con_cmd("RES:SET R1,MTX,31"),"OK");CHECK_STR(con_cmd("RES:SET R2,MTX,0"),"OK");CHECK(host_section_value(0,0x27)==31);
    host_journal_clear();
    const char *bad[]={"RES:SET R1,POT,100.5","RES:SET R1,POT,-1","RES:SET R1,POT,9999999999999999999999","RES:SET R1,POTH,1","RES:SET R5,POT,1","RES:PWR C,ON","RES:PWR A,1","RES:SET R1,POT,1,MTX,2"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)CHECK_PREFIX(con_cmd(bad[i]),"ERR:RANGE");
    CHECK(host_journal_count()==0);CHECK_STR(con_cmd("res:set r1,pot,64"),"OK");
    CHECK(host_sections_forbidden()==0);
    struct desired before=desired;
    CHECK(strstr(con_cmd("TEST:ALL?"),"I2C_A:OK;I2C_B:OK"));CHECK(memcmp(&before,&desired,sizeof desired)==0);
    CHECK_STR(con_cmd("SAFE"),"OK");CHECK(!desired.section_on[0]&&!desired.section_on[1]);
    CHECK_PREFIX(con_cmd("RES:STAT?"),"A:OFF;B:OFF;R1,POT,?;");
}
static void faults(void){
    boot();CHECK_STR(con_cmd("RES:PWR A,ON"),"OK");
    host_section_fault(0,0x2f,HOST_SECTION_TRANSIENT,1);
    CHECK_STR(con_cmd("RES:SET R1,POT,10"),"OK");CHECK(strstr(con_cmd("SYST:ERR?"),"RETRY"));
    CHECK_STR(con_cmd("RES:SET R2,MTX,7"),"OK");
    host_section_fault(0,0x27,HOST_SECTION_RESET,0);
    CHECK_STR(con_cmd("RES:SET R1,MTX,1"),"OK");CHECK(host_section_value(0,0x27)==225);
    CHECK(strstr(con_cmd("SYST:ERR?"),"RESET"));
    host_section_fault(0,0x2c,HOST_SECTION_RESET,0);
    CHECK_STR(con_cmd("RES:SET R2,POTH,900"),"OK");CHECK(host_section_value(0,0x2c)==900);
    CHECK_STR(con_cmd("RES:SET R2,POT,10"),"OK");
    CHECK(tcp_connect());CHECK(strstr(tcp_cmd("RES:PWR A,ON"),";OK"));CHECK(strstr(tcp_cmd("RES:SET R2,POT,10"),";OK"));
    uint32_t g=observed.generation;host_section_fault(0,0x2f,HOST_SECTION_POWER,0);
    CHECK(strstr(tcp_cmd("RES:SET R1,POT,40"),";OK"));CHECK(observed.generation==g+1);
    CHECK(desired.section_code[0][2]==128 && desired.section_code[0][0]==40);
    host_section_fault(0,0x2f,HOST_SECTION_MISSING,0);
    CHECK(strstr(tcp_cmd("RES:SET R1,POT,50"),";ERR:I2C_FAIL,R1,POT,"));CHECK(!desired.section_on[0]);
    CHECK(host_sections_forbidden()==0);
    boot();host_section_fault(0,0x2c,HOST_SECTION_MISSING,0);
    CHECK_STR(con_cmd("RES:PWR A,ON"),"ERR:SECTION_FAIL,4");CHECK(!desired.section_on[0]);
    boot();CHECK_STR(con_cmd("RES:PWR B,ON"),"OK");host_section_fault(1,0x2e,HOST_SECTION_MISMATCH,0);
    CHECK_PREFIX(con_cmd("RES:SET R4,POT,70"),"ERR:I2C_FAIL,R4,POT,");CHECK(!desired.section_on[1]);CHECK(host_sections_forbidden()==0);
}
static unsigned polls,scenario;
static void interrupt(void){tick();if(++polls==20){if(scenario==0)host_console_push_line("SAFE");else if(scenario==1)host_net_disconnect();else host_console_push_line("RES:STAT?");}}
static void interruptions(void){
    for(scenario=0;scenario<3;scenario++){
        boot();CHECK(tcp_connect());polls=0;host_net_set_poll_hook(interrupt);
        host_net_push_line("RES:PWR A,ON");device_run(6);host_net_set_poll_hook(tick);
        CHECK(host_net_take_line(reply_buf,sizeof reply_buf)||scenario==1);
        if(scenario==0)CHECK(strstr(reply_buf,"ERR:ABORTED"));
        if(scenario<2)CHECK(!desired.section_on[0]);
        else {CHECK(desired.section_on[0]);CHECK(host_console_take_line(reply_buf,sizeof reply_buf));CHECK_STR(reply_buf,"ERR:BUSY");}
    }
    boot();host_set_ms(UINT32_MAX-50);CHECK_STR(con_cmd("RES:PWR A,ON"),"OK");
}
static int slow(uint8_t bus,uint8_t addr,const uint8_t *w,size_t wn,uint8_t *r,size_t rn){host_advance_ms(25);return host_sections_xfer(bus,addr,w,wn,r,rn);}
static void deadline_test(void){
    boot();host_i2c_set_handler(slow);uint32_t start=hal_millis();CHECK_STR(con_cmd("RES:PWR A,ON"),"OK");CHECK(hal_millis()-start<=1000);
    host_section_fault(0,0x27,HOST_SECTION_MISMATCH,0);start=hal_millis();
    CHECK_PREFIX(con_cmd("RES:SET R1,MTX,5"),"ERR:I2C_FAIL,");CHECK(hal_millis()-start<=1000);CHECK(!desired.section_on[0]);
}
int main(void){normal();faults();interruptions();deadline_test();return test_result("sections");}
