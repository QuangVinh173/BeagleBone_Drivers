#ifndef SPI_H
#define SPI_H

#include <string>
#include <cstdint>
#include <linux/ioctl.h>
#include <linux/types.h>

enum spi_mode {
    SPI_MODE_0, /* CPOL = 0, CPHA = 0 */
    SPI_MODE_1, /* CPOL = 0, CPHA = 1 */
    SPI_MODE_2, /* CPOL = 1, CPHA = 0 */
    SPI_MODE_3, /* CPOL = 1, CPHA = 1 */
};

struct bbb_spi_config {
    __u8 clk_divided;
    spi_mode mode;
    bool use_dma;
};

#define BBB_SPI_IOC_MAGIC 'U'

#define BBB_SPI_SET_CONFIG \
    _IOW(BBB_SPI_IOC_MAGIC, 1, struct bbb_spi_config)


class Spi {
private:
    int fd = -1;
    std::string device_path;

public:
    Spi(const std::string& dev_path);
    ~Spi();                    // Automatic cleanup in destructor

    // Disable Copy Semantics (important for classes holding file descriptors)
    Spi(const Spi&) = delete;
    Spi& operator=(const Spi&) = delete;

    // Enable Move Semantics
    Spi(Spi&& other) noexcept;
    Spi& operator=(Spi&& other) noexcept;

    // Initialize (open the device)
    bool init(struct bbb_spi_config *config);

    // De-initialize (close the device)
    void deinit();

    // Write data
    ssize_t write(const void* data, size_t length);
    ssize_t write(const std::string& data);

    // Read data
    ssize_t read(void* buffer, size_t length);

    bool isOpen() const { return fd >= 0; }
    std::string getDevicePath() const { return device_path; }
};

#endif // SPI_H

