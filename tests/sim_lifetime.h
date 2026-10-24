/* Modeled resources; test_hci_usb checks the real transport's operation calls. */
#ifndef PH_SIM_LIFETIME_H
#define PH_SIM_LIFETIME_H

#include "../src/attrib.h"
#include "../src/util.h"

#include <string.h>

typedef struct {
    int opens, event_stops, acl_stops, uninits, closes;
    int event_reads, acl_reads;
} sim_lifetime;

/* Oracle-only provenance. The hci_ops interface still returns bytes alone. */
typedef struct {
    unsigned invocation;
    int foreign;
    int copy;
} sim_origin;

enum sim_observation { SIM_COMMAND, SIM_GENERATED, SIM_DELIVERED };
typedef void (*sim_observer)(void *, enum sim_observation, sim_origin,
                             unsigned, const unsigned char *, int);

/* A genuinely retired Classic link can reconnect on a fresh wire handle. */
struct sim;
void sim_set_handle(struct sim *, unsigned);

/* Match hci_usb.c:event_done after delivery faults, without oracle provenance. */
static inline void sim_ownership_event(own_ledger *ledger, const unsigned char *ev,
                                       int len, long now)
{
    if (!ledger || len <= 0) return;
    if (ev[0] != OWN_CC && ev[0] != OWN_CS) attrib_observe(ev, len, now);
    if (ev[0] == 3 && len == 13 && ev[2] == 0 && ev[11] == 1)
        own_observe_connection(ledger, 0, 0, ev + 5, (uint16_t)le16(ev + 3),
                               ev[2], (uint64_t)now);
    if (ev[0] == 0x3e && len >= 3 &&
        ((ev[2] == 1 && len == 21) || (ev[2] == 0x0a && len == 33)) && ev[3] == 0)
        own_observe_connection(ledger, 1, ev[7], ev + 8, (uint16_t)le16(ev + 4),
                               ev[3], (uint64_t)now);
    if (ev[0] == 5) {
        if (len != 6 || le16(ev + 3) >= 0x0f00)
            own_detector_trip(ledger, (uint64_t)now);
        else if (!ev[2])
            own_observe_disconnect(ledger, (uint16_t)le16(ev + 3), (uint64_t)now);
    }
    if (ev[0] == OWN_CC || ev[0] == OWN_CS) {
        own_ack a = {.event = (own_ack_event)ev[0]};
        own_receipt receipt;
        if (ev[0] == OWN_CC && len == 5 && ev[1] == 3 && !le16(ev + 3)) {
            a.opcode = 0;
        } else if (len < 6 || len != 2 + ev[1] ||
                   (ev[0] == OWN_CS && ev[1] != 4)) {
            a.malformed = 1;
        } else {
            a.opcode = le16(ev + (ev[0] == OWN_CC ? 3 : 4));
            a.status = ev[ev[0] == OWN_CC ? 5 : 2];
            if (a.opcode == 0x2018 && ev[0] == OWN_CC) {
                a.random_len = (uint8_t)(len - 6);
                if (a.random_len == 8) memcpy(a.random, ev + 6, 8);
            }
        }
        own_provenance provenance = own_backend_receipt(ledger, &a, (uint64_t)now, &receipt);
        attrib_observe_receipt(ev, len, now, provenance);
        own_receive(ledger, &receipt, (uint64_t)now);
    }
}

#endif
