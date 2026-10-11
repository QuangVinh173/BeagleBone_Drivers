#ifndef UART_H
#define UART_H

#include <string>
#include <cstdint>
#include <linux/ioctl.h>
#include <linux/types.h>

struct bbb_uart_config {
    __u32 baudrate;
};

#define BBB_UART_IOC_MAGIC 'U'

#define BBB_UART_SET_CONFIG \
    _IOW(BBB_UART_IOC_MAGIC, 1, struct bbb_uart_config)


class Uart {
private:
    int fd = -1;
    std::string device_path;

public:
    Uart(const std::string& dev_path = "/dev/uart1");
    ~Uart();                    // Automatic cleanup in destructor

    // Disable Copy Semantics (important for classes holding file descriptors)
    Uart(const Uart&) = delete;
    Uart& operator=(const Uart&) = delete;

    // Enable Move Semantics
    Uart(Uart&& other) noexcept;
    Uart& operator=(Uart&& other) noexcept;

    // Initialize (open the device)
    bool init(struct bbb_uart_config *config);

    // De-initialize (close the device)
    void deinit();

    // Write data
    ssize_t write(const void* data, size_t length);
    ssize_t write(const std::string& data);

    // Read data
    ssize_t read(void* buffer, size_t length);
    
    // Convenience: Read with timeout (in milliseconds)
    ssize_t readTimeout(void* buffer, size_t length, int timeout_ms);

    bool isOpen() const { return fd >= 0; }
    std::string getDevicePath() const { return device_path; }
};

#endif // UART_H

