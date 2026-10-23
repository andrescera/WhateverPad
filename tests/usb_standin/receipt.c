#include "receipt.h"

struct test_usb_receipt_state test_usb_receipts;

/* Observe the real producer; never replace its receipt or provenance. */
own_provenance test_usb_receipt(own_ledger *l, const own_ack *ack, uint64_t now,
                                own_receipt *receipt)
{
    test_usb_receipts.calls++;
    test_usb_receipts.last = own_backend_receipt(l, ack, now, receipt);
    return test_usb_receipts.last;
}
