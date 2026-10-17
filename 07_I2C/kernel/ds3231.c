#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include "ds3231.h"
#include "i2c_ioctl.h"
 
#define SLAVE_ADDRESS 0x68
 
// Register base addresses
#define I2C1_BASE 0x4802A000
#define GPIO_BASE 0x44e10000
#define CM_PER_BASE 0x44E00000
 
// CM_PER registers
// #define CM_PER_I2C1_CLKCTRL    (*(volatile uint32_t *)(CM_PER_BASE + 0x48))
 
// I2C1 registers (offsets)
#define I2C_SYSC 0x10          // System configuration
#define I2C_SYSS 0x90          // System status
#define I2C_CON 0xA4           // Control
#define I2C_PSC 0xB0           // Prescaler
#define I2C_SCLL 0xB4          // SCL low time
#define I2C_SCLH 0xB8          // SCL high time
#define I2C_SA 0xAC            // Slave address
#define I2C_OA 0xA8            // Own address
#define I2C_CNT 0x98           // Data count
#define I2C_DATA 0x9C          // Data
#define I2C_IRQENABLE_CLR 0x30 // Enable interrupts
#define I2C_IRQENABLE_SET 0x2C // Enable interrupts
#define I2C_IRQSTATUS_RAW 0x24 // Interrupt raw status
#define I2C_IRQSTATUS 0x28     // Interrupt status
#define I2C_BUF 0x94           // Buffer
#define I2C_BUFSTAT 0xC0       // Buffer ststus
#define I2C_DMATXENABLE_SET 0x3C
 
#define I2C_IRQSTATUS_RAW_XRDY BIT(4)
#define I2C_IRQSTATUS_RAW_BB BIT(12)
// #define I2C_IRQSTATUS_RAW_BF BIT(😎
#define I2C_IRQSTATUS_RAW_ARDY BIT(2)
#define I2C_IRQSTATUS_RAW_NACK BIT(1)
#define I2C_IRQSTATUS_RAW_RRDY BIT(3)
 
#define XRDY_IE BIT(4)
#define RRDY_IE BIT(3)
#define AAS_IE BIT(9)
#define AERR_IE BIT(7)
#define ARDY_IE BIT(2)
 
#define I2C_BUF_RXTRSH 8 // [13:8]
#define I2C_BUF_RXFIFO_CLR BIT(14)
#define I2C_BUF_TXTRSH 0 // [5:0]
#define I2C_BUF_TXFIFO_CLR BIT(6)
#define XDMA_EN BIT(8)
 
#define DRIVER_NAME "i2c1_device_driver"
#define DEVICE_NAME "i2c1"
 
struct i2c_device_data
{
    struct i2c_adapter *adap;
 
    dev_t dev_num;
    struct cdev cdev;
    struct class *class;
    struct device *dev;
    struct device *char_dev;
 
    void __iomem *base; // Mapped base address of i2c1 registers
    void __iomem *base_gpio;
    struct clk *clk;
 
    int irq;
    int count;
    int data_count;
 
    u8 *rx; // used as a pointer to read user data
    u8 byte;
    u8 byte_tx;
    u8 ardy_check;
 
    u8 len;
    u8 rx_buf[256];
    u8 tx_buf[256];
 
    spinlock_t lock; // Add spinlock for synchronization
 
    u8 slave_addr;
    u8 reg_addr;
    bool data_ready;
    bool done_configured;
    wait_queue_head_t recv_wait;
};
unsigned long flags;
 
// IRQ handler
static irqreturn_t irqHandler(int irq, void *d)
{
    struct i2c_device_data *data = d;
    u32 irqsts;
 
    if (!data)
    {
        pr_err("NULL data in irqHandler\n");
        return IRQ_NONE;
    }
 
    if (!data->done_configured)
        return IRQ_HANDLED;
 
    spin_lock_irqsave(&data->lock, flags);
    irqsts = ioread32(data->base + I2C_IRQSTATUS);
 
    if ((irqsts & RRDY_IE) == RRDY_IE)
    {
 
        data->rx[data->data_count++] = ioread32(data->base + I2C_DATA); // Read bytes
 
        data->byte = 1;
 
        data->data_count = 0;
        data->count = 0;
 
        // Clear the XRDY interrupt
        irqsts |= RRDY_IE;
        iowrite32(irqsts, data->base + I2C_IRQSTATUS);
 
        data->data_ready = true;
        wake_up_interruptible(&data->recv_wait);
    }
 
    if ((irqsts & AAS_IE) == AAS_IE)
    {
 
        // used when an address slave is choosen by master
        // Clear the AAS interrupt
        irqsts |= AAS_IE;
        iowrite32(irqsts, data->base + I2C_IRQSTATUS);
    }
 
    if ((irqsts & AERR_IE) == AERR_IE)
    {
        printk("aerr, bufsts = 0x%x\n", ioread32(data->base + I2C_BUFSTAT));
        data->count = 0;
 
        // used when an address slave is choosen by master
        // Clear the AAS interrupt
        irqsts |= AERR_IE;
        iowrite32(irqsts, data->base + I2C_IRQSTATUS);
    }
 
    if ((irqsts & ARDY_IE) == ARDY_IE)
    {
 
        // iowrite32(data->tx_buf[data->data_count++], data->base + I2C_DATA);
 
        data->count = 0;
 
        data->ardy_check = 1;
 
        // printk("Ardy\n", ioread32(data->base + I2C_BUFSTAT));
 
        // Clear the XRDY_IE interrupt
        irqsts |= ARDY_IE;
        iowrite32(irqsts, data->base + I2C_IRQSTATUS);
    }
 
    data->count++;
    if (data->count > 100)
    {
        printk("Too many interrupts, 0x%x\n", ioread32(data->base + I2C_IRQSTATUS));
        iowrite32(0, data->base + I2C_IRQENABLE_SET);
        iowrite32(0xFF, data->base + I2C_IRQENABLE_CLR);
        data->count = 0;
    }
 
    spin_unlock_irqrestore(&data->lock, flags);
 
    return IRQ_HANDLED;
}
 
// init speed
void init_clk2(struct i2c_device_data *data, u32 fclk_rate, u32 speed)
{
    u32 scll = 0, sclh = 0;
    u8 psc = 0;
    u32 internal_speed;
 
    if (speed <= 100 && speed > 0) internal_speed = 4000;
    else internal_speed = 9600;
 
    /* Compute prescaler divisor: Fclk -> F_internal */
    psc = fclk_rate / internal_speed;
    psc = psc - 1;
    iowrite32(psc, data->base + I2C_PSC);
 
    /* Standard speed mode */
    if (speed <= 100 && speed > 0)
    {
        /* F_internal -> time_H and time_L for SCL period */
        scll = internal_speed / (speed * 2) - 7;
        sclh = internal_speed / (speed * 2) - 5;
    }
 
    /* Fast speed mode */
    else if (speed > 100 && speed <= 400)
    {
        int scl_cycle = internal_speed / speed;
        /* F_internal -> time_H and time_L for SCL period */
        scll = scl_cycle - (scl_cycle / 3) - 7;
        sclh = (scl_cycle / 3) - 5;
    }
 
    /* If speed unconfigured -> use default 400 KHz*/
    else
    {
        /* F_internal -> time_H and time_L for SCL period */
        scll = internal_speed / (speed * 2) - 7;
        sclh = internal_speed / (speed * 2) - 5;
    }
 
    iowrite32(scll, data->base + I2C_SCLL); // SCL low time
    iowrite32(sclh, data->base + I2C_SCLH); // SCL high time
 
    dev_info(data->dev, "pdc = %d, scll = %d. sclh = %d\n", psc, scll, sclh);
}
 
// Initialize I2C1 as master
void i2c1_master_init(struct i2c_device_data* data, struct bbb_i2c_config* config)
{
    // enable_i2c1_clock(data);
    u32 i2c_con = 0;
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con &= ~(1u << 15); // [15] disable i2c module before reset
    iowrite32(i2c_con, data->base + I2C_CON);
 
    dev_info(data->dev, "Begin reset\n");
    iowrite32(0x2, data->base + I2C_SYSC); // Set soft reset
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con |= (1u << 15); // [15] enable i2c module before reset
    iowrite32(i2c_con, data->base + I2C_CON);
 
    dev_info(data->dev, "Wait to reset ...\n");
    while (!(ioread32(data->base + I2C_SYSS) & (1u)))
        ;                                  // Wait for reset complete
    iowrite32(0x0, data->base + I2C_SYSC); // Clear reset - normal mode
    dev_info(data->dev, "Done reset\n");
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con &= ~(1u << 15); // disable i2c module
    iowrite32(i2c_con, data->base + I2C_CON);
 
    /* Init clk in kHz */
    init_clk2(data, clk_get_rate(data->clk)/1000, config->speed);
 
    iowrite32(0x30, data->base + I2C_OA);
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con |= (1u << 15) | (1u << 10) | (1u << 9); // [15] enable i2c module, [MST:10]: master mode, [TRX:9]: MST = 1, TRX = 1, Operating Modes = Master transmitter
    iowrite32(i2c_con, data->base + I2C_CON);

    iowrite32((RX_TRIGGER << I2C_BUF_RXTRSH) | (TX_TRIGGER << I2C_BUF_TXTRSH) | 
               I2C_BUF_RXFIFO_CLR | I2C_BUF_TXFIFO_CLR, data->base + I2C_BUF); // clear RX FIFO, RX threshold is 1 byte
 
    iowrite32(RRDY_IE | AAS_IE | AERR_IE | ARDY_IE, data->base + I2C_IRQENABLE_SET); // Receive data ready interrupt enabled


}
 
void i2c1_master_deinit(struct i2c_device_data* data)
{
    u32 i2c_con = 0;
    printk("-> i2c1_master_deinit\n");
    // enable_i2c1_clock(data);
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con &= ~(1u << 15); // disable i2c module
    iowrite32(i2c_con, data->base + I2C_CON);

    iowrite32(0x2, data->base + I2C_SYSC); // Set soft reset
 
#if (IRQ_USED)
    iowrite32(0, data->base + I2C_BUF); // clear RX FIFO, RX threshold 
    iowrite32(0, data->base + I2C_IRQENABLE_SET); // Receive data ready interrupt enabled
#endif
}

void i2c_start(struct i2c_device_data *data)
{
    u32 i2c_con = 0;
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con &= ~0x2;
    i2c_con |= 0x1; // Start condition
    iowrite32(i2c_con, data->base + I2C_CON);
}
 
void i2c_stop(struct i2c_device_data *data)
{
    u32 i2c_con = 0;
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con &= ~0x1;
    i2c_con |= 0x2; // Stop condition
    iowrite32(i2c_con, data->base + I2C_CON);
}
 
void i2c_reinit_master_transmit(struct i2c_device_data *data)
{
    u32 i2c_con = 0;
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con |= (1u << 15) | (1u << 10) | (1u << 9); // [15] enable i2c module, [MST:10]: master mode, [TRX:9]: MST = 1, TRX = 1, Operating Modes = Master transmitter
    iowrite32(i2c_con, data->base + I2C_CON);
}
 
void i2c_reinit_master_receive(struct i2c_device_data *data)
{
    u32 i2c_con = 0;
 
    i2c_con = ioread32(data->base + I2C_CON);
    i2c_con |= (1u << 15) | (1u << 10); // [15] enable i2c module, [MST:10]: master mode, [TRX:9]: MST = 1, TRX = 0, Operating Modes = Master receiver
    i2c_con &= ~(1u << 9);
    iowrite32(i2c_con, data->base + I2C_CON);
}
 
int i2c_wait_BB(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    // printk("cnt BB = %d\n", ioread32(data->base + I2C_CNT));
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & I2C_IRQSTATUS_RAW_BB) == I2C_IRQSTATUS_RAW_BB) // Wait for bus to be free
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout BB, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
int i2c_wait_BB_occupied(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    // printk("cnt BB = %d\n", ioread32(data->base + I2C_CNT));
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & I2C_IRQSTATUS_RAW_BB) != I2C_IRQSTATUS_RAW_BB) // Wait for bus to be free
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout BB, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
int i2c_wait_XRDY(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & (I2C_IRQSTATUS_RAW_XRDY)) != I2C_IRQSTATUS_RAW_XRDY) // Wait for XRDY (transmit data ready)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout XRDY-I, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
void i2c_clr_XRDY(struct i2c_device_data *data)
{
    u32 i2c_sts_raw = 0;
 
    i2c_sts_raw = ioread32(data->base + I2C_IRQSTATUS_RAW);
    i2c_sts_raw |= I2C_IRQSTATUS_RAW_XRDY; // Clear XRDY
    iowrite32(i2c_sts_raw, data->base + I2C_IRQSTATUS_RAW);
}
 
int i2c_wait_RRDY(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & (I2C_IRQSTATUS_RAW_RRDY)) != I2C_IRQSTATUS_RAW_RRDY) // Wait for RRDY (receive data ready)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout RRDY-I, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
void i2c_clr_RRDY(struct i2c_device_data *data)
{
    u32 i2c_sts_raw = 0;
 
    i2c_sts_raw = ioread32(data->base + I2C_IRQSTATUS_RAW);
    i2c_sts_raw |= I2C_IRQSTATUS_RAW_RRDY; // Clear RRDY
    iowrite32(i2c_sts_raw, data->base + I2C_IRQSTATUS_RAW);
}
 
int i2c_wait_ARDY(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & (I2C_IRQSTATUS_RAW_ARDY)) != I2C_IRQSTATUS_RAW_ARDY) // Wait for RRDY (receive data ready)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout ARDY-I, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
int i2c_wait_ARDY_IRQ(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while (!data->ardy_check) // Wait for RRDY (receive data ready)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout ARDY_IRQ, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
void i2c_clr_ARDY(struct i2c_device_data *data)
{
    u32 i2c_sts_raw = 0;
 
    i2c_sts_raw = ioread32(data->base + I2C_IRQSTATUS_RAW);
    i2c_sts_raw |= I2C_IRQSTATUS_RAW_ARDY; // Clear ARDY
    iowrite32(i2c_sts_raw, data->base + I2C_IRQSTATUS_RAW);
}
 
void i2c_clr_ARDY_IRQ(struct i2c_device_data *data)
{
    data->ardy_check = 0;
}
 
int i2c_wait_ACK(struct i2c_device_data *data)
{
    unsigned long timeout;
    u32 i2c_con = 0;
 
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while ((ioread32(data->base + I2C_IRQSTATUS_RAW) & (I2C_IRQSTATUS_RAW_NACK)) == I2C_IRQSTATUS_RAW_NACK) // Wait for ACK (addr)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout wait ACK1-I, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
    return 0;
}
 
void i2c_clr_NACK(struct i2c_device_data *data)
{
    u32 i2c_sts_raw = 0;
 
    i2c_sts_raw = ioread32(data->base + I2C_IRQSTATUS_RAW);
    i2c_sts_raw |= I2C_IRQSTATUS_RAW_NACK; // Clear NACK
    iowrite32(i2c_sts_raw, data->base + I2C_IRQSTATUS_RAW);
}
 
void i2c_clr_ALL(struct i2c_device_data *data)
{
    u32 i2c_sts_raw = 0;
 
    i2c_sts_raw = ioread32(data->base + I2C_IRQSTATUS_RAW);
    i2c_sts_raw = 0xFF; // Clear NACK
    iowrite32(i2c_sts_raw, data->base + I2C_IRQSTATUS_RAW);
}
 
// Write data (internal register) to slave
int i2c1_write(struct i2c_device_data *data, u8 slave_addr, u8 register_addr, u8 *tx, size_t len)
{
    u32 i;
    int ret;
 
    if (slave_addr <= 0) return -1;
 
    // ===================== Master sends START. ===========================
    ret = i2c_wait_BB(data);
    if (ret < 0)
    {
        return ret;
    }
 
    // wait until BB is free (STOP condition already happened), re-init master mode else, STOP will clear Master mode bit
    i2c_reinit_master_transmit(data);
    iowrite32(slave_addr, data->base + I2C_SA); // Set slave address
    iowrite32(len + 1, data->base + I2C_CNT);   // Number of bytes to write (number of bytes + 1 byte (addr))
 
    // ===================== MMaster sends [slave address + write bit]. ===========================
    // Start I2C
    i2c_start(data);
 
    // wait ACK from slave
    ret = i2c_wait_ACK(data);
    if (ret < 0)
    {
        return ret;
    }
 
    // ===================== MMaster sends internal register address. ===========================
    if (register_addr != 0xFF)
    {
        ret = i2c_wait_XRDY(data);
        if (ret < 0)
        {
            return ret;
        }
 
        iowrite32(register_addr, data->base + I2C_DATA); // Write data
 
        i2c_clr_XRDY(data);
 
        // wait ACK from slave
        ret = i2c_wait_ACK(data);
        if (ret < 0)
        {
            return ret;
        }
    }
 
    // ===================== Master sends data. ===========================
    for (i = 0; i < len; i++)
    {
        ret = i2c_wait_XRDY(data);
        if (ret < 0)
        {
            return ret;
        }
 
        iowrite32(tx[i], data->base + I2C_DATA); // Write data
 
        i2c_clr_XRDY(data);
 
        // wait ACK from slave
        ret = i2c_wait_ACK(data);
        if (ret < 0)
        {
            return ret;
        }
    }
 
    // printk("w cnt = %d\n", ioread32(data->base + I2C_CNT));
 
    // ret = i2c_wait_ARDY(data);
    // if (ret < 0){
    //     return ret;
    // }
 
    // i2c_clr_ARDY(data);
 
    ret = i2c_wait_ARDY_IRQ(data);
    if (ret < 0)
    {
        return ret;
    }
    i2c_clr_ARDY_IRQ(data);
 
    // Stop i2c
    i2c_stop(data);
 
    // udelay(MS_READ_W);
 
    return 0;
}
 
// Read data from slave
int i2c1_read(struct i2c_device_data *data, u8 slave_addr, u8 register_addr, u8 *rx, size_t len)
{
    int ret;
    unsigned long timeout;
    u32 i2c_con = 0;
 
    if (slave_addr <= 0) return -1;
 
    data->rx = rx; // assign pointer to user data
 
    // ===================== Master sends START. ===========================
    ret = i2c_wait_BB(data);
    if (ret < 0)
    {
        return ret;
    }
 
    i2c_reinit_master_transmit(data);
    iowrite32(slave_addr, data->base + I2C_SA); // Set slave address
    iowrite32(1, data->base + I2C_CNT);         // Number of bytes to write (here 1 byte for register address )
 
    // ===================== MMaster sends [slave address + write bit]. ===========================
    i2c_start(data);
 
    // wait ACK from slave
    ret = i2c_wait_ACK(data);
    if (ret < 0)
    {
        return ret;
    }
 
    // ===================== Master sends register address (tells slave which register to read). ===========================
    ret = i2c_wait_XRDY(data);
    if (ret < 0)
    {
        return ret;
    }
    i2c_clr_XRDY(data); // clear XRDY
 
    iowrite32(register_addr, data->base + I2C_DATA); // Write internal register address
 
    ret = i2c_wait_ARDY_IRQ(data);
    if (ret < 0)
    {
        return ret;
    }
    i2c_clr_ARDY_IRQ(data);
 
    // wait ACK from slave
    ret = i2c_wait_ACK(data);
    if (ret < 0)
    {
        return ret;
    }
 
    ret = i2c_wait_ARDY_IRQ(data);
    if (ret < 0)
    {
        return ret;
    }
    i2c_clr_ARDY_IRQ(data);

    // ===================== Master sends REPEATED START. ===========================
    // do not need to check BB as we are in transfer (BB must be 1 to generate restart condition)
    iowrite32(len, data->base + I2C_CNT); // Number of bytes to write
    i2c_reinit_master_receive(data);
 
    // Bus should be occupied to generate restart
    // ===================== Master sends [slave address + read bit].. ===========================
    // Start i2c
    i2c_start(data);
 
    // wait ACK from slave
    ret = i2c_wait_ACK(data);
    if (ret < 0)
    {
        return ret;
    }
 
    // Read data from DS3231
    // ===================== Master reads data from slave. ===========================
 
    //data->data_count = 0; // reset index
    timeout = jiffies + msecs_to_jiffies(DELAY_MS);
    while (!data->byte)
    {
        if (time_after(jiffies, timeout))
        {
            dev_err(data->dev, "Timeout wait data->byte, irqsts_raw = 0x%x\n", ioread32(data->base + I2C_IRQSTATUS_RAW));
 
            i2c_con = ioread32(data->base + I2C_CON);
            i2c_con &= ~0x1;
            i2c_con |= 0x2; // Stop condition
            iowrite32(i2c_con, data->base + I2C_CON);
 
            return -ETIMEDOUT;
        }
        cpu_relax();
    }
 
    data->byte = 0;
    //data->data_count = 0; // reset index
 
    ret = i2c_wait_ARDY_IRQ(data);
    if (ret < 0)
    {
        return ret;
    }
    i2c_clr_ARDY_IRQ(data);
 
    // Stop i2c
    i2c_stop(data);
 
    return 0;
}
 
/* File operations funcs */
static int i2c1_master_open(struct inode *inode, struct file *file)
{
    struct i2c_device_data *data = container_of(inode->i_cdev, struct i2c_device_data, cdev);
    file->private_data = data;
    return 0;
}
 
static int i2c1_master_release(struct inode *inode, struct file *filp)
{
    struct i2c_device_data *i2c = container_of(inode->i_cdev, struct i2c_device_data, cdev);
    filp->private_data = i2c;

    i2c1_master_deinit(i2c);

    return 0;
}

static ssize_t i2c1_master_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct i2c_device_data *data = filp->private_data;
    char *kbuf;
 
    int ret = 0;
 
    if (!data)
    {
        pr_err("NULL data\n");
        return -EFAULT;
    }
 
    /* Create kernel buffer */
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
 
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    printk("W -> [0x%x][0x%x], kbuf = %d\n", data->slave_addr, data->reg_addr, kbuf[0]);
 
    ret = i2c1_write(data, data->slave_addr, data->reg_addr, (u8*)&kbuf[0], 1);
    if (ret < 0)
    {
        dev_info(data->dev, "Send error\n");
        return -1;
    }
 
    return count;
}
 
static ssize_t i2c1_master_read(struct file *filp, char __user *buf, size_t len, loff_t *f_pos){
    struct i2c_device_data *data = filp->private_data;
    unsigned long flags;
    int ret;
 
    /* read data */
    ret = i2c1_read(data, data->slave_addr, data->reg_addr, &data->rx_buf[0], 1);
    if (ret < 0) return -EFAULT;
 
    /* wait data recieved done */
    ret = wait_event_interruptible_timeout(data->recv_wait, data->data_ready,
                                           msecs_to_jiffies(3000));
 
    spin_lock_irqsave(&data->lock, flags);
    data->data_ready = false;
    data->data_count = 0;
    spin_unlock_irqrestore(&data->lock, flags);
 
    if (ret == 0)
        return -ETIMEDOUT;
    if (ret < 0)
        return ret;                                 /* signal */

    printk("R -> [0x%x][0x%x], kbuf = %d\n", data->slave_addr, data->reg_addr, data->rx_buf[0]);
        
    /* copy_to_user can sleep → must be outside the spinlock */
    if (copy_to_user(buf, data->rx_buf, 1))
        return -EFAULT;
 
    return 1;
}
 
static long i2c_device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct i2c_device_data *i2c = filp->private_data;
    struct bbb_i2c_config config;
    __u8 addr, reg_addr;
    int ret = 0;

    //printk("cmd = %d\n", cmd);
 
    switch (cmd)
    {
        case BBB_I2C_SET_CONFIG:
            //printk("cmd1 = %d\n", BBB_I2C_SET_CONFIG);
            if (copy_from_user(&config,
                            (struct bbb_i2c_config __user *)arg,
                            sizeof(config)))
            {
                return -EFAULT;
            }
 
            /* Validate the configuration. */
            if (config.speed < 0 || config.speed > 400) // in KHz
            {
                return -EINVAL;
            }
 
            i2c->data_ready = false;
            i2c->done_configured = false;
 
            /*
            * Apply the configuration to the I2C hardware.
            */
            i2c1_master_init(i2c, &config);
            i2c->done_configured = true;
 
            return ret;
 
        case BBB_I2C_SET_SLAVE_ADDR:
            if (copy_from_user(&addr,
                            (__u8 __user *)arg,
                            sizeof(addr)))
            {
                return -EFAULT;
            }
            i2c->slave_addr = addr;
            break;
 
        case BBB_I2C_SET_REG_ADDR:
            if (copy_from_user(&reg_addr,
                            (__u8 __user *)arg,
                            sizeof(reg_addr)))
            {
                return -EFAULT;
            }
            i2c->reg_addr = reg_addr;

            //printk("cmd3 = i2c->reg_addr = %d\n", i2c->reg_addr);

            break;
            
 
    default:
        return -ENOTTY;
    }

    return ret;
}
 
/* File operations */
static const struct file_operations i2c_device_fops = {
    .owner =            THIS_MODULE,
    .open =             i2c1_master_open,
    .release =          i2c1_master_release,
    .write =            i2c1_master_write,
    .read =             i2c1_master_read,
    .unlocked_ioctl =   i2c_device_ioctl,
};
 
void GPIO_init(struct i2c_device_data *data)
{
    iowrite32(0x32, data->base_gpio + 0x95c); // i2c1_scl
    iowrite32(0x32, data->base_gpio + 0x958); // i2c1_sda
}
 
static int i2c1_probe(struct platform_device *pdev)
{
    struct i2c_device_data *data;
    int ret;
 
    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
 
    platform_set_drvdata(pdev, data);
 
    data->base = ioremap(I2C1_BASE, 0x1000);
    data->base_gpio = ioremap(GPIO_BASE, 0x1000);
    data->dev = &pdev->dev;
    data->byte = 0;
    data->byte_tx = 0;
    data->ardy_check = 0;
    spin_lock_init(&data->lock);       // Initialize spinlock
 
    /* Clock setup (assuming this part is unchanged) */
    data->clk = devm_clk_get(&pdev->dev, "fck-i2c1");
    if (IS_ERR(data->clk))
    {
        dev_err(&pdev->dev, "Failed to get clock: %ld\n", PTR_ERR(data->clk));
        return PTR_ERR(data->clk);
    }
    ret = clk_prepare_enable(data->clk);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to enable clock: %d\n", ret);
        return ret;
    }
    dev_info(&pdev->dev, "I2c1 clock rate: %lu Hz\n", clk_get_rate(data->clk));
 
    GPIO_init(data);
    init_waitqueue_head(&data->recv_wait);
    data->data_ready = false;
    data->done_configured = false;
 
    // ========== Request IRQ (hwirq 30 maps to swirq x on AM33xx) ==========
    data->irq = platform_get_irq(pdev, 0);
    if (data->irq < 0)
    {
        dev_err(&pdev->dev, "Failed Formatted: Unable to get IRQ: %d\n", data->irq);
        return data->irq;
    }
    ret = devm_request_irq(&pdev->dev, data->irq, irqHandler, 0, "i2c1", data);
    if (ret < 0)
    {
        dev_err(&pdev->dev, "Unable to request IRQ %d: %d\n", data->irq, ret);
        return ret;
    }
 
    dev_info(&pdev->dev, "IRQ num = %d\n", data->irq);
 
    // Create character device
    ret = alloc_chrdev_region(&data->dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0)
    {
        dev_err(&pdev->dev, "Failed to allocate chrdev region: %d\n", ret);
        return ret;
    }
 
    cdev_init(&data->cdev, &i2c_device_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->dev_num, 1);
    if (ret < 0)
    {
        dev_err(&pdev->dev, "Failed to add cdev: %d\n", ret);
        // unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base);
        return ret;
    }
 
    data->class = class_create(THIS_MODULE, "i2c1_class");
    if (IS_ERR(data->class))
    {
        dev_err(&pdev->dev, "Failed to create class: %ld\n", PTR_ERR(data->class));
        cdev_del(&data->cdev);
        // unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base);
        return PTR_ERR(data->class);
    }
 
    data->char_dev = device_create(data->class, &pdev->dev, data->dev_num, NULL, "i2c1");
    if (IS_ERR(data->char_dev))
    {
        dev_err(&pdev->dev, "Failed to create device: %ld\n", PTR_ERR(data->char_dev));
        class_destroy(data->class);
        cdev_del(&data->cdev);
        // unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base);
        return PTR_ERR(data->char_dev);
    }
 
    dev_info(&pdev->dev, "Created /dev/%s\n", DEVICE_NAME);
 
    return 0;
}
 
static int i2c1_remove(struct platform_device *pdev)
{
    struct i2c_device_data *data = platform_get_drvdata(pdev);
 
    dev_info(data->dev, "Removed\n");
 
    if (data->dev)
        device_destroy(data->class, data->dev_num);
    if (data->class)
        class_destroy(data->class);
    cdev_del(&data->cdev);
 
    return 0;
}
 
static const struct of_device_id i2c_device_of_match[] = {
    {.compatible = "i2c1-based"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, i2c_device_of_match);
 
static struct platform_driver i2c_device_driver = {
    .probe = i2c1_probe,
    .remove = i2c1_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = i2c_device_of_match,
    },
};
 
module_platform_driver(i2c_device_driver);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Custom I2C Device Driver for BeagleBone Black I2C1");