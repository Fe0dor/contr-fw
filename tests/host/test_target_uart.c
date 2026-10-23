#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define HAL_TARGET_H
#define hal_console_init target_hal_console_init
#define hal_console_read target_hal_console_read
#define hal_console_write target_hal_console_write
struct uart_regs { uint32_t ISR,RDR,ICR,BRR,CR1,CR2,CR3,TDR; } uart;
struct rcc_regs {uint32_t APB1ENR;} rcc;
#define USART3 (&uart)
#define RCC (&rcc)
#define USART_ISR_RXNE 1u
#define USART_ISR_ORE 2u
#define USART_ISR_FE 4u
#define USART_ISR_NE 8u
#define USART_ISR_TXE 16u
#define USART_ICR_ORECF 2u
#define USART_ICR_FECF 4u
#define USART_ICR_NCF 8u
#define RCC_APB1ENR_USART3EN 1u
#define USART_CR1_TE 1u
#define USART_CR1_RE 2u
#define USART_CR1_RXNEIE 4u
#define USART_CR1_UE 8u
#define USART3_IRQn 0
#define PCLK1_HZ 54000000u
void target_gpio_af(uint8_t a,uint8_t b,uint8_t c,bool d,bool e){(void)a;(void)b;(void)c;(void)d;(void)e;}
void NVIC_SetPriority(int a,int b){(void)a;(void)b;}
void NVIC_EnableIRQ(int a){(void)a;}
static uint32_t irq_mask;
static uint32_t __get_PRIMASK(void){return irq_mask;}
static void __disable_irq(void){irq_mask=1;}
static void __set_PRIMASK(uint32_t mask){irq_mask=mask;}
#include "../../hal/target/console.c"
#undef hal_console_init
#undef hal_console_read
#undef hal_console_write
#include "check.h"

static void receive(uint8_t byte,uint32_t flags){uart.ISR=flags;uart.RDR=byte;USART3_IRQHandler();}
static void bytes(const char *s){while(*s)receive((uint8_t)*s++,USART_ISR_RXNE);}
static void tick(void){host_advance_ms(1);}
static void drain(void){uint8_t b[512];size_t n=target_hal_console_read(b,sizeof b);host_console_push(b,n);device_run(700);}
static void boot(void){device_boot();target_hal_console_init();host_net_set_poll_hook(tick);}
static void rejected(void){
    CHECK(host_console_take_line(reply_buf,sizeof reply_buf));CHECK_STR(reply_buf,"ERR:RANGE");
    CHECK_STR(con_cmd("ROUT:STAT?"),"");
    bytes("ROUT:HIGH M4\n");drain();
    CHECK(host_console_take_line(reply_buf,sizeof reply_buf));CHECK_STR(reply_buf,"OK");
    CHECK_STR(con_cmd("ROUT:STAT?"),"M4");
}

static void drain_all(size_t chunk) {
    uint8_t b[64];size_t n;
    while((n=target_hal_console_read(b,chunk))!=0) {host_console_push(b,n);device_run(2);}
    device_run(700);
    while(host_console_take_line(reply_buf,sizeof reply_buf)) {}
}
static void safe_overflow(void) {
    for(unsigned scenario=0;scenario<6;scenario++) {
        boot();CHECK_STR(con_cmd("PSU:OUTP HLG,ON"),"OK");uint32_t seq=observed.safe.seq;
        if(scenario==0)bytes("SAFE\n");
        if(scenario==1) {for(unsigned i=0;i<507;i++)bytes("\n");bytes("SAFE\n");}
        if(scenario==2) {for(unsigned i=0;i<511;i++)bytes("\n");bytes("sAfE\n");}
        if(scenario==3) {bytes("SA");drain_all(1);bytes("FE\n");}
        for(unsigned i=0;i<100;i++)bytes("*IDN?\n");
        if(scenario==4)bytes(" \tsAfE\r \t\n");
        if(scenario==5) {bytes("SAFE\n");receive('x',USART_ISR_RXNE|USART_ISR_FE);bytes("junk\n");}
        drain_all(scenario%2?1:64);
        CHECK(observed.safe.seq==seq+1);CHECK(!desired.psu_hlg_on);
    }
    /* Ordinary partial input already in core cannot swallow synthetic SAFE. */
    boot();CHECK_STR(con_cmd("PSU:OUTP HLG,ON"),"OK");bytes("GARBAGE");drain_all(1);
    bytes("\nSAFE\n");for(unsigned i=0;i<100;i++)bytes("*IDN?\n");drain_all(1);
    CHECK(observed.safe.seq==1&&!desired.psu_hlg_on);
    /* More than one discarded SAFE must not duplicate or lose acknowledgments. */
    boot();bytes("SAFE\nSAFE\n");for(unsigned i=0;i<250;i++)bytes("*IDN?\n");drain_all(64);
    CHECK(observed.safe.seq==2);
}
static void no_false_safe(void) {
    const uint32_t errors[]={USART_ISR_ORE,USART_ISR_FE,USART_ISR_NE};
    for(unsigned i=0;i<3;i++) {
        boot();bytes("SAFE\n");receive('x',USART_ISR_RXNE|errors[i]);bytes("junk\n");drain_all(1);
        CHECK(observed.safe.seq==1); /* earlier complete SAFE survives UART error */
        boot();bytes("SA");drain_all(1);receive('F',USART_ISR_RXNE|errors[i]);bytes("E\n");
        for(unsigned j=0;j<100;j++) {bytes("*IDN?\n");}
        drain_all(64);CHECK(observed.safe.seq==0);
    }
    const char *bad[]={"SAFE junk\n","SAFE\tX\n","SA FE\n"};
    for(unsigned i=0;i<3;i++) {boot();bytes(bad[i]);for(unsigned j=0;j<100;j++)bytes("*IDN?\n");drain_all(1);CHECK(observed.safe.seq==0);}
    boot();bytes("SAFE");receive(0,USART_ISR_RXNE);bytes("\n");for(unsigned j=0;j<100;j++) {bytes("*IDN?\n");}
        drain_all(64);CHECK(observed.safe.seq==0);
    boot();for(unsigned j=0;j<CMD_LINE_MAX;j++)bytes(" ");bytes("SAFE\n");drain_all(1);CHECK(observed.safe.seq==0);
}
int main(void){
    boot();
    for(unsigned i=0;i<499;i++)receive('\n',USART_ISR_RXNE);
    bytes("ROUT:HIGH M4,M5");drain();bytes("\n");drain();rejected();
    /* Error after a prefix has already reached core must invalidate that prefix. */
    const uint32_t errors[]={USART_ISR_ORE,USART_ISR_FE,USART_ISR_NE};
    for(unsigned i=0;i<3;i++){
        boot();bytes("ROUT:HIGH M4");drain();
        receive(',',USART_ISR_RXNE|errors[i]);bytes("M5\n");drain();rejected();
    }
    boot();bytes("ROUT:HIGH M4");drain();receive(0,USART_ISR_ORE);
    bytes(",M5\n");drain();rejected();
    /* Preserve the caller's interrupt mask, including an empty read. */
    uint8_t b;irq_mask=1;target_hal_console_read(&b,1);CHECK(irq_mask==1);
    irq_mask=0;target_hal_console_read(&b,1);CHECK(irq_mask==0);
    safe_overflow();no_false_safe();
    return test_result("target_uart");
}
