#ifndef USB_HID_H
#define USB_HID_H

#include <string>

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

class HIDUSB {
public:
    HIDUSB();
    ~HIDUSB();

    HIDUSB(const HIDUSB&) = delete;
    HIDUSB& operator=(const HIDUSB&) = delete;

    bool openDevice();
    void closeDevice();
    bool isOpen() const;

    void checkConnection() const;
    void checkInfo() const;

    bool writeData(const std::string& data);
    bool readData(std::string& response, int timeoutMs = 3000);

    void writeOption();
    void readOption();
    void writeReadOption();

    void CaptureOption();
    void NoCaptureOption();

    // Wait until the mouse sends its neutral report.
    void readMouseNeutralOption();

private:
    int fd_;
    std::string pendingRx_;
    std::string pendingMouseRx_;
};

#endif

