#include "check.h"

static unsigned polls;
static bool console_input;
static void tick_and_send(void)
{
    host_advance_ms(1);
    if (++polls != 20) return;
    for (unsigned i = 0; i < 8; ++i) {
        const char *line = i == 7 ? "RES:PWR B,ON" : "*IDN?";
        if (console_input) host_console_push_line(line);
        else host_net_push_line(line);
    }
}

static void send_queued_safe(void)
{
    host_advance_ms(1);
    if (++polls != 20) return;
    for (unsigned i = 0; i < 5; ++i) {
        const char *line = i == 4 ? "  sAfE \r" : "*IDN?";
        if (console_input) host_console_push_line(line);
        else host_net_push_line(line);
    }
}

static unsigned safe_variant;
static void send_safe_variant(void)
{
    host_advance_ms(1);
    if (++polls == 20) {
        for (unsigned i = 0; i < 4; ++i) host_net_push_line("*IDN?");
        if (safe_variant == 0) host_net_push_line("SAFE junk");
        else if (safe_variant == 1) {
            static const uint8_t invalid[] = {'S','A','F','E',0,'\n'};
            host_net_push(invalid, sizeof invalid);
        } else host_net_push((const uint8_t *)"SA", 2);
    } else if (polls == 21 && safe_variant == 2) {
        CHECK(!core_abort_requested());
        host_net_push((const uint8_t *)"FE\n", 3);
    }
}

static void safe_variants(void)
{
    for (safe_variant = 0; safe_variant < 3; ++safe_variant) {
        device_boot();
        CHECK(tcp_connect());
        polls = 0;
        host_net_set_poll_hook(send_safe_variant);
        host_net_push_line("RES:PWR A,ON");
        device_run(1);
        CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
        CHECK_STR(reply_buf, safe_variant == 2 ? "G1;ERR:ABORTED" : "G1;OK");
        CHECK(desired.section_on[0] == (safe_variant != 2));
        device_run(12);
        if (safe_variant == 2) CHECK(observed.in_safe_state);
    }
}

static void queued_safe(void)
{
    for (unsigned channel = 0; channel < 2; ++channel) {
        device_boot();
        CHECK(tcp_connect());
        console_input = channel != 0;
        polls = 0;
        host_net_set_poll_hook(send_queued_safe);
        host_net_push_line("RES:PWR A,ON");
        device_run(1);
        CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
        CHECK_STR(reply_buf, "G1;ERR:ABORTED");
        CHECK(!desired.section_on[0]);
        device_run(12);
        for (unsigned i = 0; i < 5; ++i) {
            CHECK(console_input ? host_console_take_line(reply_buf, sizeof reply_buf)
                                : host_net_take_line(reply_buf, sizeof reply_buf));
            CHECK_STR(reply_buf, i == 4 ? (console_input ? "OK" : "G2;OK")
                                       : (console_input ? "ERR:BUSY" : "G1;ERR:BUSY"));
        }
        CHECK(observed.in_safe_state);
    }
}

static unsigned overflow_variant;
static void overflow_packet(void)
{
    host_advance_ms(1);
    if (++polls != 5) return;
    char packet[1800]="";
    if (overflow_variant==0) strcat(packet,"SAFE\n");
    else if (overflow_variant==1) {memset(packet,' ',507);strcpy(packet+507,"SAFE\n");}
    else if (overflow_variant==2) strcat(packet,"SAFE junk\n");
    for (unsigned i=0;i<100;i++) strcat(packet,"*IDN?\n");
    if (overflow_variant==3) strcat(packet,"sAfE\n");
    if (console_input) host_console_push((const uint8_t *)packet,strlen(packet));
    else host_net_push((const uint8_t *)packet,strlen(packet));
}
static void safe_survives_overflow(void)
{
    for (unsigned channel=0;channel<2;channel++) for (overflow_variant=0;overflow_variant<4;overflow_variant++) {
        device_boot();CHECK(tcp_connect());
        console_input=channel!=0;polls=0;host_net_set_poll_hook(overflow_packet);
        host_net_push_line("ROUT:HIGH M4");device_run(1);
        CHECK(host_net_take_line(reply_buf,sizeof reply_buf));
        CHECK_STR(reply_buf,overflow_variant==2?"G1;OK":"G1;ERR:ABORTED");
        device_run(200);
        CHECK(observed.safe.seq==(overflow_variant==2?1u:2u));
        if (overflow_variant!=2) {
            CHECK(observed.in_safe_state);
            CHECK(!desired.relays[0]&&!desired.relays[1]&&!desired.relays[2]);
        }
    }
}

int main(void)
{
    for (unsigned channel = 0; channel < 2; ++channel) {
        device_boot();
        CHECK(tcp_connect());
        console_input = channel != 0;
        polls = 0;
        host_net_set_poll_hook(tick_and_send);
        host_net_push_line("RES:PWR A,ON");
        device_run(20);
        CHECK(host_net_take_line(reply_buf, sizeof reply_buf));
        CHECK_STR(reply_buf, "G1;OK");
        for (unsigned i = 0; i < 8; ++i) {
            bool got = console_input ? host_console_take_line(reply_buf, sizeof reply_buf)
                                     : host_net_take_line(reply_buf, sizeof reply_buf);
            CHECK(got);
            CHECK_STR(reply_buf, console_input ? "ERR:BUSY" : "G1;ERR:BUSY");
        }
        CHECK(desired.section_on[0]);
        CHECK(!desired.section_on[1]);
        /* A newly received command after completion must still execute. */
        CHECK_STR(tcp_cmd("RES:PWR B,ON"), "G1;OK");
        CHECK(desired.section_on[1]);
    }
    safe_survives_overflow();
    queued_safe();
    safe_variants();
    return test_result("busy_queue");
}
