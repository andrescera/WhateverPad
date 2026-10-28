/* Read-budget decisions from the 19422e9 console run, not a radio simulation. */
#include "../src/event_reads.h"
#include "../src/host_int.h"
#include "../src/util.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main(void)
{
    /* task-7-partB-whateverpad.log:14598-14618, Xbox reconnect then ready. */
    static const unsigned char connected[] = {
        0x03, 0x0b, 0x00, 0x01, 0x00, 0x06, 0xfa, 0xab, 0xea, 0xb6, 0x98, 0x01, 0x00
    };
    /* Same log:14623-14628, two native requests, both followed by a yield. */
    static const unsigned char incoming[] = {
        0x04, 0x0a, 0xd6, 0x32, 0xb0, 0x8d, 0x18, 0x48, 0x08, 0x25, 0x00, 0x01
    };
    static const long request_at[] = { 944769, 989681 };
    static const long yielded_at[] = { 944771, 989684 };
    host_t h = {0};
    int i;

    h.links[0].used = h.links[0].connected = h.links[0].ready = 1;
    h.links[0].handle = le16(connected + 3) & 0x0fff;
    memcpy(h.links[0].addr, connected + 5, 6);
    CHECK(link_by_addr(&h, incoming + 2) == NULL);

    puts("idle beside the ready Xbox pad: one event read, not forty");
    for (i = 0; i < 2; i++)
        CHECK(event_reads_wanted(&h, request_at[i], g_tune.idle_reads) == 1);

    puts("a foreign call leaves no event reads, including during our setup");
    for (i = 0; i < 2; i++) {
        h.yield_at = yielded_at[i];
        h.links[0].ready = 0;
        h.pair_until = yielded_at[i] + 60000;
        CHECK(event_reads_wanted(&h, yielded_at[i], 1) == 0);
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD - 1, 1) == 0);
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD, 1) == HOST_EV_READS);
        h.links[0].ready = 1;
        h.pair_until = 0;
        CHECK(event_reads_wanted(&h, yielded_at[i] + T_YIELD, g_tune.idle_reads) == 1);
    }
    h.yield_at = 0;

    puts("searches, reconnect setup, disconnects and command replies keep full reads");
    h.links[0].ready = 0;             /* reconnecting, not a new pairing */
    CHECK(event_reads_wanted(&h, 900813, 1) == HOST_EV_READS);
    h.links[0].ready = 1;
    h.links[0].disconnecting = 1;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.links[0].disconnecting = 0;
    h.pair_until = 944770;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.pair_until = 0;
    h.inquiring = 1;                 /* still awaiting Inquiry Complete */
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.inquiring = 0;
    h.repage_at = 944769;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.repage_at = 0;
    h.defer_at = 944769;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.defer_at = 0;
    h.pair_pending_s = 60;
    CHECK(event_reads_wanted(&h, 944769, 1) == HOST_EV_READS);
    h.pair_pending_s = 0;
    h.full_reads_until = 927347 + 2000; /* logged Read Buffer Size, existing reply interval */
    CHECK(event_reads_wanted(&h, h.full_reads_until - 1, 1) == HOST_EV_READS);
    CHECK(event_reads_wanted(&h, h.full_reads_until, 1) == 1);

    puts("the existing explicit idle-read tuning remains available");
    CHECK(event_reads_wanted(&h, 944769, 0) == 0);
    CHECK(event_reads_wanted(&h, 944769, HOST_EV_READS) == HOST_EV_READS);
    printf(g_fail ? "%d check(s) failed\n" : "all event-read checks passed\n", g_fail);
    return g_fail != 0;
}
