#include "check.h"
#include "signal_table.h"
static void tick(void){host_advance_ms(1);}
static void boot(void){device_boot();host_net_set_poll_hook(tick);}
int main(void){
    boot(); /* Pull-ups mean not ready for active-low ready pins. */
    uint32_t start=hal_millis();CHECK_STR(con_cmd("RES:PWR A,ON"),"ERR:SECTION_FAIL,2");
    CHECK(hal_millis()-start<=1000);CHECK(!desired.section_on[0]);
    /* Read pin addresses from the generated board table in this test only. */
    const struct signal_desc *a=&signal_table[SIG_PWROK4],*b=&signal_table[SIG_PWROK5];
    host_gpio_set_input(a->port,a->pin,false);host_gpio_set_input(b->port,b->pin,false);
    CHECK_STR(con_cmd("RES:PWR A,ON"),"OK");CHECK_STR(con_cmd("RES:PWR B,ON"),"OK");
    CHECK(desired.section_on[0]&&desired.section_on[1]);
    return test_result("sections_ready");
}
