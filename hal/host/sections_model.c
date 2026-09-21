/* Register-level models; whitelist rejects every nonvolatile programming frame. */
#include "hal_host.h"
#include <string.h>
struct chip {unsigned value, control, selected;enum host_section_fault fault;unsigned remaining;};
static struct {bool on;bool reset[2];struct chip chips[4];uint8_t dir;} buses[2];
static unsigned forbidden;
static int index_of(uint8_t addr) {
    switch(addr){case 0x2f:return 0;case 0x2e:return 1;case 0x2c:return 2;case 0x27:return 3;default:return -1;}
}
void host_sections_reset(void) {memset(buses,0,sizeof buses);forbidden=0;}
unsigned host_sections_forbidden(void){return forbidden;}
unsigned host_section_value(unsigned bus,uint8_t addr){int i=index_of(addr);return i<0?0:buses[bus].chips[i].value;}
void host_section_fault(unsigned bus,uint8_t addr,enum host_section_fault fault,unsigned count) {
    int i=index_of(addr);if(bus<2 && i>=0){buses[bus].chips[i].fault=fault;buses[bus].chips[i].remaining=count;}
}
void host_sections_gpio(uint8_t port,uint8_t pin,enum hal_pin_mode mode,bool level) {
    /* rev1 pins, independent of the core driver's signal selection. */
    int bus=-1,reset=-1;
    if(port==2 && pin==7)bus=0;
    if(port==3 && pin==15)bus=1;
    if(bus>=0){
        bool on=mode==HAL_PIN_IN;
        if(buses[bus].on && !on)for(unsigned i=0;i<4;i++) {
            struct chip *c=&buses[bus].chips[i];
            if(c->fault==HOST_SECTION_POWER)c->fault=HOST_SECTION_OK;
        }
        if(!buses[bus].on && on) {
            for(unsigned i=0;i<4;i++){buses[bus].chips[i].value=i<2?128:i==2?512:0;buses[bus].chips[i].control=0;}
            buses[bus].dir=255;
        }
        buses[bus].on=on;return;
    }
    /* Filled from documented rev1 pin addresses below by generator script. */
    if(port==5 && pin==5){bus=0;reset=0;}
    if(port==5 && pin==4){bus=0;reset=1;}
    if(port==3 && pin==11){bus=1;reset=0;}
    if(port==3 && pin==14){bus=1;reset=1;}
    if(bus>=0 && reset>=0) {
        if(!level){
            struct chip *c=&buses[bus].chips[reset==0?2:3];
            if(c->fault==HOST_SECTION_RESET)c->fault=HOST_SECTION_OK;
            c->value=reset==0?512:0;c->control=0;if(reset==1)buses[bus].dir=255;
        }
        buses[bus].reset[reset]=!level;
    }
}
int host_sections_xfer(uint8_t bus,uint8_t addr,const uint8_t *w,size_t wn,uint8_t *r,size_t rn) {
    int i=index_of(addr);
    if(bus>1 || i<0 || !buses[bus].on || (i>=2 && buses[bus].reset[i-2]))return HAL_I2C_NACK;
    struct chip *c=&buses[bus].chips[i];
    if(c->fault==HOST_SECTION_TRANSIENT){if(c->remaining){c->remaining--;return HAL_I2C_NACK;}c->fault=HOST_SECTION_OK;}
    if(c->fault==HOST_SECTION_RESET || c->fault==HOST_SECTION_POWER || c->fault==HOST_SECTION_MISSING)return HAL_I2C_NACK;
    if(!wn && !rn)return HAL_I2C_OK;
    unsigned value=0;
    if(i<2){
        if(wn==2 && rn==0 && w[0]<=1){c->value=((unsigned)w[0]<<8)|w[1];return HAL_I2C_OK;}
        if(wn!=1 || w[0]!=0x0c || rn!=2){forbidden++;return HAL_I2C_BUS_ERROR;}
        value=c->value;
    } else if(i==2) {
        if(wn==2 && rn==0){
            unsigned cmd=w[0]>>2,data=((unsigned)(w[0]&3)<<8)|w[1];c->selected=cmd;
            if(cmd==7 && data==2)c->control=2;
            else if(cmd==1 && (c->control&2))c->value=data;
            else if(cmd!=2 && cmd!=8){forbidden++;return HAL_I2C_BUS_ERROR;}
            return HAL_I2C_OK;
        }
        if(wn || rn!=2 || (c->selected!=2 && c->selected!=8)){forbidden++;return HAL_I2C_BUS_ERROR;}
        value=c->selected==2?c->value:c->control;
    } else {
        if(wn==2 && !rn){
            if(w[0]==0)buses[bus].dir=w[1];
            else if(w[0]==9 || w[0]==10)c->value=w[1];
            else {forbidden++;return HAL_I2C_BUS_ERROR;}
            return HAL_I2C_OK;
        }
        if(wn!=1 || rn!=1 || (w[0]!=0 && w[0]!=9 && w[0]!=10)){forbidden++;return HAL_I2C_BUS_ERROR;}
        value=w[0]==0?buses[bus].dir:c->value;
    }
    if(c->fault==HOST_SECTION_MISMATCH)value^=1;
    if(rn==2){r[0]=(uint8_t)(value>>8);r[1]=(uint8_t)value;}else r[0]=(uint8_t)value;
    return HAL_I2C_OK;
}
