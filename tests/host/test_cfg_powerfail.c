#include "check.h"
#include "cfg.h"
#include "config.h"
#include "update.h"
#include <setjmp.h>

static jmp_buf power_cut;
static unsigned events, cut_at;
static bool trace_only;
static void fault(const char *op,size_t offset) {
    /* Every programmed word; first/middle/last erase chunks. */
    if (strstr(op,"erase") && offset!=64 && offset!=8192 && offset!=HAL_CFG_SIZE
        && offset!=HAL_CFG_BACKUP_SIZE) return;
    events++;
    if (!trace_only && events==cut_at) longjmp(power_cut,1);
}
static struct cfg_net network={0,0x0a141e28,0xffffff00,0x0a141e01};
static void prepare(unsigned bank) {
    device_boot();host_set_active_bank((uint8_t)bank);cfg_init();cfg_set_erase_allowed(true);
    CHECK(cfg_set_serial("PERSIST")==0);
    CHECK(cfg_set_net(&network)==0);
    struct cfg_trial t={(uint8_t)bank,1,1};CHECK(cfg_set_trial(&t)==0);
    struct cfg_psu_cal c;for(unsigned i=0;i<PSU_CAL_POINTS;i++)c.code[i]=(uint16_t)(40000+i*1000);CHECK(cfg_set_psu_cal(&c)==0);
    while(cfg_free_records())CHECK(cfg_set_net(&network)==0);
}
static void verify(unsigned bank) {
    char serial[CFG_SERIAL_MAX+1];struct cfg_net n;struct cfg_trial t;
    struct cfg_psu_cal c;CHECK(cfg_get_psu_cal(&c));for(unsigned i=0;i<PSU_CAL_POINTS;i++)CHECK(c.code[i]==40000+i*1000);
    CHECK(cfg_get_serial(serial)&&strcmp(serial,"PERSIST")==0);
    CHECK(cfg_get_net(&n)&&memcmp(&n.addr,&network.addr,3*sizeof(uint32_t))==0);
    CHECK(cfg_get_trial(&t)&&t.bank==bank&&t.boot_count==1&&t.confirmed==1);
}
static unsigned count_events(void) {
    prepare(1);events=0;trace_only=true;host_cfg_flash_hook(fault);
    CHECK(cfg_compact()==0);host_cfg_flash_hook(NULL);trace_only=false;return events;
}
static void cuts(void) {
    unsigned total=count_events();
    for(unsigned bank=1;bank<=2;bank++)for(cut_at=1;cut_at<=total;cut_at++){
        prepare(bank);events=0;host_cfg_flash_hook(fault);
        if(setjmp(power_cut)==0){cfg_compact();CHECK(false);}
        host_cfg_flash_hook(NULL);cfg_init();verify(bank);
        cfg_set_erase_allowed(true);CHECK(cfg_compact()==0);verify(bank);
        struct cfg_net changed=network;changed.addr++;
        CHECK(cfg_set_net(&changed)==0);cfg_init();struct cfg_net n;
        CHECK(cfg_get_net(&n)&&n.addr==changed.addr);
        char serial[CFG_SERIAL_MAX+1];CHECK(cfg_get_serial(serial)&&strcmp(serial,"PERSIST")==0);
    }
    printf("power-cut boundaries: %u per bank\n",total);
}
static void restore_failure(void) {
    prepare(1);
    /* Primary restore write fails after a valid committed snapshot exists. */
    host_cfg_fail_writes(2);
    CHECK(cfg_compact()!=0);verify(1);CHECK(cfg_free_records()==0);
    CHECK(cfg_set_net(&network)!=0);
    cfg_init();verify(1);CHECK(cfg_free_records()==0);
    cfg_init();verify(1);CHECK(cfg_free_records()==CFG_RECORDS-4);
}
static void stale_backup_cannot_replay(void) {
    prepare(1);CHECK(cfg_compact()==0);
    struct cfg_net changed=network;changed.addr++;
    CHECK(cfg_set_net(&changed)==0);
    /* Simulate a partial erase that resurrects only the retired word. */
    uint8_t *b=(uint8_t *)hal_cfg_backup_base();memset(b+20,0xff,4);
    cfg_init();struct cfg_net n;CHECK(cfg_get_net(&n)&&n.addr==changed.addr);
    cfg_set_erase_allowed(true);CHECK(cfg_compact()==0);cfg_init();
    CHECK(cfg_get_net(&n)&&n.addr==changed.addr);
}
static void full_button(void) {
    prepare(1);host_set_button(true);device_run(1);
    host_advance_ms(BUTTON_RESET_HOLD_MS);device_run(1);CHECK(host_reset_requested());
    cfg_init();struct cfg_net n;CHECK(cfg_get_net(&n)&&n.addr==0xc0a80014);
    char serial[CFG_SERIAL_MAX+1];CHECK(cfg_get_serial(serial)&&strcmp(serial,"PERSIST")==0);
}

static void calibration_append_cuts(void) {
    for(cut_at=1;cut_at<=16;cut_at++) {
        device_boot();struct cfg_psu_cal old,next,got;
        for(unsigned i=0;i<PSU_CAL_POINTS;i++){old.code[i]=(uint16_t)(38000+i*1000);next.code[i]=old.code[i]+1;}
        CHECK(cfg_set_psu_cal(&old)==0);events=0;host_cfg_flash_hook(fault);
        if(setjmp(power_cut)==0){cfg_set_psu_cal(&next);CHECK(false);}
        host_cfg_flash_hook(NULL);cfg_init();CHECK(cfg_get_psu_cal(&got));
        CHECK(!memcmp(&got,&old,sizeof got)||!memcmp(&got,&next,sizeof got));
        if(cut_at<16)CHECK(!memcmp(&got,&old,sizeof got));
    }
    device_boot();struct cfg_psu_cal a,b,got;
    for(unsigned i=0;i<PSU_CAL_POINTS;i++){a.code[i]=(uint16_t)(38000+i*1000);b.code[i]=a.code[i]+1;}
    CHECK(cfg_set_psu_cal(&a)==0);CHECK(cfg_set_psu_cal(&b)==0);
    ((uint8_t *)hal_cfg_base())[CFG_RECORD_SIZE+10]^=1;cfg_init();
    CHECK(cfg_get_psu_cal(&got)&&!memcmp(&got,&a,sizeof got));
}

int main(void){calibration_append_cuts();cuts();restore_failure();stale_backup_cannot_replay();full_button();return test_result("cfg_powerfail");}
