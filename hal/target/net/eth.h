/* Драйвер Ethernet MAC STM32F7 + LAN8742A, опрос без прерываний. */
#ifndef HAL_TARGET_ETH_H
#define HAL_TARGET_ETH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool eth_init(const uint8_t mac[6]);
bool eth_poll_link(void);
bool eth_link_up(void);
bool eth_send(const uint8_t *frame, size_t len);
/* Кадр в буфере DMA до вызова eth_receive_done(); 0 — кадров нет. */
size_t eth_receive(const uint8_t **frame);
void eth_receive_done(void);

#endif /* HAL_TARGET_ETH_H */
