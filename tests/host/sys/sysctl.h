/* Linux host tests read the real boot time used by the BSD lock source. */
#ifndef TEST_SYS_SYSCTL_H
#define TEST_SYS_SYSCTL_H

#include <stdio.h>
#include <sys/time.h>

#define CTL_KERN 1
#define KERN_BOOTTIME 21

static inline int sysctl(const int *name, unsigned int namelen, void *oldp,
                         size_t *oldlenp, const void *newp, size_t newlen)
{
    FILE *f;
    char line[256];
    long boot;
    struct timeval *tv = (struct timeval *)oldp;

    if (namelen != 2 || name[0] != CTL_KERN || name[1] != KERN_BOOTTIME ||
        !tv || !oldlenp || *oldlenp < sizeof *tv || newp || newlen) return -1;
    f = fopen("/proc/stat", "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "btime %ld", &boot) == 1) {
            tv->tv_sec = boot;
            tv->tv_usec = 0;
            *oldlenp = sizeof *tv;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

#endif
