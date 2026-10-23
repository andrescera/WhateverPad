/* HCI over USB to the PS5's Bluetooth chip, alongside the system's driver.
 *
 * What is known about the chip: one USB device, /dev/ugen0.2 (MediaTek
 * 0e8d:3603), with two complete HCI controllers. The system runs the
 * DualSense on the second. The first is opened here without detaching the
 * system's driver, which would take the DualSense down. Its endpoints are
 * read from the chip's configuration descriptor (usb_desc.c): on a PS5 fat
 * with firmware 10.01 the events come on 0x81, ACL in on 0x82, ACL out on
 * 0x01, and commands go on the control endpoint to interface 0. Other consoles may
 * lay the chip out differently.
 *
 * The system's driver keeps reads pending on those endpoints too, so each
 * incoming packet goes to whichever read is first in line. Keeping many
 * reads pending wins most of them; the stack above must cope with the few
 * it loses. Token-backed commands never retransmit after a missing answer.
 */
#ifndef PH_HCI_USB_H
#define PH_HCI_USB_H

#include "hci.h"
#include "ownership.h"

/* The chip's USB vendor id once opened (0x0E8D MediaTek, 0x1286 Marvell/NXP), 0 before. */
int hci_usb_chip_vendor(void);
/* "MediaTek (0e8d:3603)", "Marvell/NXP (1286:2059)", or "" before it is opened. */
const char *hci_usb_chip_name(void);
/* Opens the controller. Returns 0 on failure, with the reason logged. */
int hci_usb_open(hci_t *out);

/* Bind once, before protocol work. The ledger outlives close; service may latch
 * release, but must not dispose/reenter this transport. Raw output then fails.
 * Bound consumers leave attrib observation to this backend, once per arrival. */
int hci_usb_bind_ownership(hci_t *, own_ledger *, void (*service)(void *), void *);
own_submit_result hci_usb_submit(hci_t *, const own_token *);

#endif
