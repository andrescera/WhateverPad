/* End-to-end runs of the host stack against the simulated controller. */
#include "sim.h"
#include "../src/crc32.h"
#include "../src/host.h"
#include "../src/host_int.h"
#include "../src/log.h"
#include "../src/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sdp_find_ids(const unsigned char *p, int n, uint16_t *vid, uint16_t *pid);

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

typedef struct {
    int connects, disconnects, states, slot;
    pad_info info;
    pad_state last;
} seen;

static void on_connect(void *ud, link_ref ref, const pad_info *info)
{
    seen *s = ud;
    s->connects++;
    s->slot = ref.slot;
    s->info = *info;
}

static void on_state(void *ud, link_ref ref, const pad_state *st)
{
    seen *s = ud;
    (void)ref;
    s->states++;
    s->last = *st;
}

static void on_disconnect(void *ud, link_ref ref)
{
    seen *s = ud;
    (void)ref;
    s->disconnects++;
}

static char fixture_dir[] = "/tmp/whateverpad-host-XXXXXX";
static char DB[128], BAN[128];
static unsigned fixture_case;

static void clear_db(void)
{
    char path[160];
    if (!DB[0]) return;
    unlink(DB);
    snprintf(path, sizeof path, "%s.new", DB);
    unlink(path);
    snprintf(path, sizeof path, "%s.irk", DB);
    unlink(path);
}

static int fixture_init(void)
{
    if (!mkdtemp(fixture_dir)) { perror("mkdtemp"); return 0; }
    snprintf(BAN, sizeof BAN, "%s/ban.txt", fixture_dir);
    printf("FIXTURE dir=%s\n", fixture_dir);
    return 1;
}

static void next_db(void)
{
    clear_db();
    snprintf(DB, sizeof DB, "%s/case-%u.db", fixture_dir, ++fixture_case);
    CHECK(access(DB, F_OK) != 0);
}

static void fixture_cleanup(void)
{
    clear_db();
    unlink(BAN);
    CHECK(rmdir(fixture_dir) == 0);
}

#define RUN_TEST(call) do { next_db(); call; } while (0)

/* A DualShock 4 full report 0x11: cross and dpad-up held, left stick at
 * (0x20, 0xE0), L2 half pressed. */
static int ds4_report(unsigned char *r)
{
    memset(r, 0, 78);
    r[0] = 0x11;
    r[1] = 0xC0;
    r[3] = 0x20; r[4] = 0xE0; r[5] = 0x80; r[6] = 0x80;
    r[7] = 0x20 | 0x00;     /* cross, hat north */
    r[10] = 0x80;
    r[32] = 0x08;           /* battery 8/8, no cable */
    return 78;
}

static void run(host_t *h, int ms)
{
    long end = sim_now() + ms;
    while (sim_now() < end) host_poll(h, 5);
}

static host_t *open_host(sim_t *sim, seen *s)
{
    attrib_reset();
    host_events ev = { on_connect, on_state, on_disconnect, s, NULL };
    return sim_open_host(sim, DB, &ev);
}

static int test_pairing_seed(int drop_every, unsigned seed, int quiet);
static void test_pairing(int drop_every) { test_pairing_seed(drop_every, 0, 0); }

static int test_pairing_seed(int drop_every, unsigned seed, int quiet)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    unsigned char out[128];
    int n;

    int fail_before = g_fail;

    if (!quiet) printf("pairing a DualShock 4 (dropping every %d packets)\n", drop_every);
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
    pad.drop_every = drop_every;
    pad.seed = seed;

    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) return 0;

    host_pair(h, 30);
    run(h, 15000);

    CHECK(s.connects == 1);
    CHECK(s.info.vid == 0x054C && s.info.pid == 0x05C4);
    CHECK(s.info.profile && strcmp(s.info.profile, "DualShock 4") == 0);
    CHECK(sim_sdp_queries(sim) >= 1);
    CHECK(host_paired_count(h) == 1);
    CHECK(sim_outputs(sim) >= 1);           /* switched it to report 0x11 */
    CHECK(s.states > 0);
    CHECK(s.last.buttons & 0x4000);         /* cross */
    CHECK(s.last.buttons & 0x10);           /* up */
    CHECK(s.last.lx == 0x20 && s.last.ly == 0xE0);
    CHECK(s.last.l2 == 0x80 && (s.last.buttons & 0x100));
    CHECK(s.last.has_battery && s.last.battery_pct == 100);

    /* Rumble and light bar reach the pad, with a CRC it accepts. */
    {
        pad_output o = { 200, 100, 1, 2, 3, 1 };
        CHECK(host_set_output(h, s.slot, &o));
        n = sim_last_output(sim, out, sizeof out);
        CHECK(n == 79);
        CHECK(out[0] == 0xA2 && out[1] == 0x11);
        CHECK(out[7] == 100 && out[8] == 200);
        CHECK(out[9] == 1 && out[10] == 2 && out[11] == 3);
        CHECK(crc32_calc(out, 75) == ((uint32_t)out[75] | (uint32_t)out[76] << 8 |
                                      (uint32_t)out[77] << 16 | (uint32_t)out[78] << 24));
    }

    /* Input changes are followed. */
    pad.report[7] = 0x08 | 0x10;            /* square, hat centred */
    sim_set_report(sim, pad.report, pad.report_len);
    run(h, 200);
    CHECK((s.last.buttons & 0x8000) && !(s.last.buttons & 0x4000));
    CHECK(!(s.last.buttons & 0x10));

    sim_disconnect(sim);
    run(h, 100);
    CHECK(s.disconnects == 1);
    CHECK(!host_connected(h, s.slot));
    if (drop_every && !quiet) printf("  (%d packets dropped)\n", sim_dropped(sim));

    host_close(h);
    sim_free(sim);
    return g_fail == fail_before;
}

/* Many runs with different loss patterns: how often does pairing succeed? */
static void test_pairing_stress(void)
{
    int ok = 0, runs = 40, before = g_fail;
    unsigned seed;

    printf("pairing under 1-in-4 packet loss, %d runs\n", runs);
    for (seed = 1; seed <= (unsigned)runs; seed++) ok += test_pairing_seed(4, seed * 7919, 1);
    printf("  %d/%d runs fully passed\n", ok, runs);
    g_fail = before + (ok < runs);
}

static void test_reconnect(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a paired DualShock 4 reconnecting by itself\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;

    /* Without this case's enrollment, no previous test can supply a bond. */
    CHECK(access(DB, F_OK) != 0);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    CHECK(host_paired_count(h) == 0);
    sim_power_on(sim);
    run(h, 3000);
    CHECK(s.connects == 0);
    CHECK(!sim_link_up(sim));
    host_close(h);
    sim_free(sim);

    pad.mode = SIM_WAIT_PAIRING;
    memset(&s, 0, sizeof s);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 30);
    run(h, 15000);
    CHECK(s.connects == 1);
    CHECK(host_paired_count(h) == 1);
    host_close(h);
    sim_free(sim);

    /* Reopen only the bond produced by this case's own pairing. */
    pad.mode = SIM_RECONNECT;
    memset(&s, 0, sizeof s);
    {
        FILE *f = fopen(DB, "rb");
        unsigned char rec[64 + 64];     /* versioned header, then first record */
        int ok = f && fread(rec, 1, sizeof rec, f) == sizeof rec;
        if (f) fclose(f);
        CHECK(ok);
        if (!ok) return;
        sim = sim_new(&pad);
        sim_give_key(sim, rec + 64 + 8);
    }
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }

    sim_power_on(sim);
    run(h, 8000);
    CHECK(s.connects == 1);
    CHECK(sim_sdp_queries(sim) == 0);       /* ids came from the store */
    CHECK(s.states > 0 && (s.last.buttons & 0x4000));

    host_close(h);
    sim_free(sim);
}

/* An earlier run left its link to the pad open on the chip: paging the pad
 * answers "connection already exists" until the stale link is closed. */
static void test_stale_link(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("an unverified old link never triggers a sweep of system handles\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    sim_stale_link(sim, 0x34);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 30000);
    CHECK(sim_stale_cleared(sim) == 0);
    CHECK(sim_disconnect_requests(sim) == 0);
    CHECK(sim_create_requests(sim) == 1);
    CHECK(s.connects == 0);
    host_close(h);
    sim_free(sim);
}

/* An answer to the search that came cut short (no data at all) is not used
 * to call the pad: a complete one is waited for; if none comes within
 * T_EIR_WAIT, the pad is called anyway. */
static void test_empty_answer_waits(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a search answer cut short is not used to call the pad; a complete one is waited for\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    pad.eir_empty_rounds = 1;                   /* the first answer is cut short */
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 40000);
    CHECK(s.connects == 1);
    CHECK(sim_early_calls(sim) == 0);           /* not called from the cut-short answer */
    host_close(h);
    sim_free(sim);

    /* Every answer cut short: after the wait, it is called anyway. */
    unlink(DB);
    memset(&s, 0, sizeof s);
    pad.eir_empty_rounds = 1000;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 50000);
    CHECK(s.connects == 1);
    host_close(h);
    sim_free(sim);
}

/* A DualSense that first answers cut short (no data, so no model) and then
 * completely is recognised and left alone: the call that was pending for it
 * is cancelled, never made. */
static void test_dualsense_pending_cancelled(void)
{
    static const unsigned char addr[6] = { 0x29, 0xEA, 0x5F, 0x3A, 0x47, 0xE8 };
    static const unsigned char eir_ds[] = { 9, 0x10, 0x02, 0x00, 0x4C, 0x05, 0xE6, 0x0C, 0x00, 0x01 };   /* 054c:0ce6 */
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a DualSense answering cut short first is never called once recognised\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x0CE6;
    pad.report_len = ds4_report(pad.report);
    memcpy(pad.eir, eir_ds, sizeof eir_ds);
    pad.eir_len = sizeof eir_ds;
    pad.eir_empty_rounds = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 50000);
    CHECK(sim_calls(sim) == 0);
    CHECK(s.connects == 0);
    host_close(h);
    sim_free(sim);
}

/* «Pair» pressed during a pause for the system: the search waits for the
 * pause to end (nothing is searched blind) and then gets its whole window. */
static void test_pair_waits_for_pause(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    static const unsigned char stranger[10] = { 0x60, 0xB0, 0xB8, 0xD8, 0x17, 0xE4, 0x08, 0x25, 0x00, 0x01 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int yields;

    printf("pairing pressed during a pause waits for it, then searches its whole window\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    yields = sim_yields();
    sim_inject_event(sim, 0x04, stranger, (int)sizeof stranger);   /* a pad of an earlier session calls */
    run(h, 500);
    CHECK(sim_yields() == yields + 1);          /* the pause for the system began */
    host_pair(h, 60);
    CHECK(host_pair_wait_left(h) > 0);
    run(h, 25000);
    CHECK(sim_inquiries(sim) == 0);             /* no blind search during the pause */
    CHECK(s.connects == 0);
    run(h, 6000);                               /* the pause is over */
    CHECK(host_pair_wait_left(h) == 0);
    CHECK(host_pairing_left(h) >= 55);          /* the whole window, from now */
    CHECK(sim_inquiries(sim) >= 1);
    run(h, 15000);
    CHECK(s.connects == 1);
    host_close(h);
    sim_free(sim);
}

/* A pad that does not answer the call (page timeout) is called again, up to
 * REPAGE_TRIES more times, without a new search. */
static void test_repage_after_timeout(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a pad that does not answer the call is called again\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    sim_fail_pages(sim, 2);                     /* the first two calls get no answer */
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 20);
    {
        long end = sim_now() + 30000;
        while (!s.connects && sim_now() < end) host_poll(h, 5);
    }
    CHECK(s.connects == 1);
    CHECK(sim_calls(sim) == 3);                 /* one call and two extra... */
    CHECK(sim_inquiries(sim) <= 2);             /* ...with no new search in between (one more starts once the pad is ready) */
    host_close(h);
    sim_free(sim);
}

/* The console's system can leave the radio listening for calls almost all the
 * time (page scan 334 of 668 slots, interlaced): then a search hears nothing
 * and calls time out. While pairing, the page scan is lightened, and the
 * system's value is put back afterwards. */
static void test_busy_radio_lightened(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    unsigned iv, win;

    printf("with the system's heavy page scan, pairing lightens it and puts it back\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    sim_set_page_scan(sim, 0x029C, 0x014E);
    sim_busy_radio(sim, 1);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    sim_get_page_scan(sim, &iv, &win);
    CHECK(iv == 0x029C && win == 0x014E);       /* untouched until pairing */
    host_pair(h, 60);
    {
        long end = sim_now() + 20000;
        while (!s.connects && sim_now() < end) host_poll(h, 5);
    }
    CHECK(s.connects == 1);
    host_pair(h, 0);                            /* pairing over, the pad still connected */
    {
        long end = sim_now() + 2000;
        while (sim_now() < end) host_poll(h, 5);
    }
    sim_get_page_scan(sim, &iv, &win);
    CHECK(iv == 0x0800 && win == 0x0012);       /* kept light: a connected pad lags otherwise */
    host_close(h);
    sim_get_page_scan(sim, &iv, &win);
    if (WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_SINGLE_PRODUCER)
        CHECK(iv == 0x029C && win == 0x014E);
    else CHECK(iv == 0x0800 && win == 0x0012); /* NO_RESTORE leaves our change alone. */
    sim_free(sim);
}

/* Calls to the console are not taken: the event mask leaves Connection Request
 * out while WhateverPad runs, and puts it back on close. */
static void test_calls_left_to_system(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("calls to the console: as before by default; leaving them out is an experiment, undone on close\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    CHECK(sim_event_mask(sim)[0] == 0xFF);             /* by default, as every earlier version */
    CHECK(host_tune_set(h, "mask_conn", 1));            /* the dialog-mode experiment */
    CHECK((sim_event_mask(sim)[0] & 0x08) == 0);
    CHECK((sim_event_mask(sim)[0] & 0x07) == 0x07);     /* inquiry and connection complete still on */
    host_close(h);
    CHECK(sim_event_mask(sim)[0] == 0xF7);              /* No original mask was captured. */
    host_tune_set(NULL, "mask_conn", 0);
    sim_free(sim);
}

/* Dialog mode: while idle, the host can keep fewer event reads (the rest of
 * the events go to the system); pairing and any command open them all again. */
static void test_idle_reads(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    unsigned char reply[300];
    int rlen = 0;
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("idle event reads: fewer while idle, all while pairing or waiting for a reply\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    CHECK(host_tune_set(h, "idle_reads", 0));
    host_pair(h, 20);
    {
        long end = sim_now() + 20000;
        while (!s.connects && sim_now() < end) host_poll(h, 5);
    }
    CHECK(s.connects == 1);
    CHECK(sim_event_reads(sim) == 40);          /* all while pairing */
    host_pair(h, 0);
    {
        long end = sim_now() + 3000;
        while (sim_now() < end) host_poll(h, 5);
    }
    CHECK(sim_event_reads(sim) == 0);           /* idle: left to the system */
    {
        int before = s.states;
        long end = sim_now() + 1000;
        while (sim_now() < end) host_poll(h, 5);
        CHECK(s.states > before);               /* the pad's input still comes (ACL) */
    }
    CHECK(host_raw_cmd(h, 0x0C1B, NULL, 0, reply, &rlen) && rlen == 5);    /* a command gets its reply */
    host_tune_set(h, "idle_reads", 40);
    host_close(h);
    sim_free(sim);
}

/* A pad that thinks it is still linked calls the console when switched on;
 * its call stays pending and the controller refuses ours (0x0B). It is turned
 * down and called at once, instead of waiting out the 29 s. */
static void test_pad_own_call_turned_down(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a pad whose own call is pending is turned down and called at once\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    sim_pad_calls_pending(sim);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 20);
    {
        long end = sim_now() + 8000;
        while (!s.connects && sim_now() < end) host_poll(h, 5);
    }
    CHECK(s.connects == 0);
    CHECK(sim_rejects(sim) == 0); /* A collision is not evidence of an inbound request. */
    host_close(h);
    sim_free(sim);
}

/* A pad whose own call was just turned down and that then refuses every
 * channel for lack of resources gets a fresh link after three refusals,
 * instead of eight (the 8BitDo, 9 Oct). */
static void test_fresh_link_on_no_resources(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("no resources for a channel on the first link: a fresh link at once\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    pad.refuse_first_link = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    {
        long end = sim_now() + 9000;
        while (!s.connects && sim_now() < end) host_poll(h, 5);
    }
    CHECK(s.connects == 0 && sim_link_up(sim));
    CHECK(sim_disconnect_requests(sim) == 0); /* No accepted profile input: no freshness. */
    host_close(h);
    sim_free(sim);
}

/* A pairing step whose event is lost on the way is answered anyway after
 * T_SSP_STALL, instead of waiting for the whole setup time. */
static void test_pairing_event_lost(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    static const int lost[2] = { 0x31, 0x33 };  /* IO Capability Request, User Confirmation Request */
    int k;

    printf("a lost pairing event is answered anyway: the pairing does not wait the setup time\n");
    for (k = 0; k < 2; k++) {
        sim_pad pad;
        sim_t *sim;
        host_t *h;
        seen s;

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
        sim = sim_new(&pad);
        sim_drop_events(sim, lost[k], 1);
        h = open_host(sim, &s);
        CHECK(h != NULL);
        if (!h) { sim_free(sim); return; }
        host_pair(h, 60);
        run(h, 16000);                          /* well under the 30 s setup time */
        CHECK(s.connects == 1);
        host_close(h);
        sim_free(sim);
    }
}

/* Forgetting a pad, and closing the host, tell the pad to forget this host
 * too (HID virtual cable unplug), so it stops calling the controller later. */
static void test_forget_unplugs(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("forgetting a pad unplugs its virtual cable; closing does not; forget-all empties the store\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 20000);
    CHECK(s.connects == 1);
    CHECK(sim_unplugs(sim) == 0);
    CHECK(host_forget(h, addr) == 1);
    run(h, 2000);
    CHECK(sim_unplugs(sim) == 1);
    CHECK(sim_link_up(sim) ==
          (WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_NO_RESTORE ? 1 : 0));
    CHECK(host_forget(h, addr) == 0);          /* no longer known */
    host_close(h);
    sim_free(sim);

    /* Pair again, then close: the pad is NOT unplugged (it keeps its key and
     * reconnects next time). Then a fresh host forgets what the store holds. */
    memset(&s, 0, sizeof s);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 60);
    run(h, 20000);
    CHECK(s.connects == 1);
    host_close(h);
    CHECK(sim_unplugs(sim) == 0);
    sim_free(sim);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    CHECK(host_forget_all(h) == 1);
    CHECK(host_forget_all(h) == 0);
    CHECK(host_forget(h, addr) == 0);
    host_close(h);
    sim_free(sim);
}

/* The chip's end-of-inquiry event can be lost: the search must carry on by
 * itself (the simulator never sends one). */
static void test_inquiry_watchdog(void)
{
    static const unsigned char addr[6] = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a lost Inquiry terminal cannot authorize another Inquiry\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;               /* no pad in pairing mode: nothing is found */
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    sim_delay_events(sim, 0x01, 1, -1);
    host_pair(h, 60);
    run(h, 50000);
    CHECK(sim_inquiries(sim) == 1);
    CHECK(!host_pair(h, 60));
    host_close(h);
    sim_free(sim);
}

/* A pad that announces itself as a DualSense in its inquiry response is never
 * paired, whatever the console; a DualShock 4 announcing its own model is. */
static void test_dualsense_by_eir(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    static const unsigned char eir_ds[] = { 9, 0x10, 0x02, 0x00, 0x4C, 0x05, 0xE6, 0x0C, 0x00, 0x01 };    /* 054c:0ce6 */
    static const unsigned char eir_edge[] = { 9, 0x10, 0x02, 0x00, 0x4C, 0x05, 0xF2, 0x0D, 0x00, 0x01 }; /* 054c:0df2 */
    static const unsigned char eir_ds4[] = { 9, 0x10, 0x02, 0x00, 0x4C, 0x05, 0xCC, 0x09, 0x00, 0x01 };  /* 054c:09cc */
    static const struct { const unsigned char *eir; int len; int pairs; } cases[] = {
        { eir_ds, sizeof eir_ds, 0 }, { eir_edge, sizeof eir_edge, 0 }, { eir_ds4, sizeof eir_ds4, 1 }, { NULL, 0, 1 },
    };
    unsigned k;

    printf("pads announcing a DualSense model are left alone; others pair as always\n");
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        sim_pad pad;
        sim_t *sim;
        host_t *h;
        seen s;

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
        if (cases[k].eir) { memcpy(pad.eir, cases[k].eir, (size_t)cases[k].len); pad.eir_len = cases[k].len; }
        sim = sim_new(&pad);
        h = open_host(sim, &s);
        CHECK(h != NULL);
        if (!h) { sim_free(sim); return; }
        host_pair(h, 30);
        run(h, 30000);
        CHECK(s.connects == cases[k].pairs);
        CHECK(sim_link_up(sim) == cases[k].pairs);
        host_close(h);
        sim_free(sim);
    }
}

/* A banned address (ban.txt) is never looked at by the search; any other pad
 * pairs as always. */
static void test_ban_list(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };   /* shown 06:05:04:03:02:01 */
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    FILE *f;

    printf("a banned address is never looked at; others pair as always\n");
    unlink(DB);
    f = fopen(BAN, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs("# the console's own DualSense\n0A:0B:0C:0D:0E:0F\n06:05:04:03:02:01  # banned test pad\n", f);
    fclose(f);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 20);
    run(h, 12000);
    CHECK(s.connects == 0 && host_paired_count(h) == 0);        /* never connected to */
    CHECK(!sim_link_up(sim));
    host_close(h);
    sim_free(sim);

    unlink(BAN);                                              /* without the ban the same pad pairs */
    unlink(DB);
    memset(&s, 0, sizeof s);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 20);
    run(h, 12000);
    CHECK(s.connects == 1);
    host_close(h);
    sim_free(sim);
}

static void test_unknown_pad_refused(void)
{
    static const unsigned char addr[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a stranger connecting is ignored\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    sim_power_on(sim);
    run(h, 3000);
    CHECK(s.connects == 0);
    CHECK(!sim_link_up(sim));
    host_close(h);
    sim_free(sim);
}

/* A device that is not a paired pad calling this controller (a DualSense
 * reconnecting to the console) is left alone: not taken, and not turned down
 * either, also while pairing is open. */
static void test_unknown_call_left_alone(void)
{
    static const unsigned char addr[6] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int round, yields0 = sim_yields();

    printf("an unpaired caller is neither taken nor turned down\n");
    for (round = 0; round < 2; round++) {
        next_db();
        memset(&pad, 0, sizeof pad);
        memset(&s, 0, sizeof s);
        pad.mode = SIM_RECONNECT;
        memcpy(pad.addr, addr, 6);
        pad.cod = 0x002508;
        sim = sim_new(&pad);
        h = open_host(sim, &s);
        CHECK(h != NULL);
        if (!h) { sim_free(sim); return; }
        if (round == 1) host_pair(h, 60);
        sim_power_on(sim);
        run(h, 5000);
        CHECK(s.connects == 0);
        CHECK(!sim_link_up(sim));
        CHECK(sim_rejects(sim) == 0);
        CHECK(sim_yields() - yields0 == 1);           /* stood aside for the system, but not while searching (round 1) */
        run(h, 40000);                      /* the pause and after: not taken for a dead chip */
        CHECK(!host_transport_lost(h));
        host_close(h);
        sim_free(sim);
    }
}

/* A pad that only answers SDP in two steps (record handles first, then each
 * record's attributes) is still identified: the host falls back to that. */
static void test_sdp_two_step(void)
{
    static const unsigned char addr[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a pad that only answers SDP in two steps is still identified\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    pad.sdp_two_step_only = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    host_pair(h, 30);
    run(h, 30000);
    CHECK(s.connects == 1);
    CHECK(s.info.vid == 0x054C && s.info.pid == 0x05C4);
    host_close(h);
    sim_free(sim);
}

static void test_transport_lost(void)
{
    static const unsigned char addr[6] = { 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a controller that stops answering is noticed\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    run(h, 30000);
    CHECK(!host_transport_lost(h));     /* idle but answering probes */
    sim_go_deaf(sim);
    run(h, 30000);
    CHECK(!host_pair(h, 30)); /* All three unanswered liveness sends close admission. */
    host_close(h);
    sim_free(sim);
}

/* A pad no profile knows, paired and read through its HID descriptor. */
static const unsigned char k_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02,
    0x09, 0xC5, 0x09, 0xC4, 0x81, 0x02,
    0xC0,
};

static void test_generic_pad(void)
{
    static const unsigned char addr[6] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("pairing an unknown pad: generic profile from its descriptor\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x2DC8;
    pad.pid = 0x6001;
    memcpy(pad.desc, k_desc, sizeof k_desc);
    pad.desc_len = (int)sizeof k_desc;
    /* left stick right, hat south, button 1 (A), accelerator half */
    {
        static const unsigned char r[9] = { 0xFF, 0x80, 0x80, 0x80, 0x04, 0x01, 0x00, 0x00, 0x80 };
        memcpy(pad.report, r, sizeof r);
        pad.report_len = (int)sizeof r;
    }
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 10000);
    CHECK(s.connects == 1);
    CHECK(s.info.profile && strcmp(s.info.profile, "Generic HID") == 0);
    CHECK(sim_sdp_continuations(sim) >= 1);     /* the descriptor came in pieces */
    CHECK(s.states > 0);
    CHECK(s.last.lx == 255 && s.last.ly == 128);
    CHECK((s.last.buttons & 0x40) && (s.last.buttons & 0x4000));    /* down, cross */
    CHECK(s.last.r2 == 0x80 && (s.last.buttons & 0x200));
    host_close(h);
    sim_free(sim);
}

/* A Switch Pro controller: simple mode until its wake-up sequence. */
static void test_switch_pad(void)
{
    static const unsigned char addr[6] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x46 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("pairing a Switch Pro controller: woken up into its full report\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x057E;
    pad.pid = 0x2009;
    pad.needs_output = 1;
    pad.basic[0] = 0x3F;                    /* simple mode: nothing pressed */
    pad.basic[3] = 8;
    pad.basic_len = 12;
    memset(pad.report, 0, 49);
    pad.report[0] = 0x30;
    pad.report[3] = 0x08;                   /* A */
    pad.report[6] = 0x00; pad.report[7] = 0x08; pad.report[8] = 0x80;  /* centred */
    pad.report[9] = 0x00; pad.report[10] = 0x08; pad.report[11] = 0x80;
    pad.report_len = 49;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 10000);
    CHECK(s.connects == 1);
    CHECK(s.info.profile && strcmp(s.info.profile, "Switch Pro") == 0);
    CHECK(sim_outputs(sim) >= 4);           /* the whole wake-up sequence */
    CHECK(s.last.buttons & 0x2000);         /* A, from the full report: circle */
    CHECK(s.last.lx == 128 && s.last.ly == 128);
    host_close(h);
    sim_free(sim);
}

/* A Switch Pro that misses the quick wake-up rounds (it ignores the first 30
 * output reports) is asked again, more slowly, until it switches to its full
 * report: it does not stay in its simple report for good. */
static void test_switch_pad_late(void)
{
    static const unsigned char addr[6] = { 0x41, 0x42, 0x43, 0x44, 0x45, 0x47 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("a Switch Pro that ignores the first wake-up rounds is asked until it switches\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x057E;
    pad.pid = 0x2009;
    pad.needs_output = 1;
    pad.ignore_outputs = 30;
    pad.basic[0] = 0x3F;
    pad.basic[3] = 8;
    pad.basic_len = 12;
    memset(pad.report, 0, 49);
    pad.report[0] = 0x30;
    pad.report[3] = 0x08;                   /* A */
    pad.report[6] = 0x00; pad.report[7] = 0x08; pad.report[8] = 0x80;
    pad.report[9] = 0x00; pad.report[10] = 0x08; pad.report[11] = 0x80;
    pad.report_len = 49;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 40000);
    CHECK(s.connects == 1);
    CHECK(sim_outputs(sim) > 30);           /* kept asking past the quick rounds */
    CHECK(s.last.buttons & 0x2000);         /* and the full report finally came */
    host_close(h);
    sim_free(sim);
}

/* What the host changes in the shared controller, it puts back on close,
 * unless someone else changed it meanwhile. */
static void test_restore(void)
{
    static const unsigned char addr[6] = { 0x61, 0x62, 0x63, 0x64, 0x65, 0x66 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;

    printf("the controller's page scan is put back on close, unless changed by another\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    CHECK(h && sim_scan(sim) == 0x02);
    host_close(h);
    CHECK(sim_scan(sim) ==
          (WHATEVERPAD_ATTRIB_POLICY == OWN_POLICY_SINGLE_PRODUCER ? 0x00 : 0x02));

    h = open_host(sim, &s);
    sim_set_scan(sim, 0x03);            /* the system turns on inquiry scan too */
    host_close(h);
    CHECK(sim_scan(sim) == 0x03);       /* left alone */
    sim_free(sim);
}

/* Garbage on every channel of a working link, and garbage events: the
 * host must neither fault nor lose its pad over it. */
static void test_garbage(void)
{
    static const unsigned char addr[6] = { 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
    static const unsigned cids[] = { 0x0001, 0x0040, 0x0041, 0x0042, 0x0099 };
    static const unsigned char codes[] = { 0x03, 0x05, 0x06, 0x08, 0x0E, 0x0F, 0x13, 0x17, 0x18, 0x22, 0x2F, 0x31, 0x3E };
    unsigned seed = 777;
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int i;

    printf("garbage frames and events on a working Classic link\n");
    unlink(DB);
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    pad.report_len = ds4_report(pad.report);
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = open_host(sim, &s);
    host_pair(h, 30);
    run(h, 8000);
    CHECK(s.connects == 1);
    for (i = 0; i < 20000; i++) {
        unsigned char junk[300];
        int n, k;
        seed = seed * 1103515245u + 12345u;
        n = (int)((seed >> 8) % 280);
        for (k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            junk[k] = (unsigned char)(seed >> 16);
        }
        if (i % 4 == 3) sim_inject_event(sim, codes[(seed >> 4) % sizeof codes], junk, n < 255 ? n : 255);
        else sim_inject_acl(sim, cids[(seed >> 4) % 5], junk, n);
        if (i % 16 == 0) host_poll(h, 1);
    }
    run(h, 500);
    printf("  connects %d, disconnects %d, connected now %d\n", s.connects, s.disconnects,
           host_connected(h, s.slot));
    /* Garbage events can end the link (a Disconnection Complete for our
     * handle looks like any other); the books must still balance. */
    CHECK(host_connected(h, s.slot) == (s.connects - s.disconnects == 1));
    host_close(h);
    sim_free(sim);
}

/* A controller that never answers: the host gives up at once, and asks the
 * transport once why. */
static void test_dead_controller(void)
{
    static const unsigned char addr[6] = { 0x91, 0x92, 0x93, 0x94, 0x95, 0x96 };
    sim_pad pad;
    sim_t *sim;
    host_t *h;
    seen s;
    int before = sim_diag_calls();

    printf("a controller that does not answer: host_open fails and the transport reports\n");
    memset(&pad, 0, sizeof pad);
    memset(&s, 0, sizeof s);
    pad.mode = SIM_RECONNECT;
    memcpy(pad.addr, addr, 6);
    sim = sim_new(&pad);
    sim_go_deaf(sim);
    h = open_host(sim, &s);
    CHECK(h == NULL);
    CHECK(sim_diag_calls() == before + 1);
    sim_free(sim);
}

static int without_local_identity(void *ctx, unsigned op, const void *p, int len)
{
    if (op == OP_READ_BD_ADDR) return 0;
    return sim_model_command(ctx, op, p, len);
}

static void test_store_admission(void)
{
    sim_pad pad = {0};
    sim_t *sim = sim_new(&pad);
    seen s = {0};
    host_t *h;
    db_rec *r;
    FILE *f;
    char got[4];
    static const unsigned char addr[6] = {7, 8, 9, 10, 11, 12};
    host_events ev = { on_connect, on_state, on_disconnect, &s, NULL };

    printf("durable admission waits for commit and a successful local identity read\n");
    sim_output_hooks(sim, without_local_identity, NULL);
    CHECK(sim_open_host(sim, DB, &ev) == NULL);
    CHECK(access(DB, F_OK) != 0);
    sim_output_hooks(sim, NULL, NULL);
    h = open_host(sim, &s);
    CHECK(h != NULL);
    if (!h) { sim_free(sim); return; }
    r = db_put(h, addr, 0, 0);
    CHECK(r != NULL);
    if (!r) { host_close(h); sim_free(sim); return; }
    memset(r->key, 0x41, 16); r->complete = 1;
    CHECK(host_paired_count(h) == 0);
    CHECK(db_save(h) == BOND_COMMITTED && host_paired_count(h) == 1);
    f = fopen(DB, "wb"); CHECK(f != NULL);
    if (!f) { host_close(h); sim_free(sim); return; }
    CHECK(fwrite("bad", 1, 3, f) == 3 && fclose(f) == 0);
    r = db_put(h, addr, 0, 0);
    CHECK(r != NULL);
    if (r) r->key[0]++;
    CHECK(db_save(h) == BOND_INVALID);
    CHECK(host_paired_count(h) == 1 && h->store.records[0].key[0] == 0x41);
    CHECK(host_forget(h, addr) == 0 && host_paired_count(h) == 1);
    f = fopen(DB, "rb"); CHECK(f != NULL);
    if (f) {
        CHECK(fread(got, 1, sizeof got, f) == 3 && !memcmp(got, "bad", 3));
        CHECK(fclose(f) == 0);
    }
    host_close(h);
    sim_free(sim);
}

static void test_sdp_parse(void)
{
    /* As a real pad answers: {{0x0200 ver, 0x0201 vid, 0x0202 pid}} */
    static const unsigned char list[] = {
        0x35, 0x14, 0x35, 0x12,
        0x09, 0x02, 0x00, 0x09, 0x01, 0x03,
        0x09, 0x02, 0x01, 0x09, 0x05, 0x4C,
        0x09, 0x02, 0x02, 0x09, 0x0C, 0xE6,
    };
    uint16_t vid = 0, pid = 0;

    printf("SDP Device ID parsing\n");
    CHECK(sdp_find_ids(list, sizeof list, &vid, &pid));
    CHECK(vid == 0x054C && pid == 0x0CE6);
    CHECK(!sdp_find_ids(list, 10, &vid, &pid));
}


/* Inject discovery independently of automatic inquiry answers, including
 * late arrivals and identity changes for the same address. */
static void audit_result(unsigned char *r, const sim_pad *pad)
{
    memset(r, 0, 255);
    r[0] = 1;
    memcpy(r + 1, pad->addr, 6);
    r[7] = 1;
    r[9] = 0x08; r[10] = 0x25;
}

static void test_discovery_regressions(void)
{
    int which;
    printf("late/invalid discovery, cancellation, and remembered system identity\n");
    for (which = 0; which < 8; which++) {
        sim_pad pad = {0};
        seen s = {0};
        sim_t *sim;
        host_t *h;
        unsigned char r[255];
        static const unsigned char addr[6] = {1, 2, 3, 4, 5, 6};
        static const unsigned char ds[] = {9, 0x10, 2, 0, 0x4c, 5, 0xe6, 0x0c, 0, 1};
        next_db();
        pad.mode = SIM_RECONNECT; /* no automatic discovery replies */
        memcpy(pad.addr, addr, 6);
        pad.cod = 0x002508; pad.vid = 0x054c; pad.pid = 0x05c4;
        pad.report_len = ds4_report(pad.report);
        pad.needs_output = 1;
        sim = sim_new(&pad);
        h = open_host(sim, &s);
        CHECK(h != NULL);
        if (!h) { sim_free(sim); continue; }
        host_pair(h, 60);
        audit_result(r, &pad);
        if (which == 0) {
            run(h, 55000);
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 4500);
            CHECK(sim_calls(sim) == 1); /* before the 60-second deadline */
            CHECK(sim_inquiry_during_setup(sim) == 0);
        } else if (which == 1) {
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 500);
            memcpy(r + 15, ds, sizeof ds);
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 500);
            memset(r + 15, 0, 240);
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 21000);
            CHECK(sim_calls(sim) == 0);
            host_pair(h, 60); /* identity also survives a new search */
            r[15] = 5; r[16] = 9; memcpy(r + 17, "Pad1", 4);
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 500);
            CHECK(sim_calls(sim) == 0);
        } else if (which == 2) {
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 500);
            host_pair(h, 0); /* cancel without a poll in between */
            host_pair(h, 60);
            run(h, 21000);
            CHECK(sim_calls(sim) == 0);
        } else {
            /* Invalid count, incomplete extended result, reserved scan mode,
             * malformed trailing EIR field, and short RSSI result. */
            r[15] = 5; r[16] = 9; memcpy(r + 17, "Pad1", 4);
            if (which == 3) r[0] = 2;
            if (which == 5) r[7] = 3;
            if (which == 6) r[21] = 255;
            sim_inject_event(sim, which == 7 ? 0x22 : 0x2f, r,
                             which == 4 ? 21 : which == 7 ? 14 : 255);
            run(h, 22000);
            CHECK(sim_calls(sim) == 0);
            CHECK(host_paired_count(h) == 0);
            /* A following valid complete result must still work immediately. */
            audit_result(r, &pad);
            r[15] = 5; r[16] = 9; memcpy(r + 17, "Pad1", 4);
            sim_inject_event(sim, 0x2f, r, 255);
            run(h, 500);
            CHECK(sim_calls(sim) == 1);
        }
        host_close(h);
        sim_free(sim);
    }
}

static void test_diagnostics_do_not_expose_secrets(void)
{
    static const unsigned char address[] = {0x91,0xa2,0xb3,0xc4,0xd5,0xe6};
    sim_pad p = {0};
    memcpy(p.addr, address, sizeof address);
    p.cod = 0x002508;
    p.vid = 0x054c; p.pid = 0x05c4;
    p.report_len = ds4_report(p.report);
    p.eir[0] = 7; p.eir[1] = 0x1b;
    memcpy(p.eir + 2, address, sizeof address);
    p.eir_len = 8;
    sim_t *sim = sim_new(&p);
    unsigned long from = log_seq();
    host_t *h = open_host(sim, &(seen){0});
    CHECK(h != NULL);
    if (h) {
        unsigned char command[28] = {0}, event[23] = {0};
        unsigned char reply[300];
        int length, trace = g_tune.trace;
        CHECK(host_pair(h, 30));
        run(h, 1000);
        memcpy(command, address, 6);
        for (int i = 0; i < 16; i++) command[6 + i] = (unsigned char)(0xe0 + i);
        host_raw_cmd(h, 0x040b, command, 22, reply, &length);
        command[6] = 16;
        host_raw_cmd(h, 0x040d, command, 23, reply, &length);
        host_raw_cmd(h, 0x2019, command, 28, reply, &length);
        CHECK(link_by_peer(h, address, 0, 0) || link_new(h, address, 0, 0));
        memcpy(event, address, 6);
        for (int i = 0; i < 16; i++) event[6 + i] = (unsigned char)(0xe0 + i);
        g_tune.trace = 1;
        sim_inject_event(sim, 0x18, event, sizeof event);
        host_poll(h, 0);
        g_tune.trace = trace;
        host_close(h);
    }
    char captured[32768];
    CHECK(log_since(from, captured, sizeof captured) > from);
    static const char *const forbidden[] = {
        "e0 e1 e2", "e1 e2 e3", "e0e1e2",
        "91 a2 b3", "91:A2:B3", "E6:D5:C4", "e6 d5 c4",
        "11:22:33", "66 55 44", "665544"
    };
    for (unsigned i = 0; i < sizeof forbidden / sizeof *forbidden; i++)
        CHECK(strstr(captured, forbidden[i]) == NULL);
    sim_free(sim);
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--reconnect") != 0)) {
        fprintf(stderr, "usage: %s [--reconnect]\n", argv[0]);
        return 1;
    }
    if (!fixture_init()) return 1;
    if (argc == 2) {
        RUN_TEST(test_reconnect());
    } else {
        RUN_TEST(test_diagnostics_do_not_expose_secrets());
        RUN_TEST(test_store_admission());
        RUN_TEST(test_discovery_regressions());
        RUN_TEST(test_sdp_parse());
        RUN_TEST(test_pairing(0));
        RUN_TEST(test_reconnect());
        RUN_TEST(test_ban_list());
        RUN_TEST(test_forget_unplugs());
        RUN_TEST(test_empty_answer_waits());
        RUN_TEST(test_repage_after_timeout());
        RUN_TEST(test_busy_radio_lightened());
        RUN_TEST(test_calls_left_to_system());
        RUN_TEST(test_idle_reads());
        RUN_TEST(test_pad_own_call_turned_down());
        RUN_TEST(test_fresh_link_on_no_resources());
        RUN_TEST(test_pairing_event_lost());
        RUN_TEST(test_dualsense_pending_cancelled());
        RUN_TEST(test_pair_waits_for_pause());
        RUN_TEST(test_inquiry_watchdog());
        RUN_TEST(test_dualsense_by_eir());
        RUN_TEST(test_unknown_call_left_alone());
        RUN_TEST(test_pairing(7));
        RUN_TEST(test_stale_link());
        RUN_TEST(test_unknown_pad_refused());
        RUN_TEST(test_transport_lost());
        RUN_TEST(test_restore());
        RUN_TEST(test_dead_controller());
        RUN_TEST(test_garbage());
        RUN_TEST(test_generic_pad());
        RUN_TEST(test_switch_pad());
        RUN_TEST(test_switch_pad_late());
        RUN_TEST(test_sdp_two_step());
        RUN_TEST(test_pairing_stress());
    }
    fixture_cleanup();
    printf("%d host checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
