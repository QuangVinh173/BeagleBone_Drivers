#ifndef BBB_I2C_IOCTL_H
#define BBB_I2C_IOCTL_H
 
#include <linux/ioctl.h>
#include <linux/types.h>
 
struct bbb_i2c_config {
    /* Supported standard and fast speed mode */
    __u32 speed;
    __u8 slave_addr;
    __u8 reg_addr;
};
 
#define BBB_I2C_IOC_MAGIC 'U'
 
#define BBB_I2C_SET_CONFIG \
    _IOW(BBB_I2C_IOC_MAGIC, 1, struct bbb_i2c_config)
 
#define BBB_I2C_SET_SLAVE_ADDR \
    _IOW(BBB_I2C_IOC_MAGIC, 2, __u8)
 
#define BBB_I2C_SET_REG_ADDR \
    _IOW(BBB_I2C_IOC_MAGIC, 3, __u8)
 
#endif