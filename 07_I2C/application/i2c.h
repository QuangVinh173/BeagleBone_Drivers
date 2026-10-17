#ifndef I2C_H
#define I2C_H
 
#include <string>
#include <cstdint>
#include <linux/ioctl.h>
#include <linux/types.h>
 
struct bbb_i2c_config {
    /* Supported standard and fast speed mode in Hz */
    __u32 speed;
    __u8 slave_addr;
    __u8 reg_addr;
    bool use_dma;
};
 
#define BBB_I2C_IOC_MAGIC 'U'
 
#define BBB_I2C_SET_CONFIG \
    _IOW(BBB_I2C_IOC_MAGIC, 1, struct bbb_i2c_config)
 
#define BBB_I2C_SET_SLAVE_ADDR \
    _IOW(BBB_I2C_IOC_MAGIC, 2, __u8)
 
#define BBB_I2C_SET_REG_ADDR \
    _IOW(BBB_I2C_IOC_MAGIC, 3, __u8)
 
 
class I2c {
private:
    int fd = -1;
    std::string device_path;
 
public:
    I2c(const std::string& dev_path = "/dev/i2c1");
    ~I2c();                    // Automatic cleanup in destructor
 
    // Disable Copy Semantics (important for classes holding file descriptors)
    I2c(const I2c&) = delete;
    I2c& operator=(const I2c&) = delete;
 
    // Enable Move Semantics
    I2c(I2c&& other) noexcept;
    I2c& operator=(I2c&& other) noexcept;
 
    // Initialize (open the device)
    bool init(struct bbb_i2c_config *config);
 
    // De-initialize (close the device)
    void deinit();
 
    // Write data
    // ssize_t write(const void* data, size_t length);
    ssize_t write(uint8_t *buffer, uint8_t slave_addr, uint8_t reg_addr, int len);
 
    // Read data
    ssize_t read(void* buffer, uint8_t slave_addr, uint8_t reg_addr, size_t length);
    
    // Convenience: Read with timeout (in milliseconds)
    ssize_t readTimeout(void* buffer, size_t length, int timeout_ms);
 
    bool isOpen() const { return fd >= 0; }
    std::string getDevicePath() const { return device_path; }
};
 
#endif // I2C_H
