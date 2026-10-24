/* Test the real runtime's slot boundary with virtual-device call stand-ins.
 * The payload entry point is discarded by section GC and is never executed. */
#if !defined(__FreeBSD__)
#define SYS_thr_set_name 0
#endif
#define main unused_payload_entry
#include "../src/ps5_main.c"
#undef main

static unsigned adds, inputs, removes, failures, checks;
static uint32_t last_buttons;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL runtime callbacks:%d: %s\n", __LINE__, #c); failures++; \
} } while (0)

int vpad_add(int slot) { CHECK(slot == 0); adds++; return 1; }
void vpad_update(int slot, const pad_state *state)
{
    CHECK(slot == 0);
    inputs++;
    last_buttons = state->buttons;
}
void vpad_remove(int slot) { CHECK(slot == 0); removes++; }
void notify(const char *format, ...) { (void)format; }
long now_ms(void) { return 0; }
uint32_t pscombo_get(uint16_t vid, uint16_t pid) { (void)vid; (void)pid; return 0; }
uint32_t pscombo_apply(uint32_t combo, uint32_t buttons) { (void)combo; return buttons; }

int main(void)
{
    link_ref first = {1, 1, 0}, second = {1, 2, 0}, old_attempt = {2, 2, 0};
    pad_info info = {.vid = 1, .pid = 2, .profile = "fixture"};
    pad_state stale = {.buttons = PAD_SQUARE}, current = {.buttons = PAD_CROSS};
    on_connect(NULL, first, &info);
    on_disconnect(NULL, first);
    on_connect(NULL, second, &info);
    on_state(NULL, first, &stale);
    on_disconnect(NULL, first);
    CHECK(adds == 2 && inputs == 0 && removes == 1);
    on_state(NULL, old_attempt, &stale);
    on_disconnect(NULL, old_attempt);
    CHECK(inputs == 0 && removes == 1);
    on_state(NULL, second, &current);
    CHECK(inputs == 1 && last_buttons == PAD_CROSS);
    on_disconnect(NULL, second);
    on_disconnect(NULL, second);
    CHECK(removes == 2);
    printf("runtime link callbacks: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
