/* contr-host: хост-сборка прошивки как процесс — TCP на 127.0.0.1:5025 (CONTR_HOST_PORT),
 * консоль на stdin/stdout. Тот же core/, что в плате; железо — заглушки hal/host. */
#include <stdio.h>

#include "core.h"
#include "hal_host.h"

int main(void)
{
    host_reset_all();
    board_early_init();
    core_init();
    for (;;) {
        core_step();
        if (host_reset_requested()) {
            fprintf(stderr, "contr-host: reset requested, exiting\n");
            return 3;
        }
    }
}
