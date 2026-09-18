/* Константы сборки (ТЗ 19, раздел 5) и версия прошивки. Команд изменения нет (FW-222);
 * все константы выдаются по SYST:CONF? (FW-223) через список CONFIG_ITEMS.
 * Значения с пометкой «ОТКРЫТО» в ТЗ стоят предварительные и уточняются по шагам. */
#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 2
#define FW_VERSION_PATCH 5
#endif
#define FW_MODEL "CONTR"
#define FW_VENDOR "TESTDUT"

#define BOARD_REV 1
#define PWRON_AB_ON_LEVEL 1     /* 1: включено — вывод отпущен во вход, выключено — выход 0 */
#define PWROK_PRESENT 0
#define PWROK_ACTIVE_LOW 1
#define PWROK_AB_ACTIVE_LOW 1   /* ОТКРЫТО-5 */
#define CS_LOAD_ON_SR 1         /* SR0.2.B */
#define CS_ACTIVE_LOW 1
#define PWRON_RSP_ON_LEVEL 0
#define SR1_LOOP_PRESENT 0
#define RELAY_BREAK_MS 20
#define PSU_VMIN 42
#define PSU_VMAX 52
#define PSU_DAC_MIN 0           /* ОТКРЫТО-1 */
#define PSU_DAC_MAX 4095        /* ОТКРЫТО-1 */
#define DCOK_ACTIVE_HIGH 1      /* ОТКРЫТО-1 */
#define ALARM_ACTIVE_HIGH 1     /* ОТКРЫТО-1 */
#define DCOK_RISE_TIMEOUT_MS 2000 /* ОТКРЫТО-1 */
#define DCOK_FALL_TIMEOUT_MS 2000 /* ОТКРЫТО-1 */
#define SAFE_STATE_BUDGET_MS 4000
#define TCP_KEEPALIVE_IDLE_S 10
#define TCP_KEEPALIVE_INTVL_S 5
#define TCP_KEEPALIVE_CNT 4
#define DUT_POLL_MS 10
#define DUT_DEBOUNCE_MS 30
#define I2C_RETRIES 3
#define I2C_SPEED_HZ 400000
#define SECTION_PWROK_TIMEOUT_MS 500  /* при выпуске */
#define SECTION_SETTLE_MS 100         /* при выпуске */
#define SECTION_ON_MAX_MS 1000
#define LOAD_HEARTBEAT_MS 100
#define LOAD_LINK_TIMEOUT_MS 500
#define LOAD_LINK_MARGIN_MS 100
#define LOAD_FRAME_GAP_US 100         /* при выпуске */
#define LOAD_PROTO_MIN 1
#define LOAD_PROTO_MAX 1
#define PWM_FREQ_MIN_HZ 1000
#define PWM_FREQ_MAX_HZ 100000
#define PWM_FREQ_STEP_HZ 1000
#define DHCP_TIMEOUT_MS 10000
#define NET_DEFAULT_ADDR "192.168.0.20"
#define NET_DEFAULT_MASK "255.255.255.0"
#define NET_DEFAULT_GW "192.168.0.100"
#define NET_PORT 5025
#define BUTTON_RESET_HOLD_MS 5000
#define UPD_CONFIRM_TIMEOUT_MS 60000
#define UPD_BLOCK_MAX_B 256
#define UPD_MIN_VERSION "0.2.0"
#define LOG_DEPTH 256
#define OPT_NDBANK 0
#define OPT_NDBOOT 0
#define OPT_IWDG_SW 0

#define WDT_PERIOD_MS 4000  /* стирание сектора 128 КБ укладывается в период */

/* X-макрос для SYST:CONF?: имя и строковое значение. */
#define CONFIG_ITEMS(X)                                     \
    X(BOARD_REV)                                            \
    X(PWRON_AB_ON_LEVEL)                                    \
    X(PWROK_PRESENT)                                        \
    X(PWROK_ACTIVE_LOW)                                     \
    X(PWROK_AB_ACTIVE_LOW)                                  \
    X(CS_LOAD_ON_SR)                                        \
    X(CS_ACTIVE_LOW)                                        \
    X(PWRON_RSP_ON_LEVEL)                                   \
    X(SR1_LOOP_PRESENT)                                     \
    X(RELAY_BREAK_MS)                                       \
    X(PSU_VMIN)                                             \
    X(PSU_VMAX)                                             \
    X(PSU_DAC_MIN)                                          \
    X(PSU_DAC_MAX)                                          \
    X(DCOK_ACTIVE_HIGH)                                     \
    X(ALARM_ACTIVE_HIGH)                                    \
    X(DCOK_RISE_TIMEOUT_MS)                                 \
    X(DCOK_FALL_TIMEOUT_MS)                                 \
    X(SAFE_STATE_BUDGET_MS)                                 \
    X(TCP_KEEPALIVE_IDLE_S)                                 \
    X(TCP_KEEPALIVE_INTVL_S)                                \
    X(TCP_KEEPALIVE_CNT)                                    \
    X(DUT_POLL_MS)                                          \
    X(DUT_DEBOUNCE_MS)                                      \
    X(I2C_RETRIES)                                          \
    X(I2C_SPEED_HZ)                                         \
    X(SECTION_PWROK_TIMEOUT_MS)                             \
    X(SECTION_SETTLE_MS)                                    \
    X(SECTION_ON_MAX_MS)                                    \
    X(LOAD_HEARTBEAT_MS)                                    \
    X(LOAD_LINK_TIMEOUT_MS)                                 \
    X(LOAD_LINK_MARGIN_MS)                                  \
    X(LOAD_FRAME_GAP_US)                                    \
    X(LOAD_PROTO_MIN)                                       \
    X(LOAD_PROTO_MAX)                                       \
    X(PWM_FREQ_MIN_HZ)                                      \
    X(PWM_FREQ_MAX_HZ)                                      \
    X(PWM_FREQ_STEP_HZ)                                     \
    X(DHCP_TIMEOUT_MS)                                      \
    X(NET_DEFAULT_ADDR)                                     \
    X(NET_DEFAULT_MASK)                                     \
    X(NET_DEFAULT_GW)                                       \
    X(BUTTON_RESET_HOLD_MS)                                 \
    X(UPD_CONFIRM_TIMEOUT_MS)                               \
    X(UPD_BLOCK_MAX_B)                                      \
    X(UPD_MIN_VERSION)                                      \
    X(LOG_DEPTH)                                            \
    X(OPT_NDBANK)                                           \
    X(OPT_NDBOOT)                                           \
    X(OPT_IWDG_SW)

#endif /* CORE_CONFIG_H */
