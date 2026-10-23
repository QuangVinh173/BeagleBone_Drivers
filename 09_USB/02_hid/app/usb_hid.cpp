#include "usb_hid.h"
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

HIDUSB::HIDUSB() : fd_(-1) {}
HIDUSB::~HIDUSB() { closeDevice(); }

std::string usbVersionToString(std::uint16_t bcdUSB)
{
    std::ostringstream oss;
    oss << ((bcdUSB >> 8) & 0xFF) << "."
        << std::hex << std::uppercase
        << ((bcdUSB >> 4) & 0x0F)
        << (bcdUSB & 0x0F);
    return oss.str();
}

bool HIDUSB::openDevice() {
    if (isOpen()) return true;
    fd_ = ::open(DEVICE_PATH, O_RDWR);
    if (fd_ < 0) {
        std::cerr << "open(" << DEVICE_PATH << ") failed: "
                  << std::strerror(errno) << '\n';
        return false;
    }
    pendingRx_.clear();
    pendingMouseRx_.clear();
    return true;
}

void HIDUSB::closeDevice() {
    if (isOpen()) ::close(fd_);
    fd_ = -1;
    pendingRx_.clear();
    pendingMouseRx_.clear();
}

bool HIDUSB::isOpen() const { return fd_ >= 0; }

void HIDUSB::checkConnection() const {
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    hid_status status{};
    if (ioctl(fd_, BBB_HID_CONNECTION, &status) < 0) {
        std::cerr << "BBB_HID_CONNECTION failed: "
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

void HIDUSB::checkInfo() const {
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    hid_info info{};
    if (ioctl(fd_, BBB_HID_INFO, &info) < 0) {
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

bool HIDUSB::writeData(const std::string& data) {
    if (!isOpen()) return false;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::write(fd_, data.data() + sent, data.size() - sent);
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{fd_, POLLOUT, 0};
            if (::poll(&pfd, 1, 3000) > 0) continue;
        }
        std::cerr << "write failed: " << (n == 0 ? "zero bytes" : std::strerror(errno)) << '\n';
        return false;
    }
    return true;
}

bool HIDUSB::readData(std::string& response, int timeoutMs) {
    response.clear();
    if (!isOpen() || timeoutMs < 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const auto pos = pendingRx_.find('\n');
        if (pos != std::string::npos) {
            response = pendingRx_.substr(0, pos + 1);
            pendingRx_.erase(0, pos + 1);
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        pollfd pfd{fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, std::max(1, remaining));
        if (ready == 0) return false;
        if (ready < 0) { if (errno == EINTR) continue; std::cerr << "poll failed: " << std::strerror(errno) << '\n'; return false; }
        if (pfd.revents & (POLLERR | POLLNVAL)) return false;
        char buffer[256];
        const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
        if (n > 0) { pendingRx_.append(buffer, static_cast<std::size_t>(n)); continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n < 0) std::cerr << "read failed: " << std::strerror(errno) << '\n';
        return false;
    }
}

void HIDUSB::writeOption() {
    std::string data;
    std::cout << "Data to send: ";
    if (!std::getline(std::cin, data)) return;
    if (writeData(data)) std::cout << "Sent " << data.size() << " bytes.\n";
}

void HIDUSB::readOption() {
    std::string response;
    if (readData(response)) std::cout << "Received: " << response;
    else std::cout << "No complete newline-terminated response (timeout/error).\n";
}

void HIDUSB::writeReadOption() {
    std::string data;
    std::cout << "Data to send: ";
    if (!std::getline(std::cin, data)) return;
    if (!writeData(data)) return;
    std::string response;
    if (readData(response)) std::cout << "Received: " << response;
    else std::cout << "No complete newline-terminated response (timeout/error).\n";
}

void HIDUSB::readMouseNeutralOption(){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    std::cout << "\nWaiting for mouse neutral report...\n"
              << "Expected: report_id=0x01, button=0, dx=0, dy=0, dz=0\n"
              << "Press Enter to return to menu.\n";

    // Save terminal settings and configure single-character input.
    struct termios oldTermios;
    bool restoreTerminal = false;

    if (tcgetattr(STDIN_FILENO, &oldTermios) == 0) {
        struct termios newTermios = oldTermios;
        newTermios.c_lflag &= ~(ICANON | ECHO);

        if (tcsetattr(STDIN_FILENO, TCSANOW, &newTermios) == 0)
            restoreTerminal = true;
    }

    char buffer[64];
    bool exitRequested = false;

    while (!exitRequested) {
        struct pollfd fds[2] = {
            { fd_, POLLIN, 0 },
            { STDIN_FILENO, POLLIN, 0 }
        };

        // Wait for either USB data or keyboard input.
        const int pollRet = poll(fds, 2, -1);

        if (pollRet < 0) {
            if (errno == EINTR)
                continue;

            std::cerr << "poll() failed: "
                      << std::strerror(errno) << '\n';
            break;
        }

        // Check keyboard first so Enter can exit immediately.
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char key;
            const ssize_t keyRead = read(STDIN_FILENO, &key, 1);

            if (keyRead > 0 && (key == '\n' || key == '\r')) {
                std::cout << "\nReturning to menu...\n";
                exitRequested = true;
                break;
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "USB device disconnected or poll error.\n";
            break;
        }

        if (!(fds[0].revents & POLLIN))
            continue;

        const ssize_t n = read(fd_, buffer, sizeof(buffer));

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;

            std::cerr << "read() failed: "
                      << std::strerror(errno) << '\n';
            break;
        }

        if (n == 0) {
            std::cerr << "Read returned zero bytes; device may be disconnected.\n";
            break;
        }

        pendingMouseRx_.append(buffer, static_cast<std::size_t>(n));

        while (1) {
            const auto* report =
                reinterpret_cast<const std::uint8_t*>(
                    pendingMouseRx_.data());

            /* Usually for wireless mouse */
            if (n >= 6){
                const std::uint8_t reportId = report[0];
                const std::uint8_t button   = report[1];
                const std::int8_t dx = static_cast<std::int8_t>(report[2]);
                const std::int8_t dy = static_cast<std::int8_t>(report[3]);
                const std::int8_t dz = static_cast<std::int8_t>(report[4]);

                std::cout << "Mouse report: "
                        << std::hex
                        << static_cast<unsigned>(report[0]) << ' '
                        << static_cast<unsigned>(report[1]) << ' '
                        << static_cast<unsigned>(report[2]) << ' '
                        << static_cast<unsigned>(report[3]) << ' '
                        << static_cast<unsigned>(report[4]) << ' '
                        << static_cast<unsigned>(report[5])
                        << std::dec << '\n';

                std::cout << "report_id = 0x" << std::hex
                        << static_cast<unsigned>(reportId) << std::dec
                        << ", button = " << static_cast<unsigned>(button)
                        << ", dx = " << static_cast<int>(dx)
                        << ", dy = " << static_cast<int>(dy)
                        << ", dz = " << static_cast<int>(dz) << '\n';

                pendingMouseRx_.erase(0, n);

                break;
            }

            /* Usually for wired mouse */
            else {
                const std::uint8_t button   = report[0];
                const std::int8_t dx = static_cast<std::int8_t>(report[1]);
                const std::int8_t dy = static_cast<std::int8_t>(report[2]);
                const std::int8_t dz = static_cast<std::int8_t>(report[3]);

                std::cout << "Mouse report: "
                        << std::hex
                        << static_cast<unsigned>(report[0]) << ' '
                        << static_cast<unsigned>(report[1]) << ' '
                        << static_cast<unsigned>(report[2]) << ' '
                        << static_cast<unsigned>(report[3])
                        << std::dec << '\n';

                std::cout 
                        << "button = " << static_cast<unsigned>(button)
                        << ", dx = " << static_cast<int>(dx)
                        << ", dy = " << static_cast<int>(dy)
                        << ", dz = " << static_cast<int>(dz) << '\n';

                pendingMouseRx_.erase(0, n);

                break;
            }

        }
    }

    // Restore the terminal before returning to the menu.
    if (restoreTerminal)
        tcsetattr(STDIN_FILENO, TCSANOW, &oldTermios);
}

void HIDUSB::CaptureOption(){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    bool capture = true;
    if (ioctl(fd_, BBB_HID_CAPTURE, &capture) < 0) {
        std::cerr << "Failed to capture usb hid: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        //return false;
    }
}

void HIDUSB::NoCaptureOption(){
    if (!isOpen()) {
        std::cerr << "Device is not open.\n";
        return;
    }

    bool capture = false;
    if (ioctl(fd_, BBB_HID_NOT_CAPTURE, &capture) < 0) {
        std::cerr << "Failed to disable capture usb hid: "
                  << strerror(errno) << std::endl;

        close(fd_);
        fd_ = -1;
        //return false;
    }
}
