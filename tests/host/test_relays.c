#include "check.h"
#include "config.h"
#include "relays.h"
#include "sr.h"
static void tick(void) { host_advance_ms(1); }
static void boot(void) { device_boot(); host_net_set_poll_hook(tick); }
static void clean(void) { CHECK_STR(con_cmd("ROUT:LOW:ALL"), "OK"); host_journal_clear(); }
static bool physical(const struct relay_desc *r) {
    if (r->addr.kind == RELAY_ADDR_PIN) return host_pin(r->addr.a,r->addr.b)->level;
    return (host_sr_outputs(r->addr.a)[r->addr.b] >> r->addr.c) & 1u;
}
static void all_names(void) {
    boot(); char cmd[128];
    for(int i=0;i<RELAY_COUNT;i++) {
        clean();snprintf(cmd,sizeof cmd,"ROUT:HIGH %s",relay_table[i].name);
        CHECK_STR(con_cmd(cmd),"OK"); CHECK_STR(con_cmd("ROUT:STAT?"),relay_table[i].name);
        for(int j=0;j<RELAY_COUNT;j++) CHECK(physical(&relay_table[j]) == (i==j));
        CHECK(sr_image_get(0,SR0_CS_LOAD_REG,SR0_CS_LOAD_BIT));
        host_journal_clear();CHECK_STR(con_cmd(cmd),"OK");CHECK(host_journal_count()==0);
        clean(); for(int j=0;j<RELAY_COUNT;j++) CHECK(!physical(&relay_table[j]));
    }
    CHECK_STR(con_cmd("rout:high m4,v1.1,m4"),"OK");CHECK_STR(con_cmd("ROUT:STAT?"),"V1.1,M4");
    host_journal_clear(); CHECK_STR(con_cmd("ROUT:HIGH L1,M99"),"ERR:RANGE");CHECK(host_journal_count()==0);
    CHECK_STR(con_cmd("ROUT:SET M5,"),"ERR:RANGE");CHECK_STR(con_cmd("ROUT:STAT?"),"V1.1,M4");
    CHECK_STR(con_cmd("ROUT:SET"),"OK");CHECK_STR(con_cmd("ROUT:STAT?"),"");
    CHECK_STR(con_cmd("ROUT:HIGH L1,L2,L3,L4,L5,PM1,PM2,PM3,CH2.1"),"OK");
    CHECK_STR(con_cmd("SAFE"),"OK");CHECK_STR(con_cmd("ROUT:STAT?"),"");
}
static void all_pairs(void) {
    boot(); char cmd[128];
    for(int i=0;i<RELAY_COUNT;i++) for(int j=i+1;j<RELAY_COUNT;j++) {
        if(!(relay_table[i].group_mask & relay_table[j].group_mask)) continue;
        clean();snprintf(cmd,sizeof cmd,"ROUT:HIGH %s,%s",relay_table[i].name,relay_table[j].name);
        CHECK_PREFIX(con_cmd(cmd),"ERR:INTERLOCK,");CHECK(host_journal_count()==0);
        CHECK_STR(con_cmd("ROUT:STAT?"),"");
    }
    clean(); CHECK_STR(con_cmd("ROUT:HIGH M4"),"OK");host_journal_clear();
    CHECK_STR(con_cmd("ROUT:HIGH V1.1,M5"),"ERR:INTERLOCK,M__M4__M5,M4,M5");
    CHECK(host_journal_count()==0);CHECK_STR(con_cmd("ROUT:STAT?"),"M4");
    CHECK(tcp_connect()); CHECK(strstr(tcp_cmd("ROUT:HIGH M4"),";OK"));
    CHECK_STR(con_cmd("ROUT:HIGH M5"),"ERR:BUSY");
    CHECK(strstr(tcp_cmd("ROUT:HIGH M5"),";ERR:INTERLOCK,M__M4__M5,M4,M5"));
    CHECK_STR(con_cmd("SAFE"),"OK");CHECK(strstr(tcp_cmd("ROUT:STAT?"),";") && !desired.relays[0] && !desired.relays[1] && !desired.relays[2]);
}
static const struct relay_desc *old_relay,*new_relay;
static uint32_t off_seen;static bool saw_off,saw_new_early;
static void observe_break(void) {
    tick();
    if(!physical(old_relay) && !saw_off){off_seen=hal_millis();saw_off=true;}
    if(physical(new_relay))saw_new_early=true;
}
static void break_order(void) {
    boot();
    /* Both GPIO and same-chain SR switches. */
    for(int pass=0;pass<2;pass++) {
        int a=-1,b=-1;
        for(int i=0;i<RELAY_COUNT && a<0;i++) for(int j=i+1;j<RELAY_COUNT;j++) {
            const struct relay_desc *x=&relay_table[i],*y=&relay_table[j];
            if(x->addr.kind==pass && y->addr.kind==pass && (pass==0 || x->addr.a==y->addr.a) && (x->group_mask & y->group_mask)){a=i;b=j;break;}
        }
        CHECK(a>=0 && b>=0);if(a<0)continue;
        old_relay=&relay_table[a];new_relay=&relay_table[b];char cmd[100];
        snprintf(cmd,sizeof cmd,"ROUT:SET %s",old_relay->name);CHECK_STR(con_cmd(cmd),"OK");
        saw_off=saw_new_early=false;host_net_set_poll_hook(observe_break);
        snprintf(cmd,sizeof cmd,"ROUT:SET %s",new_relay->name);
        host_console_push_line(cmd);core_step();
        host_net_set_poll_hook(tick);
        CHECK(saw_off);CHECK(!saw_new_early);CHECK(hal_millis()-off_seen>=RELAY_BREAK_MS);
        CHECK(!physical(old_relay));CHECK(physical(new_relay));
        CHECK(host_console_take_line(reply_buf,sizeof reply_buf));CHECK_STR(reply_buf,"OK");
        if(pass==1) { /* complete images are written on both phases */
            CHECK(memcmp(host_sr_outputs(0),desired.sr_image[0],3)==0);
            CHECK(memcmp(host_sr_outputs(1),desired.sr_image[1],4)==0);
        }
    }
    host_set_ms(UINT32_MAX-10);CHECK_STR(con_cmd("ROUT:SET M4"),"OK");
}
static int polls;static bool drop;
static void interrupt_switch(void) {
    tick();if(++polls==5) {if(drop)host_net_disconnect();else host_console_push_line("SAFE");}
}
static void abort_switch(void) {
    for(int scenario=0;scenario<2;scenario++) {
        boot();CHECK(tcp_connect());CHECK(strstr(tcp_cmd("ROUT:SET M4"),";OK"));
        polls=0;drop=scenario!=0;host_net_set_poll_hook(interrupt_switch);
        host_net_push_line("ROUT:SET M5");device_run(5);host_net_set_poll_hook(tick);
        for(int i=0;i<RELAY_COUNT;i++) CHECK(!physical(&relay_table[i]));
        CHECK(!desired.relays[0] && !desired.relays[1] && !desired.relays[2]);
        if(!drop){CHECK(host_net_take_line(reply_buf,sizeof reply_buf));CHECK(strstr(reply_buf,"ERR:ABORTED"));}
    }
}
static void diagnostic_preserves(void) {
    boot();CHECK_STR(con_cmd("ROUT:HIGH M4,V1.1,CH1.1,D1_G1,L1"),"OK");
    struct desired before=desired;
    host_journal_clear();
    CHECK_PREFIX(con_cmd("TEST:ALL?"),"WARN;SR0:OK;SR1:UNVERIFIED;");CHECK(memcmp(&before,&desired,sizeof desired)==0);
    for(int i=0;i<RELAY_COUNT;i++)CHECK(physical(&relay_table[i])==relays_bit(desired.relays,relay_table[i].number));
    unsigned shifts=0,latches=0;
    for(size_t i=0;i<host_journal_count();i++) {
        const char *line=host_journal_at(i);
        if(strncmp(line,"sr_shift 0 ",11)==0) {
            unsigned x,y,z;char extra;
            CHECK(sscanf(line+11,"%2x%2x%2x%c",&x,&y,&z,&extra)==3);
            CHECK(x==before.sr_image[0][0] && y==before.sr_image[0][1]);
            CHECK((z & ~16u)==(before.sr_image[0][2] & ~16u));
            shifts++;
        } else { CHECK_STR(line,"sr_latch 0");latches++; }
    }
    CHECK(shifts==3 && latches==3);
    /* A stuck low AND a stuck high loop must fail; original test bit restored. */
    host_sr_loop_broken(true);
    for(int level=0;level<2;level++){
        host_gpio_set_input(1,7,level!=0);CHECK_PREFIX(con_cmd("TEST:ALL?"),"FAIL;SR0:FAIL;");
        CHECK(memcmp(&before,&desired,sizeof desired)==0);
    }
    sr_image_set(0,2,4,true);sr_write(0);host_sr_loop_broken(false);CHECK(sr_test_loop());CHECK(sr_image_get(0,2,4));
    /* CS_LOAD changes preserve every relay bit on both transitions. */
    for(int level=0;level<2;level++){
        sr_image_set(0,2,1,level!=0);sr_write(0);
        for(int i=0;i<RELAY_COUNT;i++)CHECK(physical(&relay_table[i])==relays_bit(desired.relays,relay_table[i].number));
    }
}
int main(void){all_names();all_pairs();break_order();abort_switch();diagnostic_preserves();return test_result("relays");}
