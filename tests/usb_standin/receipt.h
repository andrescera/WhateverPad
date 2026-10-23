#ifndef TEST_USB_RECEIPT_H
#define TEST_USB_RECEIPT_H

#include "../../src/ownership.h"

struct test_usb_receipt_state {
    own_provenance last;
    unsigned calls;
};
extern struct test_usb_receipt_state test_usb_receipts;

own_provenance test_usb_receipt(own_ledger *, const own_ack *, uint64_t, own_receipt *);

#endif
