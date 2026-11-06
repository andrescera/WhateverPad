/* Goodbye commands from the console's handle and page-scan activity values. */
#include "../src/radio.h"
#include "../src/host_int.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

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
    goodbye_table();
    pending_le_table();
    printf("radio: %d CHECKs, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
