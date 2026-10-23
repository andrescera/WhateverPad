#ifndef TEST_USB_SYSCALLS_H
#define TEST_USB_SYSCALLS_H

/* Read libc's inlines and assembly aliases before redirecting USB calls. */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "receipt.h"

int test_usb_open(const char *path, int flags, ...);
int test_usb_close(int fd);
int test_usb_ioctl(int fd, unsigned long request, ...);
int test_usb_usleep(useconds_t us);

#define open test_usb_open
#define close test_usb_close
#define ioctl test_usb_ioctl
#define usleep test_usb_usleep
#define own_backend_receipt test_usb_receipt

#endif
