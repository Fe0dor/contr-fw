/* Образ цепочек сдвиговых регистров (FW-122, FW-123, FW-220). Образ живёт в desired.sr_image. */
#ifndef CORE_SR_H
#define CORE_SR_H

#include <stdbool.h>
#include <stdint.h>

#define SR_CHAINS 2
#define SR_REGS(chain) ((chain) == 0 ? 3 : 4)

/* Биты служебных выходов SR0 (регистр 2): CS_LOAD — B, SR_TEST_OUT — E. */
#define SR0_CS_LOAD_REG 2
#define SR0_CS_LOAD_BIT 1
#define SR0_TEST_OUT_REG 2
#define SR0_TEST_OUT_BIT 4

/* Начальный образ: реле выключены, CS_LOAD неактивен, SR_TEST_OUT = 0 (FW-123). */
void sr_image_initial(uint8_t image[2][4]);

/* Порядок старта FW-220 с начальным образом в desired. */
void sr_start(void);

/* Полная перезапись цепочки из desired.sr_image (FW-122). */
void sr_write(uint8_t chain);

/* Бит выхода в образе desired (без записи в железо). */
void sr_image_set(uint8_t chain, uint8_t reg, uint8_t bit, bool value);
bool sr_image_get(uint8_t chain, uint8_t reg, uint8_t bit);

#endif /* CORE_SR_H */
