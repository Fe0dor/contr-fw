/* Intentional HIL fault: exercise IWDG and the second unconfirmed boot. */
#include "core.h"
int main(void)
{
    board_early_init();
    core_init(); /* records the trial boot before simulating a hung application */
    for (;;) { } /* no service loop and no watchdog refresh */
}
