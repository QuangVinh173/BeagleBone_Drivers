#include "uart_app.h"
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>

Uart::Uart(const std::string& dev_path) 
    : device_path(dev_path) {}

Uart::~Uart() {
    deinit();
}

bool Uart::init() {
    if (isOpen()) {
        return true;
    }

    fd = open(device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": " << strerror(errno) << std::endl;
        return false;
    }

    // Optional: Set raw mode (recommended for custom drivers)
    struct termios options;
    if (tcgetattr(fd, &options) == 0) {
        cfmakeraw(&options);
        tcsetattr(fd, TCSANOW, &options);
    }

    std::cout << "UART opened: " << device_path << std::endl;
    return true;
}

void Uart::deinit() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
        std::cout << "UART closed: " << device_path << std::endl;
    }
}

ssize_t Uart::write(const void* data, size_t length) {
    if (!isOpen()) return -1;
    return ::write(fd, data, length);
}

ssize_t Uart::write(const std::string& data) {
    return write(data.c_str(), data.length());
}

ssize_t Uart::read(void* buffer, size_t length) {
    if (!isOpen()) return -1;
    return ::read(fd, buffer, length);
}

ssize_t Uart::readTimeout(void* buffer, size_t length, int timeout_ms) {
    if (!isOpen()) return -1;

    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (ret > 0) {
        return ::read(fd, buffer, length);
    } else if (ret == 0) {
        return 0; // timeout
    } else {
        return -1; // error
    }
}

// Move Constructor
Uart::Uart(Uart&& other) noexcept
    : fd(other.fd),
      device_path(std::move(other.device_path))
{
    other.fd = -1;                    // Steal the resource
    // other.device_path is already moved
}

// Move Assignment Operator
Uart& Uart::operator=(Uart&& other) noexcept
{
    if (this != &other) {
        deinit();                     // Clean up current resource

        fd = other.fd;
        device_path = std::move(other.device_path);

        other.fd = -1;                // Leave other in valid moved-from state
    }
    return *this;
}
