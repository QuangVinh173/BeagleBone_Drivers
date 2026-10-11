#ifndef BBB_UART_IOCTL_H
#define BBB_UART_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct bbb_uart_config {
    __u32 baudrate;
};

#define BBB_UART_IOC_MAGIC 'U'

#define BBB_UART_SET_CONFIG \
    _IOW(BBB_UART_IOC_MAGIC, 1, struct bbb_uart_config)

#endif