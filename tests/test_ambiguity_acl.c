/* GPL-3.0-or-later. A deadline crossed while an input packet is handled is named
 * for what it was, not for an earlier, innocent step. */
#define main host_fixture_main
#include "test_host.c"
#undef main

static long skew;
static int armed;
static sim_t *g_sim;
static unsigned char g_frame[79];

static long skewed_clock(void) { return sim_now() + skew; }

/* The product's idle callback is the web page: it takes real time. Here it takes
 * 2.5 s, and an input report is already waiting behind it. */
static void slow_idle(void *ud)
{
    (void)ud;
    if (!armed) return;
    armed = 0;
    sim_inject_acl(g_sim, 0x0042, g_frame, sizeof g_frame);
    skew = 2500;
}

static int drop_next;

static int drop_one_read(void *ctx, unsigned op, const void *p, int n)
{
    if (op == 0x1005 && drop_next) {
        drop_next = 0;
        return 1;                         /* accepted by the transport, never executed */
    }
    return sim_model_command(ctx, op, p, n);
}

int main(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    seen s;
    char json[ATTRIB_EVIDENCE_MAX], again[ATTRIB_EVIDENCE_MAX];
    own_summary summary;
    host_t *h;
    long armed_at;

    if (!fixture_init()) return 1;
    next_db();
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    g_frame[0] = 0xA1;                    /* HID DATA, input */
    memcpy(g_frame + 1, pad.report, 78);

    g_sim = sim_new(&pad);
    sim_output_hooks(g_sim, drop_one_read, NULL);
    attrib_reset();
    {
        host_events ev = { on_connect, on_state, on_disconnect, &s, slow_idle };
        h = sim_open_host(g_sim, DB, &ev);
    }
    CHECK(h != NULL);
    if (!h) return 1;
    host_pair(h, 30);
    run(h, 15000);
    CHECK(s.connects == 1 && s.states > 0 && host_admits(h));

    /* A runtime command that will never be answered, with a live pad streaming. */
    drop_next = 1;
    CHECK(send_cmd(h, 0x1005, NULL, 0) && !drop_next);
    ph_clock_hook = skewed_clock;
    armed_at = now_ms();
    armed = 1;
    for (int i = 0; i < 50 && host_admits(h); i++) host_poll(h, 5);

    own_snapshot(host_ownership(h), &summary);
    CHECK(!host_admits(h) && summary.reply_timeout && summary.failed_opcode == 0x1005);
    CHECK(attrib_evidence_json(json, sizeof json) > 4);
    CHECK(strstr(json, "\"source\":\"dispatch\"") != NULL);
    CHECK(strstr(json, "\"code\":0,") != NULL);          /* an input packet is not an HCI event */
    CHECK(strstr(json, "\"pending\":[{\"opcode\":4101,") != NULL);
    {
        const char *at = strstr(json, "\"timestamp_ms\":");
        long stamped = at ? atol(at + 15) : 0;
        CHECK(stamped >= armed_at + 2500);               /* the dispatch, not the earlier poll */
    }

    /* Cleanup keeps polling; the first report must not change. */
    for (int i = 0; i < 20; i++) host_poll(h, 5);
    CHECK(attrib_evidence_json(again, sizeof again) > 4 && !strcmp(json, again));
    host_close(h);
    sim_free(g_sim);
    ph_clock_hook = sim_now;
    fixture_cleanup();
    printf("ambiguity ACL: policy=%d checks=%d failures=%d\n",
           WHATEVERPAD_ATTRIB_POLICY, g_checks, g_fail);
    return g_fail != 0;
}
