#ifndef WP_PROBE_H
#define WP_PROBE_H

#include "attrib.h"

#if WHATEVERPAD_PROBE
#ifndef WHATEVERPAD_PROBE_ORIGINAL
#define WHATEVERPAD_PROBE_ORIGINAL 0
#endif
#define PROBE_TAIL_MS 5000
#if WHATEVERPAD_PROBE_ORIGINAL
#define PROBE_CREDS "original"
#else
#define PROBE_CREDS "raised"
#endif
#define PROBE_ID "T8-v2-" PROBE_CREDS
/* Shared observation boundary, not an attribution/ownership receipt. */
extern long probe_deadline_ms;
#endif

#endif
