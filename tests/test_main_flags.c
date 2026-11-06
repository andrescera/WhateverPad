/* Stop must reach the watchdog while opening pumps the idle callback. */
#define STATE_DIR "."
#define main payload_main
#include "../src/ps5_main.c"
#undef main
#include <assert.h>

static long tick = 100000;
static long test_time(void) { return tick; }

int main(void)
{
    char dir[] = "/tmp/whateverpad-stop-XXXXXX";
    FILE *f;
    unsigned remaining;
    int i, flag_exists;

    assert(mkdtemp(dir));
    assert(chdir(dir) == 0);
    ph_clock_hook = test_time;
    g_bt_state = WEB_BT_TRYING;
    check_flags(tick);
    f = fopen(STOP_FLAG, "w"); assert(f); assert(fclose(f) == 0);
    f = fopen(PAIR_FLAG, "w"); assert(f); assert(fclose(f) == 0);
    on_idle(NULL);
    assert(g_stop == 0 && g_pair_request == 0); /* same second: no second flag read */
    for (i = 0; i < 1000; i++) {
        tick += 20;
        on_idle(NULL);
    }
    remaining = alarm(0);
    flag_exists = access(STOP_FLAG, F_OK) == 0;
    printf("Opening callbacks after 20000 ms: stop=%d flag_exists=%d alarm_remaining=%u pair_requested=%d\n",
           (int)g_stop, flag_exists, remaining, g_pair_request);
    unlink(STOP_FLAG); unlink(PAIR_FLAG);
    ph_clock_hook = NULL;
    assert(chdir("/") == 0); assert(rmdir(dir) == 0);
    fflush(stdout);
    assert(g_stop == 1);
    assert(!flag_exists);
    assert(remaining > 0);
    assert(g_pair_request == 1 && g_host == NULL);
    puts("opening Stop flag: all checks passed");
    return 0;
}
