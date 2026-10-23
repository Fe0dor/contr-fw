#include "check.h"
#include "signals.h"
int main(void) {
 device_boot();
 host_gpio_set_input(signal_table[SIG_PWROK1].port,signal_table[SIG_PWROK1].pin,true);
 host_gpio_set_input(signal_table[SIG_PWROK2].port,signal_table[SIG_PWROK2].pin,false);
 host_gpio_set_input(signal_table[SIG_PWROK3].port,signal_table[SIG_PWROK3].pin,true);
 CHECK(strstr(con_cmd("DUT:STAT?"),"PWROK,0,1,0"));
 CHECK_STR(con_cmd("DUT:SET C1,ON"),"OK");
 return test_result("dut_ready");
}
