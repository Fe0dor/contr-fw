/* FW-132..143. Setpoints are millivolts; no floating-point parser/formatter. */
#include "psu.h"
#include <ctype.h>
#include <string.h>
#include "cfg.h"
#include "update.h"
#include "config.h"
#include "core.h"
#include "errlog.h"
#include "hal.h"
#include "signals.h"
#include "state.h"

static struct cfg_psu_cal calibration;
static bool cal_valid, service_mode, raw_ready;
static uint16_t minimum_code(void){return cal_valid?calibration.code[0]:PSU_DAC_MIN;}
static uint32_t voltage_mv;
static enum err_code fault;
static bool starting, dac_known;

bool psu_dcok(void) { return signal_read(SIG_RSP_DC_OK) == (DCOK_ACTIVE_HIGH != 0); }
bool psu_alarm(void) { return signal_read(SIG_RSP_ALARM) == (ALARM_ACTIVE_HIGH != 0); }
static void rsp_off(void) { signal_set(SIG_PWRON_RSP,false); desired.psu_rsp_on=false; starting=false; }
static void trip(enum err_code code,const char *detail)
{
    rsp_off(); fault=code;
    err_push(code,detail); log_event("RSP %s %s",cmd_err_name(code),detail);
}
static bool dac_write(uint16_t code)
{
    if (!hal_psu_dac_write(code)) {dac_known=false; return false;}
    desired.psu_dac=code; dac_known=true; return true;
}
bool psu_init(void)
{
    cal_valid=cfg_get_psu_cal(&calibration);service_mode=raw_ready=false;
    starting=false;fault=ERR_NONE;dac_known=false;
    rsp_off();signal_set(SIG_HLG,false);desired.psu_hlg_on=false;
    service_mode=raw_ready=false;
    voltage_mv=PSU_VMIN*1000u;
    if (!dac_write(minimum_code())) {trip(ERR_PSU_FAULT,"DAC init");return false;}
    return true;
}
void psu_poll(void)
{
    if (!desired.psu_rsp_on) return;
    if (psu_alarm()) trip(ERR_PSU_ALARM,"ALARM");
    else if (!starting && !psu_dcok()) trip(ERR_PSU_FAULT,"DCOK lost");
}
bool psu_safe_off(void)
{
    rsp_off();signal_set(SIG_HLG,false);desired.psu_hlg_on=false;
    service_mode=raw_ready=false;
    voltage_mv=PSU_VMIN*1000u;
    if (!dac_write(minimum_code())) {trip(ERR_PSU_FAULT,"DAC safe");return false;}
    return true;
}
bool psu_wait_dcok_off(void)
{
    uint32_t start=hal_millis();
    while (psu_dcok()) {
        svc_poll(); /* SAFE must finish even if another SAFE arrives. */
        if ((uint32_t)(hal_millis()-start)>=DCOK_FALL_TIMEOUT_MS) {
            trip(ERR_PSU_FAULT,"DCOK stuck");return false;
        }
    }
    return true;
}
static bool eq(const char *a,const char *b)
{
    while (*a && *b && toupper((unsigned char)*a)==*b) {a++;b++;}
    return !*a && !*b;
}
static bool parse_voltage(const char *s,uint32_t *out)
{
    uint32_t whole=0,frac=0,scale=100;
    if (!isdigit((unsigned char)*s)) return false;
    while (isdigit((unsigned char)*s)) {whole=whole*10u+(unsigned)(*s++-'0');if(whole>PSU_VMAX)return false;}
    if (*s=='.') {
        s++;if(!isdigit((unsigned char)*s))return false;
        while(isdigit((unsigned char)*s)) {if(!scale)return false;frac+=(unsigned)(*s++-'0')*scale;scale/=10;}
    }
    *out=whole*1000u+frac;
    return !*s && *out>=PSU_VMIN*1000u && *out<=PSU_VMAX*1000u;
}
void cmd_psu_volt(struct cmdctx *c)
{
    uint32_t mv;
    if (!parse_voltage(c->argv[0],&mv)) {resp_err(c,ERR_RANGE,"40..54");return;}
    if(service_mode){resp_err(c,ERR_CAL_MODE,NULL);return;}
    if(!cal_valid){resp_err(c,ERR_CAL_REQUIRED,NULL);return;}
    unsigned idx=(mv-PSU_VMIN*1000u)/1000u, frac=mv%1000u;
    uint16_t code=calibration.code[idx];
    if(frac)code=(uint16_t)(code+((uint32_t)(calibration.code[idx+1]-code)*frac+500u)/1000u);
    if (!dac_write(code)) {rsp_off();fault=ERR_PSU_FAULT;resp_err(c,ERR_PSU_FAULT,"DAC");return;}
    voltage_mv=mv;resp_ok(c);
}
void cmd_psu_outp(struct cmdctx *c)
{
    bool rsp=eq(c->argv[0],"RSP"),on=eq(c->argv[1],"ON");
    if ((!rsp&&!eq(c->argv[0],"HLG")) || (!on&&!eq(c->argv[1],"OFF"))) {resp_err(c,ERR_RANGE,NULL);return;}
    if (!rsp) {signal_set(SIG_HLG,on);desired.psu_hlg_on=on;resp_ok(c);return;}
    if (!on) {rsp_off();resp_ok(c);return;}
    if(service_mode?!raw_ready:!cal_valid){rsp_off();resp_err(c,ERR_CAL_REQUIRED,NULL);return;}
    if (psu_alarm()) {rsp_off();fault=ERR_PSU_ALARM;resp_err(c,ERR_PSU_ALARM,NULL);return;}
    if (desired.psu_rsp_on && psu_dcok()) {resp_ok(c);return;}
    if (!dac_known || !dac_write(desired.psu_dac)) {rsp_off();fault=ERR_PSU_FAULT;resp_err(c,ERR_PSU_FAULT,"DAC");return;}
    fault=ERR_NONE;starting=true;desired.psu_rsp_on=true;
    signal_set(SIG_PWRON_RSP,true);
    uint32_t start=hal_millis();
    for (;;) {
        svc_poll();
        if (core_abort_requested()) {rsp_off();resp_err(c,ERR_ABORTED,NULL);return;}
        if (!desired.psu_rsp_on) {resp_err(c,fault,NULL);return;}
        if (psu_dcok()) {starting=false;resp_ok(c);return;}
        if ((uint32_t)(hal_millis()-start)>=DCOK_RISE_TIMEOUT_MS) {
            rsp_off();fault=ERR_PSU_FAULT;resp_err(c,ERR_PSU_FAULT,"DCOK timeout");return;
        }
    }
}
void cmd_psu_stat(struct cmdctx *c)
{
    resp_puts(c,desired.psu_rsp_on?"ON":"OFF");
    if(fault!=ERR_NONE)resp_printf(c,":%s",cmd_err_name(fault));
    resp_printf(c,",%s,%lu",desired.psu_hlg_on?"ON":"OFF",(unsigned long)(voltage_mv/1000));
    if(voltage_mv%1000)resp_printf(c,".%03lu",(unsigned long)(voltage_mv%1000));
    if(dac_known)resp_printf(c,",%u",desired.psu_dac);else resp_puts(c,",?");
    resp_printf(c,",%u,%u",psu_dcok()?1u:0u,psu_alarm()?1u:0u);resp_end(c);
}

static bool cal_idle(struct cmdctx *c,bool need_safe) {
    if(upd_get_state()==UPD_TRIAL || upd_get_state()==UPD_RECEIVING){resp_err(c,ERR_BUSY,"update");return false;}
    if(need_safe && (!observed.in_safe_state || !observed.safe.complete || service_mode)) {
        resp_err(c,ERR_NOT_SAFE,NULL);return false;
    }
    return true;
}
static bool parse_code(const char *s,uint16_t *out) {
    if(!*s)return false;
    unsigned v=0;
    for(;*s;s++){if(*s<'0'||*s>'9')return false;v=v*10u+(unsigned)(*s-'0');if(v>65535u)return false;}
    *out=(uint16_t)v;return true;
}
static bool apply_cal(struct cmdctx *c,const struct cfg_psu_cal *next) {
    if(!dac_write(next->code[0])){rsp_off();fault=ERR_PSU_FAULT;observed.in_safe_state=false;resp_err(c,ERR_PSU_FAULT,"DAC");return false;}
    calibration=*next;cal_valid=true;voltage_mv=PSU_VMIN*1000u;return true;
}
void cmd_psu_cal_stat(struct cmdctx *c) {
    struct cfg_psu_cal saved;bool exists=cfg_get_psu_cal(&saved);
    const char *state=!cal_valid?"NONE":exists&&!memcmp(&saved,&calibration,sizeof saved)?"SAVED":"RAM";
    resp_printf(c,"1,40,54,%s,%s",state,service_mode?"ON":"OFF");
    for(unsigned i=0;i<PSU_CAL_POINTS;i++){if(cal_valid)resp_printf(c,",%u",calibration.code[i]);else resp_puts(c,",?");}
    resp_end(c);
}
void cmd_psu_cal_set(struct cmdctx *c) {
    struct cfg_psu_cal next;
    for(unsigned i=0;i<PSU_CAL_POINTS;i++)if(!parse_code(c->argv[i],&next.code[i])){resp_err(c,ERR_RANGE,"cal code");return;}
    if(!cfg_psu_cal_valid(&next)){resp_err(c,ERR_RANGE,"cal monotonic");return;}
    if(!cal_idle(c,true)||!apply_cal(c,&next))return;
    resp_ok(c);
}
void cmd_psu_cal_save(struct cmdctx *c) {
    if(!cal_idle(c,true))return;
    if(!cal_valid){resp_err(c,ERR_CAL_REQUIRED,NULL);return;}
    struct cfg_psu_cal saved;
    if(cfg_get_psu_cal(&saved)&&!memcmp(&saved,&calibration,sizeof saved)){resp_ok(c);return;}
    cfg_set_erase_allowed(true);
    if(cfg_set_psu_cal(&calibration)!=0 || !cfg_get_psu_cal(&saved) || memcmp(&saved,&calibration,sizeof saved)) {
        resp_err(c,ERR_CAL_STORAGE,NULL);return;
    }
    resp_ok(c);
}
void cmd_psu_cal_load(struct cmdctx *c) {
    if(!cal_idle(c,true))return;
    struct cfg_psu_cal saved;
    if(!cfg_get_psu_cal(&saved)){resp_err(c,ERR_CAL_REQUIRED,NULL);return;}
    if(apply_cal(c,&saved))resp_ok(c);
}
void cmd_psu_cal_mode(struct cmdctx *c) {
    bool on=eq(c->argv[0],"ON");
    if(!on&&!eq(c->argv[0],"OFF")){resp_err(c,ERR_RANGE,NULL);return;}
    if(!cal_idle(c,false))return;
    rsp_off();service_mode=on;raw_ready=false;observed.in_safe_state=false;
    if(!on)voltage_mv=PSU_VMIN*1000u;
    if(!dac_write(minimum_code())){fault=ERR_PSU_FAULT;resp_err(c,ERR_PSU_FAULT,"DAC");return;}
    resp_ok(c);
}
void cmd_psu_cal_code(struct cmdctx *c) {
    uint16_t code;if(!parse_code(c->argv[0],&code)){resp_err(c,ERR_RANGE,"code");return;}
    if(!cal_idle(c,false))return;
    if(!service_mode){resp_err(c,ERR_CAL_MODE,NULL);return;}
    if(!dac_write(code)){rsp_off();raw_ready=false;fault=ERR_PSU_FAULT;resp_err(c,ERR_PSU_FAULT,"DAC");return;}
    raw_ready=true;resp_ok(c);
}
