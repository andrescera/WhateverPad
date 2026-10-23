/* Run the real outer lifecycle without console, filesystem or network access. */
#define main payload_main
#define mkdir fixture_mkdir
#define access fixture_access
#define unlink fixture_unlink
#define syscall fixture_syscall
#define usleep fixture_usleep
#define log_open fixture_log_open
#define now_ms fixture_now_ms
#include "../src/ps5_main.c"
#undef main
#undef mkdir
#undef access
#undef unlink
#undef syscall
#undef usleep
#undef log_open
#undef now_ms

#include "sim.h"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
static web_cfg cfg;
static sim_t *radio[5];
static int opens, commands, installs, removes, icon_checks, pad_polls, failed_polls;
static int raises, restores, init_fail, creds_fail, remove_flag, answer_query;
static int web_starts, web_start_fail, web_lost;
static long elapsed;
static const char *mode = "original";

static void own_path(const char *p) { CHECK(!strncmp(p, STATE_DIR "/", strlen(STATE_DIR) + 1) || !strcmp(p, STATE_DIR)); }
int fixture_mkdir(const char *p, mode_t m) { (void)m; own_path(p); return 0; }
int fixture_log_open(const char *p) { own_path(p); return 1; }
int fixture_unlink(const char *p) { own_path(p); return 0; }
int fixture_access(const char *p, int m)
{
    (void)m; own_path(p);
    if (!strcmp(p, RM_ICON_FLAG) || !strcmp(p, NO_ICON_FLAG)) icon_checks++;
    return !strcmp(p, RM_ICON_FLAG) && remove_flag ? 0 : -1;
}
long fixture_syscall(long n, ...) { (void)n; return 0; }
int fixture_usleep(useconds_t us) { elapsed += us / 1000; return 0; }
long fixture_now_ms(void) { return sim_now() + elapsed; }
int lock_take(const char *p) { own_path(p); return 1; }
void lock_release(void) {}
void notify(const char *fmt, ...) { (void)fmt; }
int ui_console_language(void) { return 1; }
int ui_language_is_spanish(int code) { (void)code; return 0; }
const char *ui_language_tag(int code) { (void)code; return "en"; }
int local_ip(char out[16]) { strcpy(out, "127.0.0.1"); return 1; }
void sysinfo_log(void) {}
int power_state(void) { return POWER_WORKING; }
int apps_install_launcher(void) { installs++; return 0; }
int apps_remove_launcher(void) { removes++; return 0; }

int vpad_init(void) { mode = "raised"; return !init_fail; }
int vpad_creds_raise(void) { raises++; if (creds_fail) return 0; mode = "raised"; return 1; }
int vpad_creds_restore(void) { restores++; if (creds_fail) return 0; mode = "original"; return 1; }
#if WHATEVERPAD_PROBE
const char *vpad_creds_mode(void) { return mode; }
#endif
int vpad_add(int slot) { (void)slot; CHECK(0); return 0; }
void vpad_remove(int slot) { (void)slot; CHECK(0); }
void vpad_update(int slot, const pad_state *s) { (void)slot; (void)s; CHECK(0); }
int vpad_press_ps(int slot, int ms) { (void)slot; (void)ms; CHECK(0); return 0; }
void vpad_poll(long now) { (void)now; pad_polls++; }

web_t *web_start(const web_cfg *wc, int port)
{
    (void)port; cfg = *wc; web_starts++;
    return web_start_fail && web_starts == 1 ? NULL : (web_t *)&cfg;
}
void web_stop(web_t *w) { (void)w; }
int web_alive(const web_t *w, int port)
{
    (void)port;
    return w && !(web_lost && web_starts == 1 && *cfg.bt_state == WEB_BT_FAILED);
}
void web_poll(web_t *w)
{
    if (!w) return;
    if (*cfg.bt_state == WEB_BT_FAILED) {
        failed_polls++;
        if (failed_polls == 3) *cfg.stop = 1;
    }
    if (*cfg.bt_state == WEB_BT_OK) *cfg.stop = 1;
    assert(opens <= 5 && commands <= 15);
}

int hci_usb_chip_vendor(void) { return 0; }
const char *hci_usb_chip_name(void) { return "offline"; }
static int query(void *ctx, unsigned op, const void *p, int n)
{
    commands++;
#if WHATEVERPAD_PROBE
    CHECK(!strcmp(mode, PROBE_CREDS));
    CHECK(op == 0x1005 && commands == 1);
#endif
    return sim_hci(ctx).ops->cmd(ctx, op, p, n);
}
int hci_usb_open(hci_t *out)
{
    static hci_ops ops;
    sim_pad pad = {0};
    assert(opens < 5);
    sim_t *s = radio[opens++] = sim_new(&pad);
    if (!answer_query) sim_go_deaf(s);
    *out = sim_hci(s);
    ops = *out->ops;
    ops.cmd = query;
    out->ops = &ops;
    return 1;
}

static void run_case(int scenario)
{
    unsigned long from = log_seq();
    char logs[16000];
    remove_flag = scenario == 1;
    init_fail = scenario == 2;
    creds_fail = scenario == 3;
    answer_query = scenario == 4;
    web_start_fail = scenario == 5;
    web_lost = scenario == 6;
    attrib_reset();
    CHECK(payload_main() == 0);
    log_since(from, logs, sizeof logs);
#if WHATEVERPAD_PROBE
    CHECK(installs == 0 && removes == 0 && icon_checks == 0);
    CHECK(cfg.retry == NULL);
    CHECK(strstr(logs, "probe profile=" PROBE_ID) != NULL);
    if (scenario < 2 || scenario >= 4) {
        CHECK(opens == 1 && commands == 1);
        CHECK(strstr(logs, "requested=" PROBE_CREDS " actual=" PROBE_CREDS " checked=1") != NULL);
    } else CHECK(opens == 0 && commands == 0);
    if (scenario != 4) {
        CHECK(!g_vpad_ok && !g_host && pad_polls <= 1);
        CHECK(restores >= 1 && failed_polls == 3);
        CHECK(strstr(logs, "probe release credentials_checked=") != NULL);
    }
    if (scenario >= 5) CHECK(web_starts == 2);
#else
    CHECK(installs == !remove_flag && removes == remove_flag);
    CHECK(opens == 5 && commands == 15);
    CHECK(raises == 1 && restores == 2);
#endif
    for (int i = 0; i < opens; i++) {
        CHECK(sim_resources(radio[i]).closes == 1);
        CHECK(!sim_resources(radio[i]).event_reads && !sim_resources(radio[i]).acl_reads);
        sim_free(radio[i]);
    }
    printf("%d main lifecycle checks, %d failed (probe=%d case=%d)\n", checks, failures, WHATEVERPAD_PROBE, scenario);
}

int main(void)
{
    int total = WHATEVERPAD_PROBE ? 7 : 2;
    for (int i = 0; i < total; i++) {
        pid_t child = fork();
        int status;
        assert(child >= 0);
        if (!child) { run_case(i); exit(failures != 0); }
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    printf("%d lifecycle process checks, %d failed (probe=%d)\n", checks, failures, WHATEVERPAD_PROBE);
    return failures != 0;
}
