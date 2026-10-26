/* Reuse the real TCP server and simulated host, without the fixture's dispatch. */
#define main web_fixture_main
#include "test_web.c"
#undef main

static int checking_raw;

static void product_idle(void *ud)
{
    (void)ud;
    web_run(g_web);
    if (checking_raw) CHECK(g_ps_calls == 0);
}

static int product_press_ps(int slot)
{
    int result = fake_press_ps(slot), before = g_ps_calls;
    web_run(g_web);
    CHECK(g_ps_calls == before);
    return result;
}

static int product_reply(int fd, char *out, int max, int code)
{
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    /* Like the product: poll/drain, then write and finalize Retry on the next turn. */
    web_run(g_web);
    web_run(g_web);
    out[0] = 0;
    int n = 0;
    if (poll(&ready, 1, 0) > 0) n = response(&ready, out, max);
    CHECK(complete_response(out, n, code));
    close(fd);
    return n;
}

int main(void)
{
    sim_pad pad = {.mode = SIM_WAIT_PAIRING, .cod = 0x002508,
        .vid = 0x054c, .pid = 0x05c4, .addr = {1, 2, 3, 4, 5, 6}};
    sim_t *sim = sim_new(&pad);
    host_events events = {.idle = product_idle};
    char out[2048];
    signal(SIGPIPE, SIG_IGN);
    g_port = 20000 + (int)(getpid() % 20000);
    unlink("build/test_web_product.db");
    attrib_reset();
    g_host = sim_open_host(sim, "build/test_web_product.db", &events);
    CHECK(g_host != NULL);
    if (!g_host) return 1;
    /* Prepare a real stored pad independently of the web action under test. */
    CHECK(host_pair(g_host, 60));
    long end = sim_now() + 8000;
    while (sim_now() < end) host_poll(g_host, 5);
    CHECK(host_paired_count(g_host) == 1);
    CHECK(host_pair(g_host, 0));
    CHECK(host_pairing_left(g_host) == 0);
    web_cfg cfg = {.host = &g_host, .retry = &g_retry, .bt_state = &g_bt_state,
        .press_ps = product_press_ps, .ps_combo_set = matrix_combo_set};
    g_web = web_start(&cfg, PORT);
    CHECK(g_web != NULL);
    if (!g_web) { host_close(g_host); sim_free(sim); return 1; }

    product_reply(post_pending("/api/pair"), out, sizeof out, 200);
    CHECK(host_pairing_left(g_host) > 0); /* host_pair actually ran. */
    CHECK(strstr(out, "\"ok\":true") != NULL);

    int raw = post_pending("/api/hci?op=0C1B");
    int ps = post_pending("/api/ps?slot=1");
    checking_raw = 1;
    product_reply(raw, out, sizeof out, 200);
    checking_raw = 0;
    CHECK(strstr(out, "\"reply\":\"0000081200\"") != NULL);
    product_reply(ps, out, sizeof out, 200);
    CHECK(g_ps_calls == 1 && g_ps_slot == 0);
    web_run(g_web);
    CHECK(g_ps_calls == 1); /* Nested idle and callback drains never repeat it. */

    product_reply(post_pending("/api/pscombo?slot=1&combo=none"), out, sizeof out, 200);
    CHECK(combo_calls == 1);
    product_reply(post_pending("/api/tune?inq_len=4"), out, sizeof out, 200);
    char tune[256];
    host_tune_json(tune, sizeof tune);
    CHECK(strstr(tune, "\"inq_len\":4") != NULL);
    product_reply(post_pending("/api/tune?inq_len=8"), out, sizeof out, 200);
    product_reply(post_pending("/api/forget?addr=06:05:04:03:02:01"), out, sizeof out, 200);
    CHECK(host_paired_count(g_host) == 0);

    CHECK(host_pair(g_host, 0));
    own_release(host_ownership(g_host), SESSION_STOP, (uint64_t)sim_now());
    CHECK(host_pairing_left(g_host) == 0);
    product_reply(post_pending("/api/pair"), out, sizeof out, 503);
    CHECK(strstr(out, "\"error\":\"pairing refused\"") != NULL);
    CHECK(host_pairing_left(g_host) == 0);

    host_t *saved = g_host;
    g_host = NULL; g_bt_state = WEB_BT_FAILED;
    product_reply(post_pending("/api/retry"), out, sizeof out, 200);
    CHECK(g_retry == 1);
    g_host = saved;
    host_close(g_host);
    web_stop(g_web);
    CHECK(sim_resources(sim).closes == 1);
    sim_free(sim);
    unlink("build/test_web_product.db");
    printf("%d product web checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
