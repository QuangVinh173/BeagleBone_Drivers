#ifndef USB_HID_H
#define USB_HID_H

#include <linux/ioctl.h>
#include <linux/types.h>
 
struct hid_status {
    bool connected;
    bool full_speed;
};

struct hid_info {
    __u16 bcdUSB;
    __u16 idVendor;
    __u16 idProduct;
    __u16 bcdDevice;
    __u8 iManufacturer;
    __u8 iProduct;
    __u8 iSerialNumber;
};
 
#define BBB_HID_IOC_MAGIC 'U'
 
#define BBB_HID_CONNECTION \
    _IOR(BBB_HID_IOC_MAGIC, 1, struct hid_status)

#define BBB_HID_INFO \
    _IOR(BBB_HID_IOC_MAGIC, 2, struct hid_info)

#define BBB_HID_CAPTURE \
    _IOR(BBB_HID_IOC_MAGIC, 3, bool)

#define BBB_HID_NOT_CAPTURE \
    _IOR(BBB_HID_IOC_MAGIC, 4, bool)

#endif