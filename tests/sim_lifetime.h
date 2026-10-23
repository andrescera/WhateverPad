/* Modeled resources; test_hci_usb checks the real transport's operation calls. */
#ifndef PH_SIM_LIFETIME_H
#define PH_SIM_LIFETIME_H

typedef struct {
    int opens, event_stops, acl_stops, uninits, closes;
    int event_reads, acl_reads;
} sim_lifetime;

/* Oracle-only provenance. The hci_ops interface still returns bytes alone. */
typedef struct {
    unsigned invocation;
    int foreign;
} sim_origin;

enum sim_observation { SIM_COMMAND, SIM_GENERATED, SIM_DELIVERED };
typedef void (*sim_observer)(void *, enum sim_observation, sim_origin,
                             unsigned, const unsigned char *, int);

#endif
