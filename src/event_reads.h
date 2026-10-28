#ifndef PH_EVENT_READS_H
#define PH_EVENT_READS_H

#include "host.h"

/* Desired event reads from the supplied host state and monotonic time. */
int event_reads_wanted(const host_t *h, long now, int idle_reads);

#endif
