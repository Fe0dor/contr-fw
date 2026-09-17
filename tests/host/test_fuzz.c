/* FW-109: произвольный мусор в строке не меняет состояние и не роняет прошивку;
 * каждая строка получает ровно один кадр ответа. Псевдослучайный генератор — LCG. */
#include <ctype.h>
#include <stdlib.h>

#include "check.h"
#include "cmd.h"

static uint32_t seed = 12345;

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:?*,. -_;=/+\t";

static void random_line(char *out, size_t max)
{
    size_t len = rnd() % max;
    for (size_t i = 0; i < len; i++) {
        uint32_t r = rnd();
        out[i] = (r % 7 == 0) ? (char)(r & 0x7F ? r & 0x7F : 'x') : alphabet[r % strlen(alphabet)];
        if (out[i] == '\n' || out[i] == '\0') {
            out[i] = 'x';
        }
    }
    out[len] = '\0';
}

/* мутации настоящих команд: обрезки, лишние аргументы, чужой регистр */
static const char *seeds[] = {
    "*IDN?", "SAFE", "SYST:SAFE?", "SYST:ERR?", "SYST:LOG?", "SYST:CONF?", "SYST:NET?",
    "SYST:PROV:SERIAL X", "SYST:PROV:NET dhcp", "SYST:UPD:BEGIN 1000,00000000,0.3.0", "SYST:UPD:DATA AAAA",
    "SYST:UPD:COMMIT", "SYST:UPD:ABORT", "SYST:UPD:STAT?", "SYST:UPD:CONFIRM", "TEST:ALL?", "INTERLOCK:LIST?",
    "ROUT:HIGH M4", "ROUT:STAT?",
};

static void mutate(char *out, size_t max)
{
    const char *s = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
    size_t n = strlen(s);
    size_t cut = rnd() % (n + 1);
    size_t len = 0;
    for (size_t i = 0; i < cut && len + 1 < max; i++) {
        char ch = s[i];
        if (rnd() % 5 == 0) {
            ch = alphabet[rnd() % strlen(alphabet)];
        }
        out[len++] = ch;
    }
    if (rnd() % 3 == 0 && len + 12 < max) {
        len += (size_t)snprintf(out + len, max - len, ",%lu", (unsigned long)(rnd() % 100000));
    }
    out[len] = '\0';
}

int main(void)
{
    device_boot();
    CHECK(tcp_connect());
    con_cmd("SAFE");
    struct desired before = desired;
    static uint8_t out[16384];
    static char line[CMD_LINE_MAX + 200];
    for (int i = 0; i < 3000; i++) {
        if (i % 2) {
            random_line(line, sizeof line - 1);
        } else {
            mutate(line, sizeof line - 1);
        }
        /* SAFE и запросы состояние не меняют; PROV:NET меняет только CFG; UPD:* — flash модели */
        host_net_push_line(line);
        device_run(4);
        size_t n = host_net_take(out, sizeof out);
        int frames = 0;
        for (size_t k = 0; k < n; k++) {
            frames += out[k] == '\n';
        }
        /* пустая строка — не команда: ответа нет; иначе ровно один кадр */
        bool blank = true;
        for (const char *p = line; *p; p++) {
            blank = blank && (*p == ' ' || *p == '\t');
        }
        CHECK(frames == (blank ? 0 : 1));
        CHECK(memcmp(&before, &desired, sizeof before) == 0);
        CHECK(observed.client_connected);
    }
    /* устройство живо и отвечает по-прежнему */
    CHECK(strstr(tcp_cmd("*IDN?"), "TESTDUT,CONTR-R1") != NULL);
    return test_result("test_fuzz");
}
