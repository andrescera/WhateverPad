/* Modeled resources; test_hci_usb checks the real transport's operation calls. */
#ifndef PH_SIM_LIFETIME_H
#define PH_SIM_LIFETIME_H

typedef struct {
    int opens, event_stops, acl_stops, uninits, closes;
    int event_reads, acl_reads;
} sim_lifetime;

#endif
