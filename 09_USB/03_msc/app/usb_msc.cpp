#include "usb_msc.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {
constexpr const char* DEVICE_PATH = "/dev/usb1";
}

MSCUSB::MSCUSB() : fd_(-1) {}
MSCUSB::~MSCUSB() { closeDevice(); }

std::string usbVersionToString(std::uint16_t bcdUSB)
{
    std::ostringstream oss;
    oss << ((bcdUSB >> 8) & 0xFF) << "."
        << std::hex << std::uppercase
        << ((bcdUSB >> 4) & 0x0F)
        << (bcdUSB & 0x0F);
    return oss.str();
}

bool MSCUSB::openDevice() {
    if (isOpen()) return true;
    fd_ = ::open(DEVICE_PATH, O_RDWR);
    if (fd_ < 0) {
        std::cerr << "open(" << DEVICE_PATH << ") failed: "
                  << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

void MSCUSB::closeDevice() {
    if (isOpen()) ::close(fd_);
    fd_ = -1;
}

bool MSCUSB::isOpen() const { return fd_ >= 0; }

void MSCUSB::checkConnection() const {
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    msc_status status{};
    if (ioctl(fd_, BBB_MSC_CONNECTION, &status) < 0) {
        std::cerr << "BBB_MSC_CONNECTION failed: "
                  << std::strerror(errno) << '\n';
        return;
    }

    std::cout << "\n========================================\n"
              << "          USB DEVICE STATUS\n"
              << "========================================\n";

    if (!status.connected) {
        std::cout << "No device connected\n";
    } else if (status.speed_state == USB_SPEED_HIGH) {
        std::cout << "High-speed device connected\n";
    } else if (status.speed_state == USB_SPEED_FULL) {
        std::cout << "Full-speed device connected\n";
    } else {
        std::cout << "Low-speed device connected\n";
    }
}

void MSCUSB::checkInfo() const {
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    msc_info info{};
    if (ioctl(fd_, BBB_MSC_INFO, &info) < 0) {
        std::cerr << "Failed to get USB information: "
                  << std::strerror(errno) << '\n';
        return;
    }

    if (info.bcdUSB == 0) {
        std::cerr << "Invalid USB information. Check the connection.\n";
        return;
    }

    std::cout << "\n========================================\n"
              << "          USB DEVICE INFORMATION\n"
              << "========================================\n"
              << "USB Specification: USB " << usbVersionToString(info.bcdUSB) << '\n'
              << "Vendor ID (VID)  : 0x"
              << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
              << info.idVendor << '\n'
              << "Product ID (PID) : 0x" << std::setw(4) << info.idProduct << '\n'
              << "Device Release   : 0x" << std::setw(4) << info.bcdDevice << '\n'
              << std::dec << std::setfill(' ')
              << "\nDescriptor Indices:\n"
              << "  Manufacturer   : " << static_cast<unsigned>(info.iManufacturer) << '\n'
              << "  Product        : " << static_cast<unsigned>(info.iProduct) << '\n'
              << "  Serial Number  : " << static_cast<unsigned>(info.iSerialNumber) << '\n'
              << "========================================\n";
}

int MSCUSB::f_mount(){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    if (ioctl(fd_, BBB_MSC_MOUNT, &cmd) < 0) {
        std::cerr << "Failed to mount usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_read_all()
{
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return -1;
    }

    struct msc_command cmd{};

    if (ioctl(fd_, BBB_MSC_READ_ALL, &cmd) < 0) {
        std::cerr << "Failed to read USB MSC output: "
                    << strerror(errno) << '\n';
        return -1;
    }


    return 0;
}

int MSCUSB::f_read_file(std::string path_file){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_file.size();
    memcpy(cmd.string, path_file.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_READ_FILE, &cmd) < 0) {
        std::cerr << "Failed to read file usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_read_dir(std::string path_dir){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_dir.size();
    memcpy(cmd.string, path_dir.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_READ_DIR, &cmd) < 0) {
        std::cerr << "Failed to read dir usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_add_content(std::string path_dir, std::string content){
    if (!isOpen()) {
        std::cerr << "Device f_add_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_dir.size();
    memcpy(cmd.string, path_dir.c_str(), cmd.len);

    cmd.len_cnt = content.size();
    memcpy(cmd.content, content.c_str(), cmd.len_cnt);

    if (ioctl(fd_, BBB_MSC_ADD_CONTENT, &cmd) < 0) {
        std::cerr << "Failed to add content usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_delete_content(std::string path_dir){
    if (!isOpen()) {
        std::cerr << "Device f_delete_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_dir.size();
    memcpy(cmd.string, path_dir.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_DELETE_CONTENT, &cmd) < 0) {
        std::cerr << "Failed to delete content usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_make_file(std::string path_file){
    if (!isOpen()) {
        std::cerr << "Device f_delete_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_file.size();
    memcpy(cmd.string, path_file.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_MAKE_FILE, &cmd) < 0) {
        std::cerr << "Failed to make file usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_remv_file(std::string path_file){
    if (!isOpen()) {
        std::cerr << "Device f_delete_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_file.size();
    memcpy(cmd.string, path_file.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_REMV_FILE, &cmd) < 0) {
        std::cerr << "Failed to remv file usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_make_dir(std::string path_dir){
    if (!isOpen()) {
        std::cerr << "Device f_delete_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_dir.size();
    memcpy(cmd.string, path_dir.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_MAKE_DIR, &cmd) < 0) {
        std::cerr << "Failed to make dir usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

int MSCUSB::f_remv_dir(std::string path_dir){
    if (!isOpen()) {
        std::cerr << "Device f_delete_content is not open.\n";
        return -1;
    } 

    struct msc_command cmd{};
    cmd.len = path_dir.size();
    memcpy(cmd.string, path_dir.c_str(), cmd.len);

    if (ioctl(fd_, BBB_MSC_REMV_DIR, &cmd) < 0) {
        std::cerr << "Failed to remv dir usb msc: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        return -1;
    }    

    return 0;
}

void MSCUSB::handleFilesFolders() {
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }
    int ret = 0;

    /* mount */
    ret = f_mount();

    /* Create folder, file, content and read content */
    if (ret == 0){
        ret = f_make_dir("./Languages");
    } 
    if (ret == 0){
        ret = f_make_file("./Languages/data.txt");
    }   
    if (ret == 0){
        ret = f_add_content("./Languages/data.txt", "Lanaguages are funny hahaha!!");
    }
    if (ret == 0){
        ret = f_read_file("./Languages/data.txt");
    }

    if (ret == 0){
        ret = f_make_dir("./Countries");
    } 
    if (ret == 0){
        ret = f_make_file("./Countries/VietNam.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./Countries/VietNam.txt", "VietNam is a great country!!");
    } 
    if (ret == 0){
        ret = f_make_file("./Countries/China.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./Countries/China.txt", "China is next to VietNam!!");
    } 

    /* Create hidden folder, files and content */
    if (ret == 0){
        ret = f_make_dir("./.Hidden_item");
    } 
    if (ret == 0){
        ret = f_make_file("./.Hidden_item/tea.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./.Hidden_item/tea.txt", "Tea is made in China!!");
    } 
    if (ret == 0){
        ret = f_make_file("./.Hidden_item/coffee.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./.Hidden_item/coffee.txt", "coffee is made in Western!!");
    } 

    /* read all files and folders */
    if (ret == 0){
        ret = f_read_all();
    }



    /* Delete folder, file, content and read content */
    if (ret == 0){
        ret = f_delete_content("./Languages/data.txt");
    }
    if (ret == 0){
        ret = f_remv_file("./Languages/data.txt");
    } 
    if (ret == 0){
        ret = f_remv_dir("./Languages");
    }   


    /* Create new folder, file, content and read content */
    if (ret == 0){
        ret = f_make_dir("./Languages_new");
    } 
    if (ret == 0){
        ret = f_make_file("./Languages_new/data_new.txt");
    }   
    if (ret == 0){
        ret = f_add_content("./Languages_new/data_new.txt", "Lanaguages NEW new are funny hahaha!!");
    }
    if (ret == 0){
        ret = f_read_file("./Languages_new/data_new.txt");
    }

    /* Modify content of existing files */
    if (ret == 0){
        ret = f_delete_content("./Countries/China.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./Countries/China.txt", "CHINA is a northen country of VietNam!");
    } 
    if (ret == 0){
        ret = f_delete_content("./.Hidden_item/coffee.txt");
    } 
    if (ret == 0){
        ret = f_add_content("./.Hidden_item/coffee.txt", "Sorry, coffee is from everywhere..");
    } 


    /* read all files and folders */
    if (ret == 0){
        ret = f_read_all();
    }

    std::cout << "See output on kernel log \n";
}
