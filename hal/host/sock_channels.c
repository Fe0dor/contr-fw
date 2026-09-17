/* Каналы исполняемого файла contr-host: настоящий TCP-сервер на порту протокола и консоль
 * через stdin/stdout. Время — виртуальное (stubs.c), но идёт от часов хоста; строка
 * "#advance <мс>" на любом канале продвигает его без ожидания (директива диалогов А13).
 * Второе соединение получает ERR:BUSY и закрывается, как на плате. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#endif

#include "hal_host.h"

static struct {
    sock_t listen_fd, client_fd;
    bool client_up;
    struct hal_net_config cfg;
    bool initialized;
    uint32_t static_addr;
    uint64_t last_real_ms;
    char con_partial[512];
    size_t con_partial_len;
    char net_partial[512];
    size_t net_partial_len;
} s = {.listen_fd = SOCK_INVALID, .client_fd = SOCK_INVALID};

static uint64_t real_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void set_nonblocking(sock_t fd)
{
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(fd, FIONBIO, &one);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void host_mem_channels_reset(void) {}

/* ---- директива #advance в потоке байтов: вырезается до ядра ---- */

static void feed(const uint8_t *buf, size_t len, char *partial, size_t *plen, void (*deliver)(const uint8_t *, size_t))
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (*plen < sizeof s.con_partial - 1) {
            partial[(*plen)++] = c;
        }
        if (c == '\n') {
            partial[*plen] = '\0';
            if (partial[0] == '#') {
                unsigned long ms = 0;
                if (sscanf(partial, "#advance %lu", &ms) == 1) {
                    host_advance_ms((uint32_t)ms);
                }
            } else {
                deliver((const uint8_t *)partial, *plen);
            }
            *plen = 0;
        }
    }
}

/* ---- консоль ---- */

static uint8_t con_buf[4096];
static size_t con_len;

static void con_deliver(const uint8_t *buf, size_t len)
{
    size_t n = len > sizeof con_buf - con_len ? sizeof con_buf - con_len : len;
    memcpy(con_buf + con_len, buf, n);
    con_len += n;
}

static void console_pump(void)
{
    /* stdin в неблокирующем режиме на обеих ОС непросто: читаем по строке, если есть данные */
#ifdef _WIN32
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (GetFileType(hin) == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(hin, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            return;
        }
    } else {
        return; /* интерактивная консоль Windows: без опроса, чтобы не блокировать суперцикл */
    }
#else
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    if (select(1, &fds, NULL, NULL, &tv) <= 0) {
        return;
    }
#endif
    char line[512];
    if (fgets(line, sizeof line, stdin)) {
        feed((const uint8_t *)line, strlen(line), s.con_partial, &s.con_partial_len, con_deliver);
    }
}

size_t hal_console_read(uint8_t *buf, size_t max)
{
    size_t n = con_len > max ? max : con_len;
    memcpy(buf, con_buf, n);
    memmove(con_buf, con_buf + n, con_len - n);
    con_len -= n;
    return n;
}

size_t hal_console_write(const uint8_t *buf, size_t len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return len;
}

/* ---- сеть ---- */

void hal_net_init(const struct hal_net_config *cfg)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    s.cfg = *cfg;
    s.initialized = true;
    s.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s.listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const char *env = getenv("CONTR_HOST_PORT");
    a.sin_port = htons(env ? (uint16_t)atoi(env) : cfg->port);
    if (bind(s.listen_fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s.listen_fd, 2) != 0) {
        fprintf(stderr, "contr-host: bind %u failed\n", (unsigned)ntohs(a.sin_port));
        exit(2);
    }
    set_nonblocking(s.listen_fd);
    fprintf(stderr, "contr-host: listening 127.0.0.1:%u\n", (unsigned)ntohs(a.sin_port));
    s.last_real_ms = real_ms();
}

static void net_deliver(const uint8_t *buf, size_t len) { core_net_on_data(buf, len); }

static void close_client(void)
{
    if (s.client_fd != SOCK_INVALID) {
        sock_close(s.client_fd);
        s.client_fd = SOCK_INVALID;
    }
    if (s.client_up) {
        s.client_up = false;
        core_net_on_closed();
    }
}

void hal_net_poll(void)
{
    if (!s.initialized) {
        return;
    }
    uint64_t now = real_ms();
    if (now > s.last_real_ms) {
        host_advance_ms((uint32_t)(now - s.last_real_ms));
        s.last_real_ms = now;
    }
    console_pump();
    sock_t fd = accept(s.listen_fd, NULL, NULL);
    if (fd != SOCK_INVALID) {
        if (!core_net_on_accept()) {
            static const char busy[] = "ERR:BUSY\n";
            send(fd, busy, sizeof busy - 1, 0);
            sock_close(fd);
        } else {
            set_nonblocking(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            s.client_fd = fd;
            s.client_up = true;
            s.net_partial_len = 0;
        }
    }
    if (s.client_fd != SOCK_INVALID) {
        uint8_t buf[1024];
#ifdef _WIN32
        int n = recv(s.client_fd, (char *)buf, sizeof buf, 0);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            close_client();
        } else if (n > 0) {
            feed(buf, (size_t)n, s.net_partial, &s.net_partial_len, net_deliver);
        }
#else
        ssize_t n = recv(s.client_fd, buf, sizeof buf, 0);
        if (n == 0 || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
            close_client();
        } else if (n > 0) {
            feed(buf, (size_t)n, s.net_partial, &s.net_partial_len, net_deliver);
        }
#endif
    }
}

size_t hal_net_send(const uint8_t *buf, size_t len)
{
    if (s.client_fd == SOCK_INVALID) {
        return 0;
    }
    int n = send(s.client_fd, (const char *)buf, (int)len, 0);
    return n > 0 ? (size_t)n : 0;
}

void hal_net_close_client(void) { close_client(); }
bool hal_net_client_connected(void) { return s.client_up; }
bool hal_net_link_up(void) { return true; }
uint32_t hal_net_addr(void) { return s.cfg.dhcp ? s.static_addr : s.cfg.addr; }
bool hal_net_dhcp_bound(void) { return false; }
void hal_net_set_static(uint32_t addr, uint32_t mask, uint32_t gw) { (void)mask; (void)gw; s.static_addr = addr; }

/* заглушки интерфейса тестов, которые contr-host не использует */
bool host_net_connect(void) { return false; }
void host_net_push(const uint8_t *buf, size_t len) { (void)buf; (void)len; }
void host_net_push_line(const char *line) { (void)line; }
size_t host_net_take(uint8_t *buf, size_t max) { (void)buf; (void)max; return 0; }
bool host_net_take_line(char *out, size_t max) { (void)out; (void)max; return false; }
void host_net_disconnect(void) {}
bool host_net_device_closed(void) { return false; }
void host_net_set_link(bool up) { (void)up; }
void host_net_set_dhcp_bound(bool bound) { (void)bound; }
uint32_t host_net_static_addr(void) { return s.static_addr; }
bool host_net_initialized(void) { return s.initialized; }
void host_net_set_send_limit(size_t limit) { (void)limit; }
void host_console_push(const uint8_t *buf, size_t len) { con_deliver(buf, len); }
void host_console_push_line(const char *line) { con_deliver((const uint8_t *)line, strlen(line)); con_deliver((const uint8_t *)"\n", 1); }
size_t host_console_take(uint8_t *buf, size_t max) { (void)buf; (void)max; return 0; }
bool host_console_take_line(char *out, size_t max) { (void)out; (void)max; return false; }
