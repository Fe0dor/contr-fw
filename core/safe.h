/* Безопасное состояние одной функцией (А6, FW-200…FW-208). Шаги 1…6 выполняются
 * функциями модулей; на шаге 2 плана модули БП и нагрузок — заглушки, реле, линии и
 * секции выключаются по-настоящему. */
#ifndef CORE_SAFE_H
#define CORE_SAFE_H

#include "state.h"

/* Вход по причине; результат в observed.safe. Возвращает true, если все шаги выполнены. */
bool safe_enter(enum safe_reason reason);

const char *safe_reason_name(enum safe_reason r);
const char *safe_step_name(enum safe_step_result r);

#endif /* CORE_SAFE_H */
