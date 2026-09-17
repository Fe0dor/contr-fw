/* Настройки LwIP для CONTR: NO_SYS, raw API, только IPv4, TCP 5025 и DHCP (А14).
 * Динамической памяти после инициализации нет: пулы LwIP статические. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_TIMERS 1
#define LWIP_RAND() ((u32_t)hal_millis() * 2654435761u)

#define MEM_ALIGNMENT 4
#define MEM_SIZE (16 * 1024)
#define MEMP_NUM_PBUF 16
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_SYS_TIMEOUT 8
#define PBUF_POOL_SIZE 16
#define PBUF_POOL_BUFSIZE 1536

#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_DHCP 1
#define LWIP_AUTOIP 0
#define LWIP_DNS 0
#define LWIP_UDP 1 /* нужен DHCP */
#define LWIP_TCP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_IGMP 0
#define LWIP_STATS 0
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_SO_RCVTIMEO 0

#define TCP_MSS 1460
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_WND (4 * TCP_MSS)
#define TCP_QUEUE_OOSEQ 0
#define TCP_LISTEN_BACKLOG 1

#define CHECKSUM_GEN_IP 1
#define CHECKSUM_GEN_UDP 1
#define CHECKSUM_GEN_TCP 1
#define CHECKSUM_GEN_ICMP 1
#define CHECKSUM_CHECK_IP 1
#define CHECKSUM_CHECK_UDP 1
#define CHECKSUM_CHECK_TCP 1
#define CHECKSUM_CHECK_ICMP 1

#define LWIP_DEBUG 0
#define LWIP_NOASSERT 1

#include <stdint.h>
uint32_t hal_millis(void);

#endif /* LWIPOPTS_H */
