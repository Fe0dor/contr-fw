/* Каналы в памяти для тестов: консоль и TCP-клиент как кольца байтов. */
#include <string.h>

#include "hal_host.h"

#define RING 16384u

struct ring {
    uint8_t data[RING];
    size_t head, len;
};

static struct {
    struct ring con_in, con_out;  /* терминал → прошивка, прошивка → терминал */
    struct ring net_out;          /* прошивка → клиент */
    bool net_connected;           /* клиент подключён с точки зрения HAL */
    bool net_device_closed;
    bool link_up, dhcp_bound, initialized;
    uint32_t static_addr;
    struct hal_net_config cfg;
    size_t send_limit;
} m;

static size_t push(struct ring *r, const uint8_t *buf, size_t len)
{
    size_t n = 0;
    while (n < len && r->len < RING) {
        r->data[(r->head + r->len) % RING] = buf[n++];
        r->len++;
    }
    return n;
}

static size_t pop(struct ring *r, uint8_t *buf, size_t max)
{
    size_t n = 0;
    while (n < max && r->len > 0) {
        buf[n++] = r->data[r->head];
        r->head = (r->head + 1) % RING;
        r->len--;
    }
    return n;
}

static bool pop_line(struct ring *r, char *out, size_t max)
{
    size_t i;
    for (i = 0; i < r->len; i++) {
        if (r->data[(r->head + i) % RING] == '\n') {
            break;
        }
    }
    if (i == r->len) {
        return false;
    }
    size_t n = 0;
    for (size_t k = 0; k < i; k++) {
        uint8_t b = r->data[(r->head + k) % RING];
        if (n + 1 < max) {
            out[n++] = (char)b;
        }
    }
    out[n] = '\0';
    r->head = (r->head + i + 1) % RING;
    r->len -= i + 1;
    return true;
}

void host_mem_channels_reset(void)
{
    memset(&m, 0, sizeof m);
    m.link_up = true;
}

/* ---- консоль ---- */

void host_console_push(const uint8_t *buf, size_t len) { push(&m.con_in, buf, len); }

void host_console_push_line(const char *line)
{
    push(&m.con_in, (const uint8_t *)line, strlen(line));
    push(&m.con_in, (const uint8_t *)"\n", 1);
}

size_t host_console_take(uint8_t *buf, size_t max) { return pop(&m.con_out, buf, max); }
bool host_console_take_line(char *out, size_t max) { return pop_line(&m.con_out, out, max); }
size_t hal_console_read(uint8_t *buf, size_t max) { return pop(&m.con_in, buf, max); }
size_t hal_console_write(const uint8_t *buf, size_t len) { return push(&m.con_out, buf, len); }

/* ---- сеть ---- */

void hal_net_init(const struct hal_net_config *cfg)
{
    m.cfg = *cfg;
    m.initialized = true;
}

void hal_net_poll(void) {}
bool hal_net_link_up(void) { return m.link_up; }
uint32_t hal_net_addr(void) { return m.cfg.dhcp ? (m.dhcp_bound ? 0x0A000005u : m.static_addr) : m.cfg.addr; }
bool hal_net_dhcp_bound(void) { return m.dhcp_bound; }
void hal_net_set_static(uint32_t addr, uint32_t mask, uint32_t gw) { (void)mask; (void)gw; m.static_addr = addr; }

size_t hal_net_send(const uint8_t *buf, size_t len)
{
    if (!m.net_connected) {
        return 0;
    }
    if (m.send_limit && len > m.send_limit) {
        len = m.send_limit;
    }
    return push(&m.net_out, buf, len);
}

void hal_net_close_client(void)
{
    if (m.net_connected) {
        m.net_connected = false;
        m.net_device_closed = true;
    }
}

bool hal_net_client_connected(void) { return m.net_connected; }

bool host_net_connect(void)
{
    if (!core_net_on_accept()) {
        return false;
    }
    m.net_connected = true;
    m.net_device_closed = false;
    return true;
}

void host_net_push(const uint8_t *buf, size_t len)
{
    if (m.net_connected) {
        core_net_on_data(buf, len);
    }
}

void host_net_push_line(const char *line)
{
    host_net_push((const uint8_t *)line, strlen(line));
    host_net_push((const uint8_t *)"\n", 1);
}

size_t host_net_take(uint8_t *buf, size_t max) { return pop(&m.net_out, buf, max); }
bool host_net_take_line(char *out, size_t max) { return pop_line(&m.net_out, out, max); }

void host_net_disconnect(void)
{
    if (m.net_connected) {
        m.net_connected = false;
        core_net_on_closed();
    }
}

bool host_net_device_closed(void) { return m.net_device_closed; }
void host_net_set_link(bool up) { m.link_up = up; }
void host_net_set_dhcp_bound(bool bound) { m.dhcp_bound = bound; }
uint32_t host_net_static_addr(void) { return m.static_addr; }
bool host_net_initialized(void) { return m.initialized; }
void host_net_set_send_limit(size_t limit) { m.send_limit = limit; }
