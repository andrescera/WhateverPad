/* A narrow ioctl stand-in, selected ONLY by the test target's include path. */
#ifndef TEST_USB_IOCTL_H
#define TEST_USB_IOCTL_H
#include "usb.h"

enum {
    USB_FS_OPEN = 0x7000, USB_FS_START, USB_FS_STOP, USB_FS_COMPLETE,
    USB_FS_INIT, USB_FS_UNINIT, USB_DO_REQUEST, USB_GET_DEVICE_DESC,
    USB_GET_FULL_DESC, USB_GET_DEVICEINFO, USB_GET_IFACE_DRIVER
};
#define USB_FS_FLAG_SINGLE_SHORT_OK 1
#define USB_FS_FLAG_FORCE_SHORT 2
struct usb_fs_endpoint {
    void **ppBuffer;
    uint32_t *pLength;
    uint32_t nFrames, aFrames, flags, status;
    uint16_t timeout;
};
struct usb_fs_open { uint8_t ep_index, ep_no; uint32_t max_bufsize, max_frames; };
struct usb_fs_start { uint8_t ep_index; };
struct usb_fs_stop { uint8_t ep_index; };
struct usb_fs_complete { uint8_t ep_index; };
struct usb_fs_init { struct usb_fs_endpoint *pEndpoints; uint8_t ep_index_max; };
struct usb_fs_uninit { uint8_t unused; };
struct usb_ctl_request { void *ucr_data; struct usb_device_request ucr_request; };
struct usb_gen_descriptor {
    void *ugd_data;
    uint32_t ugd_maxlen, ugd_actlen;
    uint8_t ugd_config_index, ugd_iface_index;
};
struct usb_device_info {
    char udi_product[64], udi_vendor[64];
    unsigned udi_bus, udi_addr, udi_config_no, udi_speed, udi_power, udi_suspended;
};
#endif
