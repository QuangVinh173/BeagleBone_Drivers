#include "i2c.h"
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>
 
/* Constructor */
I2c::I2c(const std::string& dev_path) 
    : device_path(dev_path) {}
 
/* destructor */
I2c::~I2c() {
    deinit(); // close fd
}
 
bool I2c::init(struct bbb_i2c_config *config) {
    if (isOpen()) {
        return true;
    }
 
    fd = open(device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": " << strerror(errno) << std::endl;
        return false;
    }
 
    if (ioctl(fd, BBB_I2C_SET_CONFIG, config) < 0) {
        std::cerr << "Failed to configure I2C: "
                  << strerror(errno) << std::endl;
 
        close(fd);
        fd = -1;
        return false;
    }
 
    std::cout << "I2C opened and configured: " << device_path << std::endl;
    return true;
}
 
void I2c::deinit() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
        std::cout << "I2C closed: " << device_path << std::endl;
    }
}
 
ssize_t I2c::write(uint8_t *buffer, uint8_t slave_addr, uint8_t reg_addr, int len) {
    if (!isOpen()) return -1;
    ssize_t count = 0;
 
    if (ioctl(fd, BBB_I2C_SET_SLAVE_ADDR, &slave_addr) < 0) {
        std::cerr << "Failed configure I2C slave addr: "
                  << strerror(errno) << std::endl;
        return -1;
    }
 
    if (ioctl(fd, BBB_I2C_SET_REG_ADDR, &reg_addr) < 0) {
        std::cerr << "Failed configure I2C reg addr: "
                  << strerror(errno) << std::endl;
        return -1;
    }
 
    for (int i = 0; i < len; i++){
        /* Call kernel write func */
        count += ::write(fd, &buffer[i], 1);
        if (count < 0) return -1;
    }
    
    return count;
}
 
ssize_t I2c::read(void* buffer, uint8_t slave_addr, uint8_t reg_addr, size_t length) {
    if (!isOpen()) return -1;
 
    if (ioctl(fd, BBB_I2C_SET_SLAVE_ADDR, &slave_addr) < 0) {
        std::cerr << "Failed configure I2C slave addr: "
                  << strerror(errno) << std::endl;
        return -1;
    }
 
    if (ioctl(fd, BBB_I2C_SET_REG_ADDR, &reg_addr) < 0) {
        std::cerr << "Failed configure I2C reg addr: "
                  << strerror(errno) << std::endl;
        return -1;
    }
 
    /* Call kernel read func */
    return ::read(fd, buffer, length);
}
 
// Move Constructor
I2c::I2c(I2c&& other) noexcept
    : fd(other.fd),
      device_path(std::move(other.device_path))
{
    other.fd = -1;                    // Steal the resource
    // other.device_path is already moved
}
 
// Move Assignment Operator
I2c& I2c::operator=(I2c&& other) noexcept
{
    if (this != &other) {
        deinit();                     // Clean up current resource
 
        fd = other.fd;
        device_path = std::move(other.device_path);
 
        other.fd = -1;                // Leave other in valid moved-from state
    }
    return *this;
}
