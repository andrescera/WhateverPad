/* Only the fields hci_usb.c uses. Test protocol, NOT a FreeBSD ABI replica. */
#ifndef TEST_USB_H
#define TEST_USB_H
#include <stdint.h>

#define UGETW(w) ((unsigned)(w)[0] | (unsigned)(w)[1] << 8)
#define USETW(w, v) do { (w)[0] = (uint8_t)(v); (w)[1] = (uint8_t)((v) >> 8); } while (0)
#define UT_WRITE_CLASS_DEVICE 0x20

struct usb_device_descriptor {
    uint8_t idVendor[2], idProduct[2], bcdUSB[2];
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize;
};
struct usb_device_request {
    uint8_t bmRequestType, bRequest, wValue[2], wIndex[2], wLength[2];
};
#endif
