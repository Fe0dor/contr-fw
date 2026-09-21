/* Таблица команд 3.0, разбор строки, арбитраж по каналу (А4, А5), форматтер ответа
 * с префиксом G<n>; по TCP (FW-107). Один кадр на команду (FW-106). */
#include "cmd.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "config.h"
#include "errlog.h"
#include "hal.h"
#include "netcfg.h"
#include "safe.h"
#include "relays.h"
#include "sections.h"
#include "signals.h"
#include "sr.h"
#include "state.h"
#include "update.h"

/* ---------------------------------------------------------------- ответ */

static void flush(struct cmdctx *c)
{
    if (c->len && !c->closed) {
        if (!channel_write(c->ch, (const uint8_t *)c->buf, c->len)) {
            c->closed = true;
        }
    }
    c->len = 0;
}

static void begin(struct cmdctx *c)
{
    if (c->started) {
        return;
    }
    c->started = true;
    c->len = 0;
    if (c->ch == CH_TCP) {
        c->len = (size_t)snprintf(c->buf, sizeof c->buf, "G%lu;", (unsigned long)observed.generation);
    }
}

void resp_putc(struct cmdctx *c, char ch)
{
    begin(c);
    if (c->len >= sizeof c->buf) {
        flush(c);
    }
    c->buf[c->len++] = ch;
}

void resp_puts(struct cmdctx *c, const char *s)
{
    while (*s) {
        resp_putc(c, *s++);
    }
}

void resp_printf(struct cmdctx *c, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    resp_puts(c, tmp);
}

void resp_end(struct cmdctx *c)
{
    if (c->ended) {
        return;
    }
    begin(c);
    resp_putc(c, '\n');
    flush(c);
    c->ended = true;
}

void resp_ok(struct cmdctx *c)
{
    resp_puts(c, "OK");
    resp_end(c);
}

void resp_err(struct cmdctx *c, enum err_code code, const char *fields)
{
    c->failed = true;
    c->err = code;
    resp_puts(c, "ERR:");
    resp_puts(c, cmd_err_name(code));
    if (fields && *fields) {
        resp_putc(c, ',');
        resp_puts(c, fields);
    }
    resp_end(c);
    err_push(code, fields);
    if (c->cmd) {
        log_event("%s ERR:%s", c->cmd->name, cmd_err_name(code));
    }
}

const char *cmd_err_name(enum err_code code)
{
    switch (code) {
    case ERR_UNKNOWN_CMD: return "UNKNOWN_CMD";
    case ERR_RANGE: return "RANGE";
    case ERR_INTERLOCK: return "INTERLOCK";
    case ERR_PSU_FAULT: return "PSU_FAULT";
    case ERR_PSU_ALARM: return "PSU_ALARM";
    case ERR_I2C_FAIL: return "I2C_FAIL";
    case ERR_SECTION_OFF: return "SECTION_OFF";
    case ERR_SECTION_FAIL: return "SECTION_FAIL";
    case ERR_LOADBOARD_FAIL: return "LOADBOARD_FAIL";
    case ERR_LOADBOARD_VERSION: return "LOADBOARD_VERSION";
    case ERR_SAFE_INCOMPLETE: return "SAFE_INCOMPLETE";
    case ERR_ABORTED: return "ABORTED";
    case ERR_UPD_SEQ: return "UPD_SEQ";
    case ERR_UPD_CRC: return "UPD_CRC";
    case ERR_UPD_HEADER: return "UPD_HEADER";
    case ERR_NOT_SAFE: return "NOT_SAFE";
    case ERR_PROVISIONED: return "PROVISIONED";
    case ERR_CONSOLE_ONLY: return "CONSOLE_ONLY";
    case ERR_BUSY: return "BUSY";
    default: return "NONE";
    }
}

/* ---------------------------------------------------------------- обработчики шага 2 */

static void h_idn(struct cmdctx *c)
{
    char serial[CFG_SERIAL_MAX + 1];
    if (!cfg_get_serial(serial)) {
        strcpy(serial, "UNPROVISIONED");
    }
    resp_printf(c, "%s,%s-R%d,%s,%d.%d.%d", FW_VENDOR, FW_MODEL, BOARD_REV, serial, FW_VERSION_MAJOR,
                FW_VERSION_MINOR, FW_VERSION_PATCH);
    resp_end(c);
}

static void h_safe(struct cmdctx *c)
{
    bool ok = safe_enter(SAFE_REASON_COMMAND);
    if (ok) {
        resp_ok(c);
        return;
    }
    char fields[32] = "";
    size_t n = 0;
    for (int i = 0; i < SAFE_STEPS; i++) {
        if (observed.safe.steps[i] == STEP_FAILED) {
            n += (size_t)snprintf(fields + n, sizeof fields - n, "%s%d", n ? "," : "", i + 1);
        }
    }
    resp_err(c, ERR_SAFE_INCOMPLETE, fields);
}

static void h_syst_safe(struct cmdctx *c)
{
    const struct safe_report *r = &observed.safe;
    resp_printf(c, "%lu,%s", (unsigned long)r->seq, safe_reason_name(r->reason));
    for (int i = 0; i < SAFE_STEPS; i++) {
        resp_printf(c, ",%s", safe_step_name(r->steps[i]));
    }
    resp_printf(c, ",%lu", (unsigned long)r->duration_ms);
    resp_end(c);
}

static void h_syst_err(struct cmdctx *c)
{
    struct err_entry e;
    bool any = false;
    while (err_pop(&e)) {
        if (any) {
            resp_putc(c, ';');
        }
        resp_puts(c, e.code == ERR_NONE ? "INFO" : cmd_err_name(e.code));
        if (e.text[0]) {
            resp_putc(c, ',');
            resp_puts(c, e.text);
        }
        any = true;
    }
    if (!any) {
        resp_puts(c, "NONE");
    }
    resp_end(c);
}

static void h_syst_log(struct cmdctx *c)
{
    size_t n = log_count();
    if (n == 0) {
        resp_puts(c, "NONE");
    }
    for (size_t i = 0; i < n; i++) {
        const struct log_entry *e = log_at(i);
        if (i) {
            resp_putc(c, ';');
        }
        resp_printf(c, "%lu,%s", (unsigned long)e->ms, e->text);
    }
    resp_end(c);
}

#define STR2(x) #x
#define STR(x) STR2(x)

/* строковая константа после стрингификации несёт кавычки: снять их */
static void resp_puts_unquoted(struct cmdctx *c, const char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        for (size_t i = 1; i + 1 < n; i++) {
            resp_putc(c, s[i]);
        }
    } else {
        resp_puts(c, s);
    }
}

static void h_syst_conf(struct cmdctx *c)
{
    bool first = true;
#define EMIT(name)                                          \
    do {                                                    \
        resp_printf(c, "%s%s=", first ? "" : ";", #name);   \
        resp_puts_unquoted(c, STR(name));                   \
        first = false;                                      \
    } while (0);
    CONFIG_ITEMS(EMIT)
#undef EMIT
    resp_end(c);
}

static void h_syst_net(struct cmdctx *c)
{
    char macs[18], ip[16];
    netcfg_mac_string(macs);
    netcfg_format_ip(hal_net_addr(), ip);
    resp_printf(c, "%s,%s,%s,%s", netcfg_mode_name(), macs, ip, hal_net_link_up() ? "UP" : "DOWN");
    resp_end(c);
}

static void h_prov_serial(struct cmdctx *c)
{
    char cur[CFG_SERIAL_MAX + 1];
    if (cfg_get_serial(cur)) {
        resp_err(c, ERR_PROVISIONED, NULL);
        return;
    }
    const char *s = c->argv[0];
    size_t len = strlen(s);
    if (len == 0 || len > CFG_SERIAL_MAX) {
        resp_err(c, ERR_RANGE, "serial");
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (!(isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_')) {
            resp_err(c, ERR_RANGE, "serial");
            return;
        }
    }
    if (cfg_set_serial(s) != 0) {
        resp_err(c, ERR_RANGE, "flash");
        return;
    }
    log_event("provisioned %s", s);
    resp_ok(c);
}

static void h_prov_net(struct cmdctx *c)
{
    struct cfg_net n;
    if (!netcfg_parse_prov(c->argc, c->argv, &n)) {
        resp_err(c, ERR_RANGE, "net");
        return;
    }
    if (cfg_set_net(&n) != 0) {
        resp_err(c, ERR_RANGE, "flash");
        return;
    }
    log_event("net %s", n.dhcp ? "dhcp" : "static");
    resp_ok(c);
}

static void h_test_all(struct cmdctx *c)
{
    /* шаг 2: часть провижининга, опционных байтов, сети, сброса и входа; остальное — по шагам */
    struct hal_optbytes ob = hal_optbytes_read();
    bool opt_ok = ob.ndbank == (OPT_NDBANK != 0) && ob.ndboot == (OPT_NDBOOT != 0) && ob.iwdg_sw == (OPT_IWDG_SW != 0);
    bool safe_ok = observed.safe.complete;
    bool sr_ok = sr_test_loop();
    bool i2c_a=section_test(0), i2c_b=section_test(1);
    bool warn = true; /* SR1:UNVERIFIED и шаг «подтверждён молчанием» */
    const char *verdict = (!opt_ok || !safe_ok || !sr_ok || !i2c_a || !i2c_b) ? "FAIL" : warn ? "WARN" : "OK";
    char serial[CFG_SERIAL_MAX + 1], macs[18], ip[16];
    if (!cfg_get_serial(serial)) {
        strcpy(serial, "UNPROVISIONED");
    }
    netcfg_mac_string(macs);
    netcfg_format_ip(hal_net_addr(), ip);
    const char *cause = (observed.reset_cause & HAL_RST_IWDG) ? "WATCHDOG"
                        : (observed.reset_cause & HAL_RST_PIN) ? "PIN"
                        : (observed.reset_cause & HAL_RST_SW)  ? "SOFTWARE"
                        : (observed.reset_cause & HAL_RST_POR) ? "POWER"
                                                               : "UNKNOWN";
    bool silent = false;
    for (int i = 0; i < SAFE_STEPS; i++) {
        silent = silent || observed.safe.steps[i] == STEP_SILENT;
    }
    resp_printf(c, "%s;SR0:%s;SR1:UNVERIFIED;I2C_A:%s;I2C_B:%s;DCOK:%d;ALARM:%d;", verdict, sr_ok ? "OK" : "FAIL",
                !desired.section_on[0]?"SKIP,OFF":i2c_a?"OK":"FAIL",
                !desired.section_on[1]?"SKIP,OFF":i2c_b?"OK":"FAIL",
                signal_read(SIG_RSP_DC_OK) ? 1 : 0, signal_read(SIG_RSP_ALARM) ? 1 : 0);
    resp_printf(c, "LOADBOARD:LINK_LOST;INTERLOCK:%s;", RELAY_TABLE_CHECKSUM);
    resp_printf(c, "PROV:%s;OPTBYTES:%s;", serial, opt_ok ? "OK" : "MISMATCH");
    resp_printf(c, "NET:%s,%s,%s;RESET:%s;SAFE:%s", netcfg_mode_name(), ip, hal_net_link_up() ? "UP" : "DOWN", cause,
                !safe_ok ? "SAFE_INCOMPLETE" : silent ? "SILENT" : "OK");
    resp_end(c);
}

static void h_interlock_list(struct cmdctx *c)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        const struct relay_desc *r = &relay_table[i];
        resp_printf(c, "%s,%u,%s,%s,", r->name, r->number, r->block, r->address);
        bool first = true;
        for (int g = 0; g < RELAY_GROUP_COUNT; g++) {
            if ((r->group_mask >> g) & 1u) {
                resp_printf(c, "%s%s", first ? "" : "+", relay_groups[g].name);
                first = false;
            }
        }
        resp_putc(c, ';');
    }
    resp_printf(c, "%s,%s", RELAY_TABLE_VERSION, RELAY_TABLE_CHECKSUM);
    resp_end(c);
}

/* ---------------------------------------------------------------- таблица */

#ifdef CONTR_BRINGUP
void dbg_pin(struct cmdctx *c);
void dbg_pin_q(struct cmdctx *c);
void dbg_sr(struct cmdctx *c);
void dbg_sr_q(struct cmdctx *c);
void dbg_i2c_scan(struct cmdctx *c);
void dbg_i2c(struct cmdctx *c);
void dbg_i2c_stat(struct cmdctx *c);
void dbg_i2c_timing(struct cmdctx *c);
void dbg_i2c_reset(struct cmdctx *c);
void dbg_spi(struct cmdctx *c);
#endif

static const struct cmd_desc table[] = {
    {"*IDN?", 0, 0, CMDF_QUERY, h_idn},
    {"SAFE", 0, 0, CMDF_SAFE, h_safe},
    {"SYST:SAFE?", 0, 0, CMDF_QUERY, h_syst_safe},
    {"SYST:ERR?", 0, 0, CMDF_QUERY, h_syst_err},
    {"SYST:LOG?", 0, 0, CMDF_QUERY, h_syst_log},
    {"SYST:CONF?", 0, 0, CMDF_QUERY, h_syst_conf},
    {"SYST:NET?", 0, 0, CMDF_QUERY, h_syst_net},
    {"SYST:PROV:SERIAL", 1, 1, CMDF_CONSOLE_ONLY, h_prov_serial},
    {"SYST:PROV:NET", 1, 4, CMDF_CONSOLE_ONLY, h_prov_net},
    {"SYST:UPD:BEGIN", 3, 3, 0, cmd_upd_begin},
    {"SYST:UPD:DATA", 1, 1, 0, cmd_upd_data},
    {"SYST:UPD:COMMIT", 0, 0, 0, cmd_upd_commit},
    {"SYST:UPD:ABORT", 0, 0, 0, cmd_upd_abort},
    {"SYST:UPD:STAT?", 0, 0, CMDF_QUERY, cmd_upd_stat},
    {"SYST:UPD:CONFIRM", 0, 0, 0, cmd_upd_confirm},
    {"TEST:ALL?", 0, 0, CMDF_QUERY, h_test_all},
    {"INTERLOCK:LIST?", 0, 0, CMDF_QUERY, h_interlock_list},
    {"RES:PWR", 2, 2, CMDF_SET, cmd_res_pwr},
    {"RES:SET", 3, 3, CMDF_SET, cmd_res_set},
    {"RES:STAT?", 0, 0, CMDF_QUERY, cmd_res_stat},
    {"ROUT:HIGH", 1, 81, CMDF_SET, cmd_route},
    {"ROUT:LOW", 1, 81, CMDF_SET, cmd_route},
    {"ROUT:LOW:ALL", 0, 0, CMDF_SET, cmd_route},
    {"ROUT:SET", 0, 81, CMDF_SET, cmd_route},
    {"ROUT:STAT?", 0, 0, CMDF_QUERY, cmd_route_stat},
#ifdef CONTR_BRINGUP
    {"DBG:PIN", 2, 2, CMDF_SET | CMDF_BRINGUP, dbg_pin},
    {"DBG:PIN?", 1, 1, CMDF_QUERY | CMDF_BRINGUP, dbg_pin_q},
    {"DBG:SR", 2, 2, CMDF_SET | CMDF_BRINGUP, dbg_sr},
    {"DBG:SR?", 1, 1, CMDF_QUERY | CMDF_BRINGUP, dbg_sr_q},
    {"DBG:I2C:SCAN", 1, 1, CMDF_QUERY | CMDF_BRINGUP, dbg_i2c_scan},
    {"DBG:I2C", 4, 4, CMDF_SET | CMDF_BRINGUP, dbg_i2c},
    {"DBG:I2C:STAT?", 1, 1, CMDF_QUERY | CMDF_BRINGUP, dbg_i2c_stat},
    {"DBG:I2C:TIMING", 2, 2, CMDF_SET | CMDF_BRINGUP, dbg_i2c_timing},
    {"DBG:I2C:RESET", 1, 1, CMDF_SET | CMDF_BRINGUP, dbg_i2c_reset},
    {"DBG:SPI", 1, 1, CMDF_SET | CMDF_BRINGUP, dbg_spi},
#endif
};

const struct cmd_desc *cmd_table(size_t *count)
{
    *count = sizeof table / sizeof table[0];
    return table;
}

/* ---------------------------------------------------------------- разбор и исполнение */

static const struct cmd_desc *find(const char *name)
{
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    return s;
}

bool cmd_is_safe_line(const char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if (toupper((unsigned char)line[0]) != 'S' || toupper((unsigned char)line[1]) != 'A'
        || toupper((unsigned char)line[2]) != 'F' || toupper((unsigned char)line[3]) != 'E') {
        return false;
    }
    line += 4;
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    return *line == '\0';
}

static void reply_simple_err(enum channel_id ch, enum err_code code)
{
    struct cmdctx c;
    memset(&c, 0, sizeof c);
    c.ch = ch;
    resp_err(&c, code, NULL);
}

void cmd_reply_busy(enum channel_id ch)
{
    reply_simple_err(ch, ERR_BUSY);
}

void cmd_reply_overflow(enum channel_id ch)
{
    reply_simple_err(ch, ERR_RANGE);
}

void cmd_execute(enum channel_id ch, char *line)
{
    struct cmdctx c;
    memset(&c, 0, sizeof c);
    c.ch = ch;
    line = trim(line);
    if (*line == '\0') {
        return; /* пустая строка: ни одного кадра — не команда */
    }
    /* имя до первого пробела, в верхнем регистре */
    char *args = line;
    while (*args && !isspace((unsigned char)*args)) {
        *args = (char)toupper((unsigned char)*args);
        args++;
    }
    if (*args) {
        *args++ = '\0';
        args = trim(args);
    }
    const struct cmd_desc *cmd = find(line);
    if (!cmd) {
        resp_err(&c, ERR_UNKNOWN_CMD, NULL);
        return;
    }
    c.cmd = cmd;
    /* аргументы через ',' без пробелов */
    if (*args) {
        char *p = args;
        while (p) {
            if (c.argc >= CMD_MAX_ARGS) {
                resp_err(&c, ERR_RANGE, "args");
                return;
            }
            char *comma = strchr(p, ',');
            if (comma) {
                *comma = '\0';
            }
            c.argv[c.argc++] = p;
            p = comma ? comma + 1 : NULL;
        }
    }
    if (c.argc < cmd->min_args || c.argc > cmd->max_args) {
        resp_err(&c, ERR_RANGE, "args");
        return;
    }
    /* арбитраж (А5, FW-116, FW-117) */
    if (cmd->flags & CMDF_CONSOLE_ONLY) {
        if (ch != CH_CONSOLE || observed.client_connected) {
            resp_err(&c, ERR_CONSOLE_ONLY, NULL);
            return;
        }
    } else if (ch == CH_CONSOLE && observed.client_connected && !(cmd->flags & (CMDF_QUERY | CMDF_SAFE))) {
        resp_err(&c, ERR_BUSY, NULL);
        return;
    }
    cmd->fn(&c);
    if (!c.ended) {
        resp_ok(&c); /* обработчик обязан завершить кадр; страховка принципа 1 */
    }
    if (!c.failed && !(cmd->flags & (CMDF_QUERY | CMDF_SAFE | CMDF_CONSOLE_ONLY))) {
        if (cmd->flags & CMDF_SET) {
            observed.in_safe_state = false;
        }
        if (ch == CH_CONSOLE) {
            observed.generation++; /* установка с консоли (FW-221) */
        }
    }
}
