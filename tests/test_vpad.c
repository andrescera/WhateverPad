/* vpad_init against stand-ins for the console's libraries, to check the
 * order of the libScePad calls and what is fatal. Compiled on the host from
 * the console source itself. */
#include "../src/ps5_vpad.h"
#include "../src/util.h"
#include "../src/web.h"


#include <stdint.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int g_fail, pending, strict_mode;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void expected_failure(int todo, const char *name, int desired, const char *effect)
{
    printf("%s expected-failing-until-todo-%d %s\n", desired ? "XPASS" : "XFAIL", todo, name);
    if (!desired) {
        pending++;
        if (strict_mode) printf("FAIL %s: %s (todo-%d)\n", name, effect, todo);
    }
}
#define EFFECT(todo, name, c) expected_failure(todo, name, (c), #c)

/* What the stand-ins were asked, in order. */
static char g_calls[64];
static int g_inited, g_priv_result_after_init, g_init_result;
static uint64_t g_authid = 0x4800000000000001ull;
static uint8_t g_caps[16];
static int log_pipe[2], adds, binds, deletes, frames;
static int fail_library, fail_symbol, add_result, bind_result;
static int32_t deleted_handle;
static int web_port;

/* Only web.c's bind is redirected; HTTP and vendor-log sockets stay separate. */
int test_vpad_web_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    int result = bind(fd, addr, len);
    if (result == 0) {
        struct sockaddr_in bound;
        socklen_t n = sizeof bound;
        CHECK(getsockname(fd, (struct sockaddr *)&bound, &n) == 0);
        web_port = ntohs(bound.sin_port);
    }
    return result;
}

int test_vpad_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    return dup(log_pipe[0]);
}

int test_vpad_connect(int fd, const struct sockaddr *addr, socklen_t n)
{
    (void)fd; (void)addr; (void)n;
    return 0;
}

static long g_now = 100000;
static long fake_clock(void) { return g_now; }

static void note(char c) { size_t n = strlen(g_calls); if (n < sizeof g_calls - 1) g_calls[n] = c; }

#define SCE_PAD_ERROR_NOT_INITIALIZED ((int32_t)0x80920005)

int32_t sceUserServiceInitialize(void *p) { (void)p; note('u'); return 0; }
static int32_t g_init_user = 0x10000000, g_fore_user = 0x10000000;
static int32_t g_login[4] = { 0x10000000, -1, -1, -1 };
int32_t sceUserServiceGetInitialUser(int32_t *u) { *u = g_init_user; return 0; }
int32_t sceUserServiceGetForegroundUser(int32_t *u) { *u = g_fore_user; return 0; }
int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]) { memcpy(list, g_login, sizeof g_login); return 0; }
int32_t scePadInit(void) { note('I'); if (g_init_result == 0) g_inited = 1; return g_init_result; }
int32_t scePadSetProcessPrivilege(int32_t v)
{
    (void)v;
    note('P');
    return g_inited ? g_priv_result_after_init : SCE_PAD_ERROR_NOT_INITIALIZED;
}
static int g_pad_opened, g_open_result = 7, g_closed;
static unsigned g_pad_buttons;
static int g_pad_connected = 1;
int32_t scePadGetHandle(int32_t u, int32_t t, int32_t i)
{
    (void)u; (void)t; (void)i;
    return g_pad_opened ? 7 : (int32_t)0x80920008;      /* NO_HANDLE until opened */
}
static int32_t g_open_user;
int32_t scePadOpen(int32_t u, int32_t t, int32_t i, const void *p)
{
    int k, signed_in = 0;
    (void)t; (void)i; (void)p;
    g_open_user = u;
    for (k = 0; k < 4; k++) if (g_login[k] == u) signed_in = 1;
    if (!signed_in) return (int32_t)0x809B0081;         /* SCE_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN */
    if (g_open_result >= 0) g_pad_opened = 1;
    return g_open_result;
}
int32_t scePadClose(int32_t h) { (void)h; g_closed++; g_pad_opened = 0; return 0; }
int32_t scePadReadState(int32_t h, void *d)
{
    /* ScePadData: buttons at 0, connected at offset 76 (after the touch data) */
    unsigned char *p = d;
    if (h != 7) return (int32_t)0x80920003;
    memset(p, 0, 120);
    memcpy(p, &g_pad_buttons, 4);
    p[76] = (unsigned char)g_pad_connected;
    return 0;
}
int32_t scePadVirtualDeviceAddDevice(void *p, int32_t t)
{
    static const char candidate[] =
        "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x11112222][type:1][subType:22][capabilityBattery:0]\n";
    (void)p; (void)t;
    adds++;
    if (!add_result) CHECK(write(log_pipe[1], candidate, sizeof candidate - 1) == sizeof candidate - 1);
    return add_result;
}
int32_t scePadVirtualDeviceInsertData(int32_t h, const void *d) { (void)h; (void)d; frames++; return 0; }
int32_t scePadVirtualDeviceDeleteDevice(int32_t h) { deleted_handle = h; deletes++; return 0; }
uint64_t kernel_get_ucred_authid(pid_t pid) { (void)pid; return g_authid; }
int32_t kernel_set_ucred_authid(pid_t pid, uint64_t a) { (void)pid; g_authid = a; return 0; }
int32_t kernel_get_ucred_caps(pid_t pid, uint8_t c[16]) { (void)pid; memcpy(c, g_caps, 16); return 0; }
int32_t kernel_set_ucred_caps(pid_t pid, const uint8_t c[16]) { (void)pid; memcpy(g_caps, c, 16); return 0; }
int sceKernelSendNotificationRequest(int a, void *r, size_t n, int b) { (void)a; (void)r; (void)n; (void)b; return 0; }

static int32_t fake_bind(uint64_t d, int32_t u) { (void)d; (void)u; binds++; return bind_result; }
void *dlopen(const char *path, int mode) { (void)path; (void)mode; return fail_library ? NULL : (void *)1; }
void *dlsym(void *h, const char *name) { (void)h; (void)name; return fail_symbol ? NULL : (void *)fake_bind; }

static void reset(int init_result, int priv_after_init)
{
    memset(g_calls, 0, sizeof g_calls);
    g_inited = 0;
    g_init_result = init_result;
    g_priv_result_after_init = priv_after_init;
}

static void test_refused_vendor_libraries(void)
{
    fail_library = 1;
    CHECK(!vpad_init() && !vpad_add(0));
    fail_library = 0; fail_symbol = 1;
    CHECK(!vpad_init() && !vpad_add(0));
    fail_symbol = 0;
}

/* T:164-211: vendor effects only, with real identification from a pipe. */
static void test_refused_vendor_acquisitions_do_not_invent_ownership(void)
{
    CHECK(pipe(log_pipe) == 0);
    ph_clock_hook = fake_clock;
    reset(0, 0);
    CHECK(vpad_init());
    add_result = -1;
    CHECK(vpad_add(0));
    vpad_poll(g_now);
    CHECK(adds == 1 && binds == 0 && deletes == 0 && frames == 0);
    add_result = 0;
    CHECK(vpad_add(1));
    vpad_poll(g_now);
    EFFECT(10, "refused-vendor-add-does-not-block-next-add", adds == 2);
    vpad_remove(0);
    g_now += 3001;
    vpad_poll(g_now);
    vpad_poll(g_now);
    CHECK(adds == 2);
    bind_result = -1;
    vpad_poll(g_now);
    g_now += 301;
    vpad_poll(g_now);
    CHECK(binds == 1 && deletes == 1 && frames == 0);
    CHECK(deleted_handle == 0x11112222);
    vpad_remove(1); vpad_remove(1);
    CHECK(deletes == 1);
    bind_result = 0;
    vpad_close_physical();
    g_pad_opened = 1; /* GetHandle supplies a borrowed handle. */
    int before = g_closed;
    uint32_t buttons;
    g_now += 6000;
    CHECK(vpad_read_physical(&buttons));
    vpad_close_physical();
    CHECK(g_closed == before && g_pad_opened == 1);
    g_pad_opened = 0; g_open_result = -1; g_now += 6000;
    CHECK(!vpad_read_physical(&buttons));
    vpad_close_physical();
    CHECK(g_closed == before);
    g_open_result = 7;
    close(log_pipe[0]); close(log_pipe[1]);
}

static void web_vpad_request(web_t *web, int stop)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(web_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    if (fd < 0) return;
    int connected = connect(fd, (struct sockaddr *)&address, sizeof address);
    CHECK(connected == 0);
    if (connected != 0) { close(fd); return; }
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    const char *request = stop
        ? "POST /api/stop HTTP/1.1\r\nHost: 127.0.0.1\r\nX-WhateverPad: 1\r\n\r\n"
        : "GET /api/state HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    CHECK(send(fd, request, strlen(request), 0) == (ssize_t)strlen(request));
    CHECK(shutdown(fd, SHUT_WR) == 0);
    web_poll(web);
    char out[2048];
    int bytes = 0;
    while (bytes < (int)sizeof out - 1) {
        int available = poll(&ready, 1, 1000);
        CHECK(available > 0);
        if (available <= 0) break;
        int n = (int)recv(fd, out + bytes, sizeof out - 1 - (size_t)bytes, 0);
        CHECK(n >= 0);
        if (n <= 0) break;
        bytes += n;
    }
    out[bytes] = 0;
    const char *body = strstr(out, "\r\n\r\n");
    const char *length = strstr(out, "\r\nContent-Length: ");
    unsigned n = 0;
    CHECK(bytes > 0 && !strncmp(out, "HTTP/1.1 200", 12) && body && length &&
          sscanf(length, "\r\nContent-Length: %u", &n) == 1 &&
          (size_t)bytes == (size_t)(body + 4 - out) + n);
    if (stop) CHECK(body && !strcmp(body + 4, "{\"ok\":true}"));
    CHECK(close(fd) == 0);
}

/* RV1 C / review r3 B6: ps5_main polls web Stop before queued/pending vpad work. */
static void test_web_stop_preempts_queued_and_pending_effects(void)
{
    ph_clock_hook = fake_clock;
    for (int mode = 0; mode < 3; mode++) {
        CHECK(pipe(log_pipe) == 0);
        adds = binds = deletes = frames = 0;
        reset(0, 0);
        CHECK(vpad_init());
        CHECK(vpad_add(0));
        CHECK(adds == 0);
        if (mode == 2) {
            vpad_poll(g_now);
            CHECK(adds == 1 && binds == 0 && deletes == 0 && frames == 0);
            CHECK(vpad_add(1));
            CHECK(adds == 1);
        }
        host_t *host = NULL;
        volatile int stop = 0, retry = 0, bt = WEB_BT_FAILED;
        web_cfg cfg = {.host = &host, .stop = &stop, .retry = &retry,
            .bt_state = &bt, .version = "vpad-test"};
        web_port = 0;
        web_t *web = web_start(&cfg, 0);
        CHECK(web != NULL && web_port > 0);
        if (web) {
            web_vpad_request(web, mode != 0);
            CHECK(stop == (mode != 0) && retry == 0);
            vpad_poll(g_now);
            if (mode != 1) {
                if (mode == 0) vpad_poll(g_now);
                g_now += 301;
                vpad_poll(g_now);
                if (mode == 2) vpad_poll(g_now);
            }
            pad_state input = {.buttons = PAD_CROSS};
            vpad_update(0, &input);
            vpad_update(1, &input);
            printf("WEB_VPAD mode=%d stop=%d adds=%d binds=%d deletes=%d frames=%d\n",
                   mode, stop, adds, binds, deletes, frames);
            if (mode == 1)
                EFFECT(10, "web-stop-before-queued-creation",
                       adds == 0 && binds == 0 && deletes == 0 && frames == 0);
            else if (mode == 2)
                EFFECT(10, "web-stop-drains-identification-without-queued-creation",
                       adds == 1 && binds == 0 && deletes == 1 && frames == 0);
            else CHECK(adds == 1 && binds == 1 && deletes == 0 && frames == 1);
            web_stop(web);
        }
        vpad_remove(0); vpad_remove(1);
        vpad_poll(g_now); g_now += 301; vpad_poll(g_now);
        CHECK(deletes == adds && deleted_handle == 0x11112222);
        int removed = deletes;
        vpad_remove(0); vpad_remove(1); vpad_poll(g_now);
        CHECK(deletes == removed);
        CHECK(vpad_creds_restore());
        CHECK(close(log_pipe[0]) == 0 && close(log_pipe[1]) == 0);
    }
}

/* Review r4 B7: a pending removal cannot choose an unidentified device. */
static void test_pending_removal_preserves_unidentified_devices(void)
{
    ph_clock_hook = fake_clock;
    for (int candidates = 0; candidates <= 2; candidates += 2) {
        CHECK(pipe(log_pipe) == 0);
        adds = binds = deletes = frames = 0;
        reset(0, 0);
        CHECK(vpad_init());
        CHECK(vpad_add(0));
        vpad_poll(g_now);
        CHECK(adds == 1 && binds == 0 && deletes == 0 && frames == 0);
        if (candidates == 0) {
            /* Withhold the successful Add's log event, not the Add itself. */
            char line[512];
            ssize_t n = read(log_pipe[0], line, sizeof line - 1);
            CHECK(n > 0);
            if (n > 0) {
                line[n] = 0;
                CHECK(strstr(line, "DeviceId:0x11112222") != NULL);
            }
        } else {
            const char second[] =
                "SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x33334444][type:1][subType:22][capabilityBattery:0]\n";
            CHECK(write(log_pipe[1], second, sizeof second - 1) == sizeof second - 1);
        }
        host_t *host = NULL;
        volatile int stop = 0, retry = 0, bt = WEB_BT_FAILED;
        web_cfg cfg = {.host = &host, .stop = &stop, .retry = &retry,
            .bt_state = &bt, .version = "vpad-preservation"};
        web_port = 0;
        web_t *web = web_start(&cfg, 0);
        CHECK(web != NULL && web_port > 0);
        if (web) {
            web_vpad_request(web, 1);
            CHECK(stop == 1 && retry == 0);
            web_stop(web);
        }
        vpad_remove(0);
        vpad_poll(g_now);
        g_now += 3001;
        vpad_poll(g_now);
        pad_state input = {.buttons = PAD_CROSS};
        vpad_update(0, &input);
        printf("PRESERVATION candidates=%d adds=%d binds=%d deletes=%d frames=%d\n",
               candidates, adds, binds, deletes, frames);
        CHECK(adds == 1 && binds == 0 && deletes == 0 && frames == 0);
        vpad_remove(0);
        vpad_poll(g_now);
        vpad_update(0, &input);
        CHECK(adds == 1 && binds == 0 && deletes == 0 && frames == 0);
        CHECK(vpad_creds_restore());
        CHECK(close(log_pipe[0]) == 0 && close(log_pipe[1]) == 0);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    strict_mode = argc == 2 && !strcmp(argv[1], "--expected-failures");
    if (argc > 2 || (argc == 2 && !strict_mode)) return 2;
    test_refused_vendor_libraries();
    printf("vpad_init: libScePad is initialised before its privilege is set\n");
    reset(0, 0);
    CHECK(vpad_init() == 1);
    CHECK(strstr(g_calls, "IP") != NULL);               /* init, then privilege */
    CHECK(strchr(g_calls, 'P') > strchr(g_calls, 'I'));

    printf("a privilege answer that is not 0 is a warning, as in the references\n");
    reset(0, SCE_PAD_ERROR_NOT_INITIALIZED);
    CHECK(vpad_init() == 1);

    printf("a failing scePadInit is fatal, and the privilege is never set\n");
    reset((int32_t)0x80920001, 0);
    CHECK(vpad_init() == 0);
    CHECK(strchr(g_calls, 'P') == NULL);

    printf("the original credentials can be put back and raised again\n");
    reset(0, 0);
    CHECK(vpad_init() == 1);
    CHECK(g_authid == 0x3800000000010003ull);               /* raised */
    CHECK(vpad_creds_restore() == 1);
    CHECK(g_authid == 0x4800000000000001ull && g_caps[0] == 0x00);   /* as found at the start */
    CHECK(vpad_creds_restore() == 1);                       /* twice is harmless */
    CHECK(vpad_creds_raise() == 1);
    CHECK(g_authid == 0x3800000000010003ull);

    printf("the user's pad: no handle, so it is opened the normal way; read; closed at the end\n");
    {
        uint32_t b = 0;
        ph_clock_hook = fake_clock;
        reset(0, 0);
        CHECK(vpad_init() == 1);
        g_pad_buttons = 0x00104400u;                        /* L1 + R1 + touchpad */
        CHECK(vpad_read_physical(&b) == 1);
        CHECK(g_pad_opened == 1 && b == 0x00104400u);
        CHECK(vpad_physical_error() == 0);
        g_pad_buttons = 0x80000000u | 0x400;                /* the system has the input */
        CHECK(vpad_read_physical(&b) == 0 && vpad_physical_error() == (int32_t)0xFFFF0003);
        g_pad_buttons = 0;
        g_pad_connected = 0;
        CHECK(vpad_read_physical(&b) == 0 && vpad_physical_error() == (int32_t)0xFFFF0002);
        g_pad_connected = 1;
        vpad_close_physical();
        CHECK(g_closed == 1 && g_pad_opened == 0);
        g_pad_opened = 0;
        g_open_result = (int32_t)0x80920004;                /* opening is refused */
        CHECK(vpad_read_physical(&b) == 0);                 /* tried again at most every 5 s: */
        CHECK(vpad_physical_error() == (int32_t)0x80920008);
        g_now += 6000;
        CHECK(vpad_read_physical(&b) == 0);
        CHECK(vpad_physical_error() == (int32_t)0x80920004);
        g_open_result = 7;
    }

    printf("the user given the pads is one who is signed in\n");
    {
        uint32_t b = 0;
        ph_clock_hook = fake_clock;

        /* the initial user is signed in: preferred over the foreground one */
        g_init_user = 0x2000; g_fore_user = 0x3000;
        g_login[0] = 0x3000; g_login[1] = 0x2000; g_login[2] = g_login[3] = -1;
        g_pad_opened = 0; g_closed = 0; g_now += 10000;
        reset(0, 0);
        CHECK(vpad_init() == 1);
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x2000);

        /* the initial user is not signed in; the foreground one is */
        vpad_close_physical();
        g_pad_opened = 0;
        g_login[0] = 0x3000; g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);                   /* picks again */
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x3000);

        /* neither: the first one that is signed in */
        vpad_close_physical();
        g_pad_opened = 0;
        g_init_user = 0x2000; g_fore_user = 0x2500;
        g_login[0] = 0x4000; g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);
        vpad_close_physical();
        g_pad_opened = 0;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x4000);

        /* nobody signed in: no pad can be read, and no user is made up */
        vpad_close_physical();
        g_pad_opened = 0;
        g_login[0] = g_login[1] = g_login[2] = g_login[3] = -1;
        reset(0, 0);
        g_now += 10000;
        CHECK(vpad_init() == 1);
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 0 && g_pad_opened == 0);

        /* USER_NOT_LOGIN on open: the user is looked for again, at the next try */
        g_init_user = 0x2000;
        g_login[0] = 0x2000;
        g_now += 10000;
        CHECK(vpad_read_physical(&b) == 1 && g_open_user == 0x2000);
        vpad_close_physical();
        g_pad_opened = 0;
        g_init_user = 0x10000000; g_fore_user = 0x10000000; g_login[0] = 0x10000000;
    }

    printf("vpad_choose_user: default order, and the bind_user override\n");
    {
        int32_t two[4] = { 0x1111, 0x2222, -1, -1 };
        int32_t one[4] = { 0x1111, -1, -1, -1 };
        int32_t none[4] = { -1, -1, -1, -1 };

        CHECK(vpad_choose_user(two, 0x1111, 0x2222, NULL) == 0x1111);       /* the owner */
        CHECK(vpad_choose_user(two, 0x1111, 0x2222, "") == 0x1111);
        CHECK(vpad_choose_user(two, 0x1111, 0x2222, "other") == 0x2222);    /* someone else */
        CHECK(vpad_choose_user(one, 0x1111, 0x1111, "other") == 0x1111);    /* nobody else: default */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "2222") == 0x2222);     /* an id */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "9999") == 0x1111);     /* not signed in: default */
        CHECK(vpad_choose_user(two, 0x1111, 0x1111, "zz") == 0x1111);       /* not an id: default */
        CHECK(vpad_choose_user(none, 0x1111, 0x1111, "other") == -1);       /* nobody signed in */
    }

    test_refused_vendor_acquisitions_do_not_invent_ownership();
    test_web_stop_preempts_queued_and_pending_effects();
    test_pending_removal_preserves_unidentified_devices();
    ph_clock_hook = NULL;
    printf(g_fail ? "%d check(s) failed\n" : "all vpad checks passed\n", g_fail);
    printf("%d expected-failing vpad cases\n", pending);
    return g_fail != 0 || (strict_mode && pending != 0);
}
