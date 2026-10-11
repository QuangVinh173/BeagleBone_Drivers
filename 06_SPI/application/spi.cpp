#include "spi.h"
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>

/* Constructor */
Spi::Spi(const std::string& dev_path) 
    : device_path(dev_path) {}

/* destructor */
Spi::~Spi() {
    deinit(); // close fd
}

bool Spi::init(struct bbb_spi_config *config) {
    if (isOpen()) {
        return true;
    }

    fd = open(device_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (ioctl(fd, BBB_SPI_SET_CONFIG, config) < 0) {
        std::cerr << "Failed to configure SPI: "
                  << strerror(errno) << std::endl;

        close(fd);
        fd = -1;
        return false;
    }

    std::cout << "SPI opened and configured: " << device_path << std::endl;
    return true;
}

void Spi::deinit() {
    if (fd >= 0) {
        close(fd);
        fd = -1;
        std::cout << "SPI closed: " << device_path << std::endl;
    }
}

ssize_t Spi::write(const std::string& data) {
    if (!isOpen()) return -1;

    /* Call kernel write func */
    return ::write(fd, data.c_str(), data.length());
}

ssize_t Spi::read(void* buffer, size_t length) {
    if (!isOpen()) return -1;

    /* Call kernel read func */
    return ::read(fd, buffer, length);
}

// Move Constructor
Spi::Spi(Spi&& other) noexcept
    : fd(other.fd),
      device_path(std::move(other.device_path))
{
    other.fd = -1;                    // Steal the resource
    // other.device_path is already moved
}

// Move Assignment Operator
Spi& Spi::operator=(Spi&& other) noexcept
{
    if (this != &other) {
        deinit();                     // Clean up current resource

        fd = other.fd;
        device_path = std::move(other.device_path);

        other.fd = -1;                // Leave other in valid moved-from state
    }
    return *this;
}

