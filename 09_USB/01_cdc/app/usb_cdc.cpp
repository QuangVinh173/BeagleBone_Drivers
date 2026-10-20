#include "usb_cdc.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr std::size_t MAX_RESPONSE_SIZE = 4096;
constexpr int READ_STREAM_TIMEOUT_MS = 2500;

std::string usbVersionToString(std::uint16_t bcdUSB)
{
    std::ostringstream oss;
    oss << ((bcdUSB >> 8) & 0xFF) << "."
        << std::hex << std::uppercase
        << ((bcdUSB >> 4) & 0x0F)
        << (bcdUSB & 0x0F);
    return oss.str();
}

std::string makeCommand(const char* prefix, const std::string& message)
{
    std::string command(prefix);
    command += message;
    command += '\n';
    return command;
}
} // namespace

CDCUSB::CDCUSB() : fd_(-1)
{
}

CDCUSB::~CDCUSB()
{
    closeDevice();
}

bool CDCUSB::isOpen() const
{
    return fd_ >= 0;
}

bool CDCUSB::openDevice()
{
    if (isOpen())
        return true;

    fd_ = open("/dev/usb1", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        std::cerr << "Failed to open /dev/usb1: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    std::cout << "Opened custom USB device: /dev/usb1\n";
    return true;
}

void CDCUSB::closeDevice()
{
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void CDCUSB::checkConnection() const
{
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    cdc_status status{};
    if (ioctl(fd_, BBB_CDC_CONNECTION, &status) < 0) {
        std::cerr << "BBB_CDC_CONNECTION failed: "
                  << std::strerror(errno) << '\n';
        return;
    }

    std::cout << "\n========================================\n"
              << "          USB DEVICE STATUS\n"
              << "========================================\n";

    if (!status.connected) {
        std::cout << "No device connected\n";
    } else if (status.full_speed) {
        std::cout << "Full-speed device connected\n";
    } else {
        std::cout << "Low-speed device connected\n";
    }
}

void CDCUSB::checkInfo() const
{
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    cdc_info info{};
    if (ioctl(fd_, BBB_CDC_INFO, &info) < 0) {
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

bool CDCUSB::writeData(const std::string& data)
{
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return false;
    }

    std::size_t total = 0;
    while (total < data.size()) {
        const ssize_t n = write(fd_, data.data() + total, data.size() - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "Write failed: " << std::strerror(errno) << '\n';
            return false;
        }
        if (n == 0) {
            std::cerr << "Write returned zero bytes.\n";
            return false;
        }
        total += static_cast<std::size_t>(n);
    }

    // minus len of "[writeread] "
    std::cout << "Sent " << total << " bytes\n";
    return true;
}

/*
 * Return exactly one newline-terminated response. If one read() obtains
 * multiple lines, preserve all bytes after the first newline for the next call.
 */
bool CDCUSB::readData(std::string& response, int timeoutMs)
{
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return false;
    }
    if (timeoutMs <= 0) {
        std::cerr << "timeoutMs must be positive.\n";
        return false;
    }

    response.clear();
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    while (true) {
        const auto newline = pendingRx_.find('\n');
        if (newline != std::string::npos) {
            response.assign(pendingRx_, 0, newline);
            pendingRx_.erase(0, newline + 1);
            if (!response.empty() && response.back() == '\r')
                response.pop_back();

            std::cout << "Received[" << response.size() << "]: '"
                      << response << "'\n";
            return true;
        }

        if (pendingRx_.size() >= MAX_RESPONSE_SIZE) {
            std::cerr << "Response exceeded maximum size without a line terminator.\n";
            pendingRx_.clear();
            return false;
        }

        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        char buffer[256];
        const ssize_t n = read(fd_, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
                return false;

            std::cerr << "Read failed: " << std::strerror(errno) << '\n';
            return false;
        }
        if (n == 0) {
            std::cerr << "Read returned zero bytes; device may be disconnected.\n";
            return false;
        }

        pendingRx_.append(buffer, static_cast<std::size_t>(n));
    }
}

void CDCUSB::writeOption()
{
    std::string message;
    std::cout << "Enter message ([write] is added automatically): ";
    if (!std::getline(std::cin, message))
        return;

    if (!writeData(makeCommand("[write]", message)))
        return;

    std::cout << "Write-only command sent; no Arduino response is expected.\n";
}

void CDCUSB::readOption()
{
    std::string message;
    std::cout << "Enter message ([read] is added automatically): ";
    if (!std::getline(std::cin, message))
        return;

    if (!writeData(makeCommand("[read]", message)))
        return;

    std::cout << "Streaming Arduino responses. Press Enter to stop and return to the menu.\n";

    std::atomic<bool> stopRequested{false};
    std::thread stopReader([&stopRequested]() {
        std::string ignored;
        std::getline(std::cin, ignored);
        stopRequested.store(true);
    });

    while (!stopRequested.load()) {
        std::string response;
        if (!readData(response, READ_STREAM_TIMEOUT_MS)) {
            // A timeout is acceptable in stream mode; keep waiting unless asked to stop.
            if (!stopRequested.load() && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != ETIMEDOUT) {
                // readData() already reports non-timeout I/O errors.
                if (!isOpen())
                    break;
            }
        }
    }

    // The Arduino treats a new command as cancellation of the repeating [read] mode.
    if (isOpen())
        writeData(makeCommand("[write]", "stop"));

    if (stopReader.joinable())
        stopReader.join();

    std::cout << "Read mode stopped.\n";
}

void CDCUSB::writeReadOption()
{
    std::string message;
    std::string response;

    std::cout << "Enter message ([writeread] is added automatically): ";
    if (!std::getline(std::cin, message))
        return;

    if (!writeData(makeCommand("[writeread]", message)))
        return;

    std::cout << "Waiting for Arduino echo back ...\n";
    if (!readData(response, 3000))
        std::cout << "Transaction failed or timed out.\n";
}

