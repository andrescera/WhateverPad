/* GPL-3.0-or-later. Failure evidence survives product close and HTTP serialization. */
#define RUNTIME_FAILURE_FIXTURE
#include "test_runtime_failure.c"

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    REQUIRE(log_open("/dev/null"));
    snprintf(directory, sizeof directory, "/tmp/ambiguity-web-XXXXXX");
    REQUIRE(mkdtemp(directory) != NULL);
    snprintf(db, sizeof db, "%s/bonds", directory);
    g_tune.ps_light = 0;
    begin(0);
    web_stop(f.web);
    g_host = f.host;
    g_vpad_ok = 1;
    g_bt_state = WEB_BT_OK;
    web_cfg cfg = {.host = &g_host, .stop = &g_web_stop, .retry = &g_retry,
                  .bt_state = &g_bt_state, .bt_reason = g_bt_reason};
    g_web = web_start(&cfg, 0);
    REQUIRE(g_web != NULL);
    REQUIRE(sim_foreign_command(f.classic, 0x1001, NULL, 0));
    host_poll(g_host, 4);
    REQUIRE(host_transport_lost(g_host));
    bt_lost(); f.host = NULL;
    char out[20000];
    reply(http_request("GET", "/api/state"), out, sizeof out, 200);
    REQUIRE(strstr(out, "\"evidence\":{") != NULL);
    REQUIRE(strstr(out, "\"source\":\"event\"") != NULL);
    REQUIRE(strstr(out, "\"code\":14") && strstr(out, "\"opcode\":4097"));
    REQUIRE(strstr(out, "\"recent\":[") && strstr(out, "\"pending\":["));
    close_fixture(1);
    log_close();
    printf("ambiguity HTTP: checks=%u failures=0\n", checks);
    return 0;
}
