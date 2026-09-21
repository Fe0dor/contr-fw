/* FW-144..161. Only volatile registers are writable through this module. */
#include "sections.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core.h"
#include "errlog.h"
#include "hal.h"
#include "signals.h"
#include "state.h"

static const enum signal_id power[2] = {SIG_PWRON_A, SIG_PWRON_B};
static const enum signal_id reset[2][2] = {{SIG_RESETA0,SIG_RESETA1},{SIG_RESETB0,SIG_RESETB1}};
static const char *const elements[5] = {"POT","MTX","POT","POTH","MTX"};
static const unsigned limits[5] = {256,31,256,1023,7};
static const int16_t defaults[5] = {128,0,128,512,0};
static const uint8_t addresses[5] = {0x2f,0x27,0x2e,0x2c,0x27};
/* Commands are serialized by core. Every transfer and delay has an abort point. */
static uint32_t deadline;
static bool had_client, aborted;
static int last_read;

static bool ieq(const char *a,const char *b) {
    while(*a && *b) {if(toupper((unsigned char)*a++)!=toupper((unsigned char)*b++))return false;}
    return !*a && !*b;
}
static void operation_begin(void) {
    deadline=hal_millis()+SECTION_ON_MAX_MS-50u;
    had_client=core_client_connected(); aborted=false; last_read=-1;
}
static bool service(void) {
    svc_poll();
    if(core_abort_requested() || (had_client && !hal_net_client_connected()))aborted=true;
    return !aborted && (int32_t)(deadline-hal_millis())>0;
}
static bool pause_ms(unsigned ms) {
    uint32_t until=hal_millis()+ms+1u;
    do {if(!service())return false;}while((int32_t)(until-hal_millis())>0);
    return true;
}
static bool transfer(unsigned bus,uint8_t addr,const uint8_t *w,size_t wn,uint8_t *r,size_t rn) {
    if(!service())return false;
    return hal_i2c_write_read((uint8_t)bus,addr,w,wn,r,rn)==HAL_I2C_OK && service();
}
static bool reg_read(unsigned bus,uint8_t addr,uint8_t reg,uint8_t *v) {
    return transfer(bus,addr,&reg,1,v,1);
}
static bool reg_write_check(unsigned bus,uint8_t addr,uint8_t reg,uint8_t value) {
    uint8_t w[2]={reg,value},v=0;
    return transfer(bus,addr,w,2,NULL,0) && reg_read(bus,addr,reg,&v) && v==value;
}
static bool ad_command(unsigned bus,unsigned cmd,unsigned value) {
    uint8_t w[2]={(uint8_t)((cmd<<2)|(value>>8)),(uint8_t)value};
    return transfer(bus,0x2c,w,2,NULL,0);
}
static bool ad_read(unsigned bus,unsigned cmd,unsigned *value) {
    uint8_t r[2];
    /* AD5272 read is a command frame followed by a separate receive frame. */
    if(!ad_command(bus,cmd,0) || !transfer(bus,0x2c,NULL,0,r,2))return false;
    *value=((unsigned)(r[0]&3u)<<8)|r[1];return true;
}
static bool ad_init(unsigned bus) {
    unsigned value;
    return ad_command(bus,7,2) && ad_read(bus,8,&value) && (value&7u)==2u;
}
static bool matrix_init(unsigned bus,uint8_t image) {
    /* Latch first: outputs never expose a stale high while changing IODIR. */
    return reg_write_check(bus,0x27,0x0a,image) && reg_write_check(bus,0x27,0,0);
}
static bool set_once(unsigned bus,unsigned e,unsigned value) {
    uint8_t r[2],w[2];unsigned got;
    last_read=-1;
    if(e==0 || e==2) {
        w[0]=(uint8_t)(value>>8);w[1]=(uint8_t)value;
        if(!transfer(bus,addresses[e],w,2,NULL,0))return false;
        w[0]=0x0c;
        if(!transfer(bus,addresses[e],w,1,r,2))return false;
        got=((unsigned)(r[0]&3u)<<8)|r[1];
    } else if(e==3) {
        if(!ad_command(bus,1,value) || !ad_read(bus,2,&got))return false;
    } else {
        uint8_t before,after;unsigned shift=e==1?0:5,mask=e==1?31:224;
        if(!reg_read(bus,0x27,9,&before))return false;
        uint8_t target=(uint8_t)((before&~mask)|(value<<shift));
        w[0]=9;w[1]=target;
        if(!transfer(bus,0x27,w,2,NULL,0) || !reg_read(bus,0x27,9,&after))return false;
        last_read=(after>>shift)&limits[e];
        if(after!=target)return false; /* check neighbour bits too */
        got=(after>>shift)&limits[e];
    }
    last_read=(int)got;
    if(got!=value)return false;
    desired.section_code[bus][e]=(int16_t)value;return true;
}
void section_off(unsigned bus) {
    signal_set(power[bus],false);
    signal_set(reset[bus][0],false);signal_set(reset[bus][1],false);
    desired.section_on[bus]=false;
    for(unsigned e=0;e<5;e++)desired.section_code[bus][e]=-1;
}
/* Returns the failing FW-153 step; caller handles final reply/recovery. */
static unsigned power_on(unsigned bus) {
    section_off(bus);
    signal_set(power[bus],true);
#if PWROK_PRESENT
    const enum signal_id pin=bus?SIG_PWROK5:SIG_PWROK4;
    uint32_t until=hal_millis()+SECTION_PWROK_TIMEOUT_MS;
    while(signal_read(pin)==(PWROK_AB_ACTIVE_LOW!=0)) {
        if(!service() || (int32_t)(hal_millis()-until)>=0)return 2;
    }
#else
    if(!pause_ms(SECTION_SETTLE_MS))return 2;
#endif
    if(!service())return 3;
    signal_set(reset[bus][0],true);signal_set(reset[bus][1],true);
    if(!pause_ms(1))return 3;
    hal_i2c_reset((uint8_t)bus);
    if(!matrix_init(bus,0) || !ad_init(bus))return 4;
    for(unsigned e=0;e<5;e++)if(!set_once(bus,e,(unsigned)defaults[e]))return 4;
    for(unsigned i=0;i<4;i++) {
        static const uint8_t addr[4]={0x2c,0x2e,0x2f,0x27};
        if(!transfer(bus,addr[i],NULL,0,NULL,0))return 5;
    }
    desired.section_on[bus]=true;return 0;
}
void cmd_res_pwr(struct cmdctx *c) {
    unsigned bus;
    if(ieq(c->argv[0],"A"))bus=0;else if(ieq(c->argv[0],"B"))bus=1;
    else {resp_err(c,ERR_RANGE,NULL);return;}
    if(ieq(c->argv[1],"OFF")){section_off(bus);resp_ok(c);return;}
    if(!ieq(c->argv[1],"ON")){resp_err(c,ERR_RANGE,NULL);return;}
    if(desired.section_on[bus]){resp_ok(c);return;}
    operation_begin();unsigned failed=power_on(bus);
    if(failed){section_off(bus);char field[12];snprintf(field,sizeof field,"%u",failed);resp_err(c,aborted?ERR_ABORTED:ERR_SECTION_FAIL,aborted?NULL:field);return;}
    resp_ok(c);
}
static void recovery_log(unsigned bus,unsigned e,const char *stage) {
    char field[40];snprintf(field,sizeof field,"R%u,%s,%s",bus*2+(e<2?1:2),elements[e],stage);
    err_push(ERR_I2C_FAIL,field);log_event("I2C %s",field);
}
void cmd_res_set(struct cmdctx *c) {
    const char *ch=c->argv[0];
    if(strlen(ch)!=2 || toupper((unsigned char)ch[0])!='R' || ch[1]<'1' || ch[1]>'4'){resp_err(c,ERR_RANGE,NULL);return;}
    unsigned channel=(unsigned)(ch[1]-'1'),bus=channel/2,e;
    for(e=(channel%2?2:0);e<(channel%2?5:2);e++)if(ieq(c->argv[1],elements[e]))break;
    if(e==(channel%2?5u:2u)){resp_err(c,ERR_RANGE,NULL);return;}
    unsigned value=0;const char *p=c->argv[2];bool valid=*p!=0;
    for(;*p;p++){if(*p<'0'||*p>'9'||value>limits[e]){valid=false;break;}value=value*10u+(unsigned)(*p-'0');}
    if(!valid || value>limits[e]){char field[16];snprintf(field,sizeof field,"0..%u",limits[e]);resp_err(c,ERR_RANGE,field);return;}
    if(!desired.section_on[bus]){resp_err(c,ERR_SECTION_OFF,NULL);return;}
    operation_begin();
    if(set_once(bus,e,value)){resp_ok(c);return;}
    for(unsigned attempt=0;attempt<I2C_RETRIES && service();attempt++) {
        recovery_log(bus,e,"RETRY");hal_i2c_reset((uint8_t)bus);
        if(set_once(bus,e,value)){resp_ok(c);return;}
    }
    if((e==1||e==3||e==4) && service()) {
        recovery_log(bus,e,"RESET");
        unsigned pin=e==3?0:1;
        /* Reset of the shared expander must preserve the other matrix. */
        int16_t lo=desired.section_code[bus][1],hi=desired.section_code[bus][4];
        signal_set(reset[bus][pin],false);
        if(pause_ms(1)) {
            signal_set(reset[bus][pin],true);
            if(pause_ms(1)) {
                bool initialized=e==3?ad_init(bus):
                    (lo>=0 && hi>=0 && matrix_init(bus,(uint8_t)(lo|(hi<<5))));
                if(initialized && set_once(bus,e,value)){resp_ok(c);return;}
            }
        }
    }
    int failed_read=last_read;
    if(service()) {
        recovery_log(bus,e,"POWER");section_off(bus);observed.generation++;
        if(pause_ms(SECTION_SETTLE_MS) && power_on(bus)==0) {
            if(set_once(bus,e,value)){resp_ok(c);return;}
            failed_read=last_read;
        }
    }
    /* Once recovery is exhausted no potentially reset neighbour is claimed known. */
    section_off(bus);
    if(aborted){resp_err(c,ERR_ABORTED,NULL);return;}
    char field[40],readback[12];
    if(failed_read<0)strcpy(readback,"?");else snprintf(readback,sizeof readback,"%d",failed_read);
    snprintf(field,sizeof field,"R%u,%s,%s",channel+1,elements[e],readback);
    resp_err(c,ERR_I2C_FAIL,field);
}
bool section_test(unsigned bus) {
    if(!desired.section_on[bus])return true;
    operation_begin();
    unsigned control;uint8_t dir;
    if(!ad_read(bus,8,&control) || (control&7u)!=2u || !reg_read(bus,0x27,0,&dir) || dir)return false;
    for(unsigned e=0;e<5;e++) {
        unsigned got;uint8_t data[2],reg;
        if(e==0 || e==2) {
            reg=0x0c;if(!transfer(bus,addresses[e],&reg,1,data,2))return false;
            got=((unsigned)(data[0]&3u)<<8)|data[1];
        } else if(e==3) {if(!ad_read(bus,2,&got))return false;}
        else {if(!reg_read(bus,0x27,9,data))return false;got=(data[0]>>(e==1?0:5))&limits[e];}
        if(desired.section_code[bus][e]<0 || got!=(unsigned)desired.section_code[bus][e])return false;
    }
    return true;
}
void cmd_res_stat(struct cmdctx *c) {
    resp_printf(c,"A:%s;B:%s",desired.section_on[0]?"ON":"OFF",desired.section_on[1]?"ON":"OFF");
    for(unsigned bus=0;bus<2;bus++)for(unsigned e=0;e<5;e++) {
        resp_printf(c,";R%u,%s,",bus*2+(e<2?1:2),elements[e]);
        if(!desired.section_on[bus] || desired.section_code[bus][e]<0)resp_putc(c,'?');
        else resp_printf(c,"%d",desired.section_code[bus][e]);
    }
    resp_end(c);
}
