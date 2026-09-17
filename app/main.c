/* Точка входа прошивки CONTR (А2): board_early_init() — первые строки main(), затем
 * инициализация и суперцикл core_run(). */
#include "core.h"

int main(void)
{
    board_early_init();
    core_init();
    core_run();
}
