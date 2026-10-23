/* Faults at real integration boundaries, separate from the oracle under test. */
#include "sim.h"
#include "usb_standin/dev/usb/usb_ioctl.h"
#include "../src/ps5_vpad.h"
#include "../src/web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned injections;
static volatile int *retry_request;

static int fault(const char *name)
{
    const char *value = getenv("INTEGRATION_ORACLE_FAULT");
    return value && !strcmp(value, name);
}

int __real_test_usb_ioctl(int fd, unsigned long request, ...);
int __wrap_test_usb_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int result = __real_test_usb_ioctl(fd, request, arg);
    if (!result && request == USB_FS_STOP && fault("duplicate-stop")) {
        injections++;
        result = __real_test_usb_ioctl(fd, request, arg);
    }
    return result;
}

web_t *__real_web_start(const web_cfg *cfg, int port);
void __real_web_poll(web_t *web);
web_t *__wrap_web_start(const web_cfg *cfg, int port)
{
    retry_request = cfg->retry;
    return __real_web_start(cfg, port);
}
void __wrap_web_poll(web_t *web)
{
    __real_web_poll(web);
    if (fault("clear-retry") && *retry_request) {
        injections++;
        *retry_request = 0;
    }
}

void __real_vpad_update(int slot, const pad_state *state);
void __wrap_vpad_update(int slot, const pad_state *state)
{
    pad_state input = *state;
    if (fault("missing-cross")) { input.buttons &= ~PAD_CROSS; injections++; }
    __real_vpad_update(slot, &input);
}

int __real_sim_foreign_command(sim_t *sim, unsigned opcode, const void *params, int length);
int __wrap_sim_foreign_command(sim_t *sim, unsigned opcode, const void *params, int length)
{
    if (fault("omit-reassignment")) { injections++; return 1; }
    return __real_sim_foreign_command(sim, opcode, params, length);
}

__attribute__((destructor)) static void report_fault(void)
{
    const char *value = getenv("INTEGRATION_ORACLE_FAULT");
    fflush(stdout);
    if (value) fprintf(stderr, "ORACLE_FAULT name=%s injections=%u\n", value, injections);
}
