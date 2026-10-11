#ifndef SPI_H
#define SPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

typedef enum spi_mode {
    SPI_MODE_0 = 0, /* CPOL = 0, CPHA = 0 */
    SPI_MODE_1,     /* CPOL = 0, CPHA = 1 */
    SPI_MODE_2,     /* CPOL = 1, CPHA = 0 */
    SPI_MODE_3,     /* CPOL = 1, CPHA = 1 */
} spi_mode;

struct bbb_spi_config {
    __u8 clk_divided;
    spi_mode mode;
    bool use_dma;
};

#define BBB_SPI_IOC_MAGIC 'U'

#define BBB_SPI_SET_CONFIG \
    _IOW(BBB_SPI_IOC_MAGIC, 1, struct bbb_spi_config)

#endif