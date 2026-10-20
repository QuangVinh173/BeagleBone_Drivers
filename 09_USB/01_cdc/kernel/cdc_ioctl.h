#ifndef USB_CDC_H
#define USB_CDC_H

#include <linux/ioctl.h>
#include <linux/types.h>
 
struct cdc_status {
    bool connected;
    bool full_speed;
};

struct cdc_info {
    __u16 bcdUSB;
    __u16 idVendor;
    __u16 idProduct;
    __u16 bcdDevice;
    __u8 iManufacturer;
    __u8 iProduct;
    __u8 iSerialNumber;
};
 
#define BBB_CDC_IOC_MAGIC 'U'
 
#define BBB_CDC_CONNECTION \
    _IOW(BBB_CDC_IOC_MAGIC, 1, struct cdc_info)

#define BBB_CDC_INFO \
    _IOW(BBB_CDC_IOC_MAGIC, 2, struct cdc_info)

#endif