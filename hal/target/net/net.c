/* Сеть: LwIP raw API, NO_SYS=1. netif над eth.c, TCP-сервер на порту протокола,
 * один активный клиент, второе соединение — ERR:BUSY и закрытие (FW-102), keepalive с
 * константами раздела 5 (FW-218), DHCP с возвратом к стандартному адресу (FW-228).
 * Колбэки LwIP только кладут данные ядру через core_net_on_* и возвращаются (А2). */
#include <string.h>

#include "eth.h"
#include "hal.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif netif;
static struct hal_net_config cfg;
static struct tcp_pcb *listen_pcb;
static struct tcp_pcb *client;
static bool client_up;
static uint32_t last_link_poll_ms;
static bool eth_ok;

/* ---- netif ---- */

static err_t low_level_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    static uint8_t frame[1536];
    if (p->tot_len > sizeof frame) {
        return ERR_BUF;
    }
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return eth_send(frame, p->tot_len) ? ERR_OK : ERR_WOULDBLOCK;
}

static err_t netif_init_cb(struct netif *n)
{
    n->name[0] = 'e';
    n->name[1] = 'n';
    n->output = etharp_output;
    n->linkoutput = low_level_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, cfg.mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void rx_pump(void)
{
    const uint8_t *frame;
    size_t len;
    while ((len = eth_receive(&frame)) != 0) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p) {
            pbuf_take(p, frame, (u16_t)len);
            if (netif.input(p, &netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
        eth_receive_done();
    }
}

/* ---- TCP ---- */

static void client_gone(void)
{
    if (client) {
        tcp_arg(client, NULL);
        tcp_recv(client, NULL);
        tcp_err(client, NULL);
        tcp_sent(client, NULL);
        if (tcp_close(client) != ERR_OK) {
            tcp_abort(client);
        }
        client = NULL;
    }
    if (client_up) {
        client_up = false;
        core_net_on_closed();
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)err;
    if (p == NULL) { /* FIN */
        if (pcb == client) {
            client_gone();
        } else {
            tcp_close(pcb);
        }
        return ERR_OK;
    }
    if (pcb == client) {
        for (struct pbuf *q = p; q; q = q->next) {
            core_net_on_data((const uint8_t *)q->payload, q->len);
        }
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    /* pcb уже освобождён LwIP */
    if (client) {
        client = NULL;
    }
    if (client_up) {
        client_up = false;
        core_net_on_closed();
    }
}

static err_t busy_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)len;
    tcp_sent(pcb, NULL);
    tcp_recv(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL) {
        return ERR_VAL;
    }
    tcp_setprio(pcb, TCP_PRIO_NORMAL);
    if (!core_net_on_accept()) {
        /* второй клиент: ERR:BUSY без префикса и закрытие без побочных эффектов */
        static const char busy[] = "ERR:BUSY\n";
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, busy_sent);
        tcp_recv(pcb, on_recv);
        if (tcp_write(pcb, busy, sizeof busy - 1, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(pcb);
        } else {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    client = pcb;
    client_up = true;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = cfg.keepalive_idle_s * 1000u;
    pcb->keep_intvl = cfg.keepalive_intvl_s * 1000u;
    pcb->keep_cnt = cfg.keepalive_cnt;
    return ERR_OK;
}

size_t hal_net_send(const uint8_t *buf, size_t len)
{
    if (!client) {
        return 0;
    }
    size_t room = tcp_sndbuf(client);
    if (len > room) {
        len = room;
    }
    if (len == 0) {
        return 0;
    }
    if (tcp_write(client, buf, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return 0;
    }
    tcp_output(client);
    return len;
}

void hal_net_close_client(void)
{
    client_gone();
}

bool hal_net_client_connected(void)
{
    return client_up;
}

/* ---- адрес ---- */

static void apply_static(uint32_t addr, uint32_t mask, uint32_t gw)
{
    ip4_addr_t a, m, g;
    ip4_addr_set_u32(&a, lwip_htonl(addr));
    ip4_addr_set_u32(&m, lwip_htonl(mask));
    ip4_addr_set_u32(&g, lwip_htonl(gw));
    netif_set_addr(&netif, &a, &m, &g);
    /* With DHCP ACD enabled, LwIP does not announce static addresses. */
    if (netif_is_link_up(&netif)) {
        etharp_gratuitous(&netif);
    }
}

void hal_net_init(const struct hal_net_config *c)
{
    cfg = *c;
    lwip_init();
    eth_ok = eth_init(cfg.mac);
    ip4_addr_t zero;
    ip4_addr_set_zero(&zero);
    netif_add(&netif, &zero, &zero, &zero, NULL, netif_init_cb, ethernet_input);
    netif_set_default(&netif);
    netif_set_up(&netif);
    if (cfg.dhcp) {
        dhcp_start(&netif);
    } else {
        apply_static(cfg.addr, cfg.mask, cfg.gw);
    }
    listen_pcb = tcp_new();
    if (listen_pcb) {
        tcp_bind(listen_pcb, IP_ADDR_ANY, cfg.port);
        listen_pcb = tcp_listen_with_backlog(listen_pcb, 1);
        tcp_accept(listen_pcb, on_accept);
    }
}

void hal_net_set_static(uint32_t addr, uint32_t mask, uint32_t gw)
{
    if (cfg.dhcp) {
        dhcp_stop(&netif);
    }
    apply_static(addr, mask, gw);
}

void hal_net_poll(void)
{
    if (!eth_ok) {
        return;
    }
    uint32_t now = hal_millis();
    if ((uint32_t)(now - last_link_poll_ms) >= 200u) {
        last_link_poll_ms = now;
        bool up = eth_poll_link();
        if (up != (netif_is_link_up(&netif) != 0)) {
            if (up) {
                netif_set_link_up(&netif);
                if (!ip4_addr_isany_val(*netif_ip4_addr(&netif)) &&
                    !hal_net_dhcp_bound()) {
                    etharp_gratuitous(&netif);
                }
            } else {
                netif_set_link_down(&netif);
            }
        }
    }
    rx_pump();
    sys_check_timeouts();
}

bool hal_net_link_up(void)
{
    return eth_ok && eth_link_up();
}

uint32_t hal_net_addr(void)
{
    return lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&netif)));
}

bool hal_net_dhcp_bound(void)
{
    return cfg.dhcp && dhcp_supplied_address(&netif);
}

/* sys_now() для LwIP */
uint32_t sys_now(void)
{
    return hal_millis();
}
