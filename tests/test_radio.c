/* Goodbye commands from the console's handle and page-scan activity values. */
#include "../src/radio.h"
#include "../src/host_int.h"
#include "../src/util.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void lifecycle_table(void)
{
    static const struct {
        const char *name;
        radio_input in;
        radio_action want;
    } cases[] = {
        { "startup closed, no request", { .vpad_ok = 1, .now = 758173 }, RADIO_NOTHING },
        { "Pair pressed", { .vpad_ok = 1, .pair_requested = 1 }, RADIO_OPEN },
        { "vpad unavailable", { .pair_requested = 1 }, RADIO_NOTHING },
        { "trying", { .vpad_ok = 1, .trying = 1, .pair_requested = 1 }, RADIO_NOTHING },
        { "window open at 758173", { .open = 1, .busy = 1, .now = 758173 }, RADIO_NOTHING },
        { "window ended at 818173, one ms before linger", { .open = 1, .last_busy = 818173, .now = 848172 }, RADIO_NOTHING },
        { "window ended at 818173, linger elapsed", { .open = 1, .last_busy = 818173, .now = 848173 }, RADIO_CLOSE },
        { "dropout reconnects after 14624 ms", { .open = 1, .last_busy = 886098, .now = 900722 }, RADIO_NOTHING },
        { "open with Pair requested", { .open = 1, .pair_requested = 1, .now = 900722 }, RADIO_NOTHING },
        { "failed closed with request", { .vpad_ok = 1, .pair_requested = 1, .now = 900722 }, RADIO_OPEN },
        { "busy after linger", { .open = 1, .busy = 1, .now = 900722 }, RADIO_NOTHING },
        { "trying while open", { .open = 1, .trying = 1, .now = 900722 }, RADIO_NOTHING },
        { "closed after release without request", { .vpad_ok = 1, .now = 900722 }, RADIO_NOTHING },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        printf("radio lifecycle: %s\n", cases[i].name);
        CHECK(radio_decide(cases[i].in) == cases[i].want);
    }
}

static void link_check_table(void)
{
    static const struct {
        const char *name;
        long now, last_acl, last_check;
        int unanswered;
        radio_link_action want;
    } cases[] = {
        { "ACL silent 19999 ms", 886098 + 19999, 886098, 886098, 0, RADIO_LINK_NOTHING },
        { "ACL silent 20000 ms, first check", 886098 + 20000, 886098, 886098, 0, RADIO_LINK_CHECK },
        { "ACL silent 20000 ms, third check", 886098 + 20000, 886098, 886098, 2, RADIO_LINK_CHECK },
        { "three unanswered checks", 886098 + 20000, 886098, 886098, 3, RADIO_LINK_GIVE_UP },
        { "recent ACL overrides unanswered", 900722, 900722, 0, 3, RADIO_LINK_NOTHING },
        { "previous check 19999 ms ago", 900722, 0, 900722 - 19999, 3, RADIO_LINK_NOTHING },
        { "previous check 20000 ms ago", 900722, 0, 900722 - 20000, 3, RADIO_LINK_GIVE_UP },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        printf("radio link check: %s\n", cases[i].name);
        CHECK(radio_link_check(cases[i].now, cases[i].last_acl,
                              cases[i].last_check, cases[i].unanswered) == cases[i].want);
    }
    CHECK(radio_link_unanswered_after(3, 0) == 0);
    CHECK(radio_link_unanswered_after(2, 0x0C) == 3);
    CHECK(radio_link_unanswered_after(0, 0x02) == 1);
}

static void rssi_reply_table(void)
{
    static const struct {
        const char *name;
        unsigned char ev[9];
        int len, outstanding;
        unsigned handle;
        int gone;
    } cases[] = {
        { "gone with RSSI byte", { 0x0e, 7, 1, 5, 0x14, 2, 1, 0, 0 }, 9, 1, 1, 1 },
        { "gone without RSSI byte", { 0x0e, 6, 1, 5, 0x14, 2, 1, 0 }, 8, 1, 1, 1 },
        { "success", { 0x0e, 7, 1, 5, 0x14, 0, 1, 0, 0 }, 9, 1, 1, 0 },
        { "another handle", { 0x0e, 7, 1, 5, 0x14, 2, 2, 0, 0 }, 9, 1, 1, 0 },
        { "nothing outstanding", { 0x0e, 7, 1, 5, 0x14, 2, 1, 0, 0 }, 9, 0, 1, 0 },
        { "incomplete reply", { 0x0e, 4, 1, 5, 0x14, 2 }, 6, 1, 1, 0 },
        { "missing handle byte", { 0x0e, 6, 1, 5, 0x14, 2, 1 }, 7, 1, 1, 0 },
        { "truncated declared payload", { 0x0e, 7, 1, 5, 0x14, 2, 1, 0 }, 8, 1, 1, 0 },
        { "garbled payload length", { 0x0e, 4, 1, 5, 0x14, 2, 1, 0, 0 }, 9, 1, 1, 0 },
        { "other event", { 0x0f, 7, 1, 5, 0x14, 2, 1, 0, 0 }, 9, 1, 1, 0 },
        { "other opcode", { 0x0e, 7, 1, 6, 0x14, 2, 1, 0, 0 }, 9, 1, 1, 0 },
        { "no bytes", { 0 }, 0, 1, 1, 0 },
        { "event code only", { 0x0e }, 1, 1, 1, 0 },
        { "disallowed", { 0x0e, 7, 1, 5, 0x14, 0x0c, 1, 0, 0 }, 9, 1, 1, 0 },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        printf("radio RSSI reply: %s\n", cases[i].name);
        CHECK(radio_rssi_reply_gone(cases[i].ev, cases[i].len,
                                   cases[i].outstanding, cases[i].handle) == cases[i].gone);
    }
}

static void host_busy_table(void)
{
    host_t h = { 0 };
    int i;

    CHECK(host_radio_busy(&h) == 0);
    h.pair_until = LONG_MAX;
    CHECK(host_radio_busy(&h) == 1);
    h.pair_until = now_ms() - 1;
    CHECK(host_radio_busy(&h) == 0);
    h.pair_pending_s = 60;
    CHECK(host_radio_busy(&h) == 1);
    h.pair_pending_s = 0;
    h.repage_at = 1;
    CHECK(host_radio_busy(&h) == 1);
    h.repage_at = 0;
    h.defer_at = 1;
    CHECK(host_radio_busy(&h) == 1);
    h.defer_at = 0;
    for (i = 0; i < HOST_MAX_PADS; i++) {
        h.links[i].used = 1;
        CHECK(host_radio_busy(&h) == 1);
        h.links[i].connected = h.links[i].ready = 1;
        CHECK(host_radio_busy(&h) == 1);
        h.links[i].disconnecting = 1;
        CHECK(host_radio_busy(&h) == 1);
        memset(&h.links[i], 0, sizeof h.links[i]);
    }
    CHECK(host_radio_busy(&h) == 0);
}

static void goodbye_table(void)
{
    /* Handle 0x001 and page scan 44 00 22 00 are from the baseline console log. */
    static const struct {
        const char *name;
        radio_goodbye_in in;
        int max, count;
        radio_cmd want[RADIO_CMD_MAX];
    } cases[] = {
        { "nothing in use", { 0 }, RADIO_CMD_MAX, 0, { { 0 } } },
        { "dead with a pad", { .dead = 1, .nhandles = 1, .handles = { 0x001 } },
          RADIO_CMD_MAX, 0, { { 0 } } },
        { "one pad", { .nhandles = 1, .handles = { 0x001 } }, RADIO_CMD_MAX, 1,
          { { 0x0406, { 0x01, 0x00, 0x13 }, 3 } } },
        { "inquiry only", { .inquiring = 1 }, RADIO_CMD_MAX, 1,
          { { 0x0402, { 0 }, 0 } } },
        { "all nine commands",
          { .nhandles = 4, .handles = { 0x001, 0x002, 0x003, 0x004 },
            .inquiring = 1, .le_scanning = 1, .le_create_pending = 1,
            .ps_saved = 1, .ps_orig_iv = 0x0044, .ps_orig_win = 0x0022, .mask_conn = 1 },
          RADIO_CMD_MAX, 9,
          { { 0x0406, { 0x01, 0x00, 0x13 }, 3 },
            { 0x0406, { 0x02, 0x00, 0x13 }, 3 },
            { 0x0406, { 0x03, 0x00, 0x13 }, 3 },
            { 0x0406, { 0x04, 0x00, 0x13 }, 3 },
            { 0x0402, { 0 }, 0 },
            { 0x200C, { 0x00, 0x00 }, 2 },
            { 0x200E, { 0 }, 0 },
            { 0x0C1C, { 0x44, 0x00, 0x22, 0x00 }, 4 },
            { 0x0C01, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F }, 8 } } },
        { "max two leaves the rest untouched",
          { .nhandles = 4, .handles = { 0x001, 0x002, 0x003, 0x004 },
            .inquiring = 1, .le_scanning = 1, .le_create_pending = 1,
            .ps_saved = 1, .ps_orig_iv = 0x0044, .ps_orig_win = 0x0022, .mask_conn = 1 },
          2, 2,
          { { 0x0406, { 0x01, 0x00, 0x13 }, 3 },
            { 0x0406, { 0x02, 0x00, 0x13 }, 3 } } },
        { "zero capacity", { .nhandles = 1, .handles = { 0x001 } },
          0, 0, { { 0 } } },
        { "LE scan only", { .le_scanning = 1 }, RADIO_CMD_MAX, 1,
          { { 0x200C, { 0x00, 0x00 }, 2 } } },
        { "LE create only", { .le_create_pending = 1 }, RADIO_CMD_MAX, 1,
          { { 0x200E, { 0 }, 0 } } },
        { "page activity only",
          { .ps_saved = 1, .ps_orig_iv = 0x0044, .ps_orig_win = 0x0022 },
          RADIO_CMD_MAX, 1, { { 0x0C1C, { 0x44, 0x00, 0x22, 0x00 }, 4 } } },
        { "mask only", { .mask_conn = 1 }, RADIO_CMD_MAX, 1,
          { { 0x0C01, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F }, 8 } } },
        { "two-byte handle", { .nhandles = 1, .handles = { 0x0ABC } },
          RADIO_CMD_MAX, 1, { { 0x0406, { 0xBC, 0x0A, 0x13 }, 3 } } },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        radio_cmd out[RADIO_CMD_MAX + 1], untouched[RADIO_CMD_MAX + 1];
        int j, n;

        printf("radio goodbye: %s\n", cases[i].name);
        memset(out, 0xA5, sizeof out);
        memcpy(untouched, out, sizeof out);
        n = radio_goodbye(&cases[i].in, out, cases[i].max);
        CHECK(n == cases[i].count);
        for (j = 0; j < cases[i].count; j++) {
            CHECK(out[j].op == cases[i].want[j].op);
            CHECK(out[j].len == cases[i].want[j].len);
            CHECK(memcmp(out[j].p, cases[i].want[j].p, (size_t)cases[i].want[j].len) == 0);
        }
        CHECK(memcmp(out + cases[i].count, untouched + cases[i].count,
                     sizeof out - (size_t)cases[i].count * sizeof out[0]) == 0);
    }
}

static void pending_le_table(void)
{
    static const struct {
        const char *name;
        int used, le, connected, create_out;
        long retry_at;
        int pending;
    } cases[] = {
        { "empty host", 0, 0, 0, 0, 0, 0 },
        { "unused LE slot", 0, 1, 0, 1, 0, 0 },
        { "Classic setup", 1, 0, 0, 1, 0, 0 },
        { "connected LE pad", 1, 1, 1, 1, 0, 0 },
        { "LE awaiting command status", 1, 1, 0, 1, 0, 1 },
        { "LE status accepted, event not received", 1, 1, 0, 0, 0, 1 },
        { "LE retry pending", 1, 1, 0, 0, 1, 1 },
    };
    unsigned i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        host_t h = { 0 };
        link_t *l = &h.links[HOST_MAX_PADS - 1];

        l->used = cases[i].used;
        l->le = cases[i].le;
        l->connected = cases[i].connected;
        l->le_s.create_out = cases[i].create_out;
        l->le_s.retry_at = cases[i].retry_at;
        printf("radio LE pending: %s\n", cases[i].name);
        CHECK(le_create_pending(&h) == cases[i].pending);
    }
}

int main(void)
{
    lifecycle_table();
    link_check_table();
    rssi_reply_table();
    host_busy_table();
    goodbye_table();
    pending_le_table();
    printf("radio: %d CHECKs, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
