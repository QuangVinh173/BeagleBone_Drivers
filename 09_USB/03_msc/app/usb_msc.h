#ifndef USB_MSC_H
#define USB_MSC_H

#include <string>
#include <linux/ioctl.h>
#include <linux/types.h>

#define SIZE_FILE_FOLDER 200
#define SIZE_CONTENT 200

/* ============== ENUM ================ */
enum usb_speed {
	USB_SPEED_UNKNOWN = 0,	
	USB_SPEED_HIGH,		            /* usb 2.0 */
	USB_SPEED_FULL, USB_SPEED_LOW,	/* usb 1.1 */
};

enum commands {
    USB_MOUNT = 0,
    USB_READ_ALL,
    USB_READ_FILE,
    USB_READ_DIR,

    USB_ADD_CONTENT,
    USB_DELETE_CONTENT,

    USB_MAKE_FILE,
    USB_REMV_FILE,

    USB_MAKE_DIR,
    USB_REMV_DIR,
};

/* ============== STRUCT ================ */
struct msc_status {
    bool connected;
    enum usb_speed speed_state;
};

struct msc_info {
    __u16 bcdUSB;
    __u16 idVendor;
    __u16 idProduct;
    __u16 bcdDevice;
    __u8 iManufacturer;
    __u8 iProduct;
    __u8 iSerialNumber;
};

struct msc_command {
    char string[SIZE_FILE_FOLDER];
    int len;

    char content[SIZE_CONTENT];
    int len_cnt;
};
 
/* ============== COMMAND ================ */
#define BBB_MSC_IOC_MAGIC 'U'
 
#define BBB_MSC_CONNECTION \
    _IOW(BBB_MSC_IOC_MAGIC, 1, struct msc_status)

#define BBB_MSC_INFO \
    _IOW(BBB_MSC_IOC_MAGIC, 2, struct msc_info)

#define BBB_MSC_MOUNT \
    _IOW(BBB_MSC_IOC_MAGIC, USB_MOUNT, struct msc_command)

#define BBB_MSC_READ_ALL \
    _IOW(BBB_MSC_IOC_MAGIC, USB_READ_ALL, struct msc_command)

#define BBB_MSC_READ_FILE \
    _IOW(BBB_MSC_IOC_MAGIC, USB_READ_FILE, struct msc_command)

#define BBB_MSC_READ_DIR \
    _IOW(BBB_MSC_IOC_MAGIC, USB_READ_DIR, struct msc_command)

#define BBB_MSC_ADD_CONTENT \
    _IOW(BBB_MSC_IOC_MAGIC, USB_ADD_CONTENT, struct msc_command)

#define BBB_MSC_DELETE_CONTENT \
    _IOW(BBB_MSC_IOC_MAGIC, USB_DELETE_CONTENT, struct msc_command)

#define BBB_MSC_MAKE_FILE \
    _IOW(BBB_MSC_IOC_MAGIC, USB_MAKE_FILE, struct msc_command)

#define BBB_MSC_REMV_FILE \
    _IOW(BBB_MSC_IOC_MAGIC, USB_REMV_FILE, struct msc_command)

#define BBB_MSC_MAKE_DIR \
    _IOW(BBB_MSC_IOC_MAGIC, USB_MAKE_DIR, struct msc_command)

#define BBB_MSC_REMV_DIR \
    _IOW(BBB_MSC_IOC_MAGIC, USB_REMV_DIR, struct msc_command)

class MSCUSB {
public:
    MSCUSB();
    ~MSCUSB();

    MSCUSB(const MSCUSB&) = delete;
    MSCUSB& operator=(const MSCUSB&) = delete;

    bool openDevice();
    void closeDevice();
    bool isOpen() const;

    void checkConnection() const;
    void checkInfo() const;

    int f_mount();
    int f_read_all();
    int f_read_file(std::string path_file);
    int f_read_dir(std::string path_dir);

    int f_add_content(std::string path_dir, std::string content);
    int f_delete_content(std::string path_dir);

    int f_make_file(std::string path_file);
    int f_remv_file(std::string path_file);

    int f_make_dir(std::string path_dir);
    int f_remv_dir(std::string path_dir);

    void handleFilesFolders();

private:
    int fd_;
};

#endif

