/* The unchanged ten reference stimuli, with generation-bearing callbacks.
 * The old HearBridge consumer is excluded from this lane's edit permissions. */
#define main existing_host_tests_main
#include "test_host.c"
#undef main

static long ready_at;
static void ready(void *ud, link_ref link, const pad_info *info)
{
    ready_at = sim_now();
    on_connect(ud, link, info);
}

int main(void)
{
    if (!fixture_init()) return 1;
    for (int kind = 0; kind < 2; kind++) for (int scenario = 0; scenario < 5; scenario++) {
        sim_pad p = {0};
        seen s = {0};
        unsigned char addr[6] = {1, 2, 3, 4, 5, 6};
        host_events events = {ready, on_state, on_disconnect, &s, NULL};
        next_db();
        p.mode = SIM_WAIT_PAIRING;
        memcpy(p.addr, addr, 6); p.cod = 0x002508;
        p.vid = kind ? 0x057e : 0x054c; p.pid = kind ? 0x2009 : 0x05c4;
        p.needs_output = 1;
        if (!kind) p.report_len = ds4_report(p.report);
        else {
            p.basic[0] = 0x3f; p.basic[3] = 8; p.basic_len = 12;
            p.report[0] = 0x30; p.report[3] = 8;
            p.report[7] = p.report[10] = 8;
            p.report[8] = p.report[11] = 0x80; p.report_len = 49;
        }
        p.early_cfg = scenario == 1 || scenario == 2;
        p.no_cfg_repeat = scenario == 2;
        p.refuse_count = scenario == 3 ? 2 : scenario == 4 ? -1 : 0;
        sim_t *sim = sim_new(&p);
        attrib_reset();
        host_t *h = sim_open_host(sim, DB, &events);
        CHECK(h != NULL);
        if (!h) { sim_free(sim); fixture_cleanup(); return 1; }
        long start = sim_now();
        ready_at = 0;
        CHECK(host_pair(h, 5));
        run(h, 3000);
        run(h, scenario == 4 ? 22000 : 7000);
        if (scenario != 4) CHECK(s.connects == 1 && s.states > 0);
        else {
            CHECK(s.connects == 0 && sim_link_up(sim));
            CHECK(sim_disconnect_requests(sim) == 0); /* No accepted input, hence no undo freshness. */
        }
        printf("METRIC kind=%d case=%d ready=%ld requests=%d cfg=%d hash=%08x connected=%d\n",
               kind, scenario, ready_at ? ready_at - start : -1L,
               sim_channel_requests(sim), sim_cfg_responses(sim), sim_tx_hash(sim), s.connects);
        host_close(h);
        sim_free(sim);
    }
    fixture_cleanup();
    printf("controller fingerprints: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail != 0;
}
