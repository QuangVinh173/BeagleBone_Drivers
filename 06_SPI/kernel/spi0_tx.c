#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include "main.h"
#include "spi_ioctl.h"

#define DRIVER_NAME "spi0_device_driver"
#define DEVICE_NAME "spi0"
#define SLK 500000

#define SPI0_BASE 0x48030000

#define MCSPI_CHCONF0   0x12c
#define MCSPI_SYSCONFIG   0x110
#define MCSPI_SYSSTATUS 0x114
#define MCSPI_CHSTAT0   0x130
#define MCSPI_CHCTRL0   0x134
#define MCSPI_TX0       0x138
#define MCSPI_RX0       0x13c
#define MCSPI_MODULCTRL 0x128
#define MCSPI_IRQSTATUS 0x118
#define MCSPI_IRQENABLE 0x11c
#define MCSPI_XFERLEVEL 0x17c

#define MCSPI_CHCONF_POL    BIT(1)  // Clock polarity
#define MCSPI_CHCONF_PHA    BIT(0)  // Clock phase
#define MCSPI_CHCONF_WL_MASK (0x1f << 7)  // Word length mask
#define MCSPI_CHCONF_CLKD_MASK (0x0f << 2)  // Word length mask
#define MCSPI_CHCONF_DPE0   BIT(16) // Data pin 0 enable (MOSI)
#define MCSPI_CHCONF_DPE1   BIT(17) // Data pin 1 enable
#define MCSPI_CHCONF_IS     BIT(18) // Input select (MISO)
#define MCSPI_CHCONF_FORCE  BIT(20) // Force SPI_EN (CS)
#define MCSPI_CHCONF_EPOL     BIT(6)

#define MCSPI_CHSTAT_EOT    BIT(2)  // end of frame
#define MCSPI_CHSTAT_TXS    BIT(1)  // TX register empty
#define MCSPI_CHSTAT_RXS    BIT(0)  // RX register full

#define MCSPI_CHCTRL_EN     BIT(0)  // Channel enable

#define MCSPI_MODULCTRL_SINGLE BIT(0) // Single-channel mode

#define MCSPI_CHSTAT_RX0_FULL BIT(2) // RX0 full interrupt (same as CHSTAT0 RXS)
#define MCSPI_CHSTAT_TX0_EMPTY BIT(0)

#define MAX_IRQ_JUMPS 300
#define MS_DELAY 1000
#define FIFO_USED 1

/*
NOTE:
+ for transmitting only -> choose transmit-only mode
+ active low = when CS becomes 0, data is sent
*/

struct spi_device_data {
    dev_t dev_num;
    struct cdev cdev;
    struct class *class;
    struct device *dev;
    struct device *char_dev;

    void __iomem *base;  // Mapped base address of SPI0 registers
    struct clk *clk;
    struct clk *edma_clk;

    int irq;

    u8 TX[256];

    char* tx_buf;
    size_t count;
    size_t count_check;

    /* DMA-related fields */
    struct dma_chan *tx_chan;
    struct completion tx_completion;
    bool use_dma; /* Flag to indicate if DMA is used */
    bool DMA_IRQ;

    u8* src1;

    struct bbb_spi_config cfg;
    bool done_configured;
};

/* SPI hw init-deinit */
void spi_hw_init(struct spi_device_data* data, struct bbb_spi_config* config){
    u32 chconf;
    iowrite32(1<<1, data->base + MCSPI_SYSCONFIG);
    while(!(ioread32(data->base + MCSPI_SYSSTATUS)));

    iowrite32(MCSPI_MODULCTRL_SINGLE, data->base + MCSPI_MODULCTRL);

    /* deselect the slave immediately (CS idle high) */
    chconf = MCSPI_CHCONF_EPOL;
    iowrite32(chconf, data->base + MCSPI_CHCONF0);

    chconf = ioread32(data->base + MCSPI_CHCONF0);
    chconf |= (8 - 1) << 7; // 8-bit word length

    switch (config->mode){
        case SPI_MODE_0: /* CPOL = 0, CPHA = 0 */
            chconf &= ~MCSPI_CHCONF_POL;
            chconf &= ~MCSPI_CHCONF_PHA;
            break;

        case SPI_MODE_1: /* CPOL = 0, CPHA = 1 */
            chconf &= ~MCSPI_CHCONF_POL;
            chconf |= MCSPI_CHCONF_PHA;
            break;

        case SPI_MODE_2: /* CPOL = 1, CPHA = 0 */
            chconf |= MCSPI_CHCONF_POL;
            chconf &= ~MCSPI_CHCONF_PHA;
            break;

        case SPI_MODE_3: /* CPOL = 1, CPHA = 1 */
            chconf |= MCSPI_CHCONF_POL;
            chconf |= MCSPI_CHCONF_PHA;
            break;
    }

    chconf &= ~MCSPI_CHCONF_IS; // D0 as input (MISO)
    chconf |= MCSPI_CHCONF_DPE0;

    chconf &= ~MCSPI_CHCONF_DPE1; // D1 as output (MOSI)

    chconf &= ~(1<<29); // Clock granularity of power of 2
    /* set clock divisor */
    chconf |= (config->clk_divided) << 2;

    chconf |= MCSPI_CHCONF_EPOL; // low EN - EPOL = 1, high EN - EPOL = 0

    chconf |= 2 << 12; // transmit-mode only

    chconf |= FIFO_USED << 27; // The buffer is used to transmit data.

    iowrite32(chconf, data->base + MCSPI_CHCONF0);

    iowrite32(0, data->base + MCSPI_XFERLEVEL); // interrupt at least 1 byte

    iowrite32(1, data->base + MCSPI_CHCTRL0);

    dev_info(data->dev, "ch0ststus = 0x%x\n", ioread32(data->base + MCSPI_CHSTAT0));
}

void spi_hw_deinit(struct spi_device_data* data){
    u32 chconf = ioread32(data->base + MCSPI_CHCONF0);

    chconf &= ~(MCSPI_CHCONF_FORCE | BIT(14) | BIT(15));   /* CS released, no DMA req */
    iowrite32(chconf, data->base + MCSPI_CHCONF0);         /* EPOL/POL/PHA unchanged */

    iowrite32(0, data->base + MCSPI_CHCTRL0);

    iowrite32(1<<1, data->base + MCSPI_SYSCONFIG);
}

/* IRQ handler */
static irqreturn_t irqHandler(int irq, void *d)
{
    struct spi_device_data *data = d;
    u32 irqsts;

    irqsts = ioread32(data->base + MCSPI_IRQSTATUS);
    printk("===> %d\n", irqsts);

    if ((irqsts & MCSPI_CHSTAT_TX0_EMPTY) == MCSPI_CHSTAT_TX0_EMPTY) {

        printk("***[CPU-IRQ] Write is issued***\n");

        iowrite32(MCSPI_CHSTAT_TX0_EMPTY, data->base + MCSPI_IRQSTATUS);
    }

    data->count_check++;
    if (data->count > MAX_IRQ_JUMPS){
        dev_info(data->dev, "Too many interrupts\n");
        iowrite32(0, data->base + MCSPI_IRQENABLE);
        data->count_check = 0;
    }

    return IRQ_HANDLED;
}

/* DMA seting */
static int spi0_configure_dma(struct spi_device_data *data)
{
    struct dma_slave_config tx_conf = {0};
    int ret;

    if (data->tx_chan) {
        tx_conf.direction = DMA_MEM_TO_DEV;
        tx_conf.dst_addr = (dma_addr_t)(SPI0_BASE + MCSPI_TX0);
        tx_conf.dst_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE; /* SPI0 uses 8-bit transfers */
        tx_conf.dst_maxburst = 1; /* Single byte per transfer */
        ret = dmaengine_slave_config(data->tx_chan, &tx_conf);
        if (ret) {
            dev_err(data->dev, "Failed to configure TX DMA channel: %d\n", ret);
            return ret;
        }
        dev_info(data->dev, "TX DMA channel configured\n");
    }

    return 0;
}

int DMA_init(struct spi_device_data *data){
    struct clk *edma_clk;
    int ret;
    /* =============== DMA ================= */
    edma_clk = devm_clk_get(data->dev, "fck-dma");
    if (IS_ERR(edma_clk)) {
        dev_err(data->dev, "Failed to get eDMA clock: %ld\n", PTR_ERR(edma_clk));
        return PTR_ERR(edma_clk);
    }

    data->edma_clk = edma_clk;

    ret = clk_prepare_enable(edma_clk);
    if (ret) {
        dev_err(data->dev, "Failed to enable eDMA clock: %d\n", ret);
        return ret;
    }
    dev_info(data->dev, "Edma clock rate: %lu Hz\n", clk_get_rate(edma_clk));

    iowrite32(0, data->base + MCSPI_IRQENABLE); // disable TX interrupt

    /* Request DMA channels */
    data->tx_chan = dma_request_chan(data->dev, "tx1");
    if (IS_ERR(data->tx_chan)) {
        dev_info(data->dev, "No TX DMA channel, falling back to non-DMA mode: %ld\n",
                PTR_ERR(data->tx_chan));
        data->tx_chan = NULL;
        return -1;
    }

    /* COnfig parameters for DMA */
    data->use_dma = (data->tx_chan);
    if (data->use_dma){
        dev_info(data->dev, "Using DMA for UART transfers\n");
        ret = spi0_configure_dma(data);
        if (ret) {
            dev_info(data->dev, "Failed to configure DMA, falling back to non-DMA mode\n");
            if (data->tx_chan)
                dma_release_channel(data->tx_chan);
            data->tx_chan = NULL;
            data->use_dma = false;
        }
        else {
            dev_info(data->dev, "Succeed to configure DMA\n");
        }
    }
    else
        dev_info(data->dev, "Using interrupt-driven transfers\n");

    init_completion(&data->tx_completion);

    return ret;
}

void DMA_deinit(struct spi_device_data *data){
    /* =============== DMA ================= */
    if (data->tx_chan){
        dma_release_channel(data->tx_chan);
        data->tx_chan = NULL;
        data->use_dma = false;
    }

}

/* DMA operation */
static void spi0_tx_dma_callback(void *data)
{
    struct spi_device_data *spi0 = data;
    complete(&spi0->tx_completion);
    printk("***[DMA-IRQ] Write is issued***\n");
    spi0->DMA_IRQ = 1;
}

void Non_dma_write(struct spi_device_data* data){
    int i;
    unsigned long timeout;
    u32 ch0cfg;
#if (PRINT_DEBUG_TX)
    dev_info(data->dev, "Non-DMA transmitting, count = %d\n", data->count);
#endif

    iowrite32(MCSPI_CHSTAT_TX0_EMPTY, data->base + MCSPI_IRQENABLE);

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg |= MCSPI_CHCONF_FORCE;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);
    timeout = jiffies + msecs_to_jiffies(3000);

    for (i = 0; i < data->count + 1; i++) {
        while ((ioread32(data->base + MCSPI_CHSTAT0) & MCSPI_CHSTAT_TXS) != MCSPI_CHSTAT_TXS) {
            if (time_after(jiffies, timeout)) {
                dev_err(data->dev, "Timeout writing at byte %d, chstst0 = 0x%x\n", i, ioread32(data->base + MCSPI_CHSTAT0));

                ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
                 ch0cfg &=~ MCSPI_CHCONF_EPOL;
                iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);
                iowrite32(0, data->base + MCSPI_CHCTRL0);
            }
            cpu_relax();
        }

        udelay(100); // need a small break to after interrupt handler

        if (i!= 0) iowrite32(data->tx_buf[i - 1], data->base + MCSPI_TX0);
        else iowrite32(data->count, data->base + MCSPI_TX0);

    }
    iowrite32(0, data->base + MCSPI_IRQENABLE); // disable TX interrupt

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg &=~ MCSPI_CHCONF_FORCE;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

    data->count_check = 0;
}

void Dma_write(struct spi_device_data* data){
    dma_addr_t dma_addr;
    struct dma_async_tx_descriptor *tx_desc;
    int ret;
    int count = data->count;
    u32 ch0cfg;

#if (PRINT_DEBUG_TX)
    dev_info(data->dev, "DMA transmitting\n");
#endif

    /* Allocate DMA-coherent buffer */
    data->src1 = dma_alloc_coherent(data->dev, count + 1, &dma_addr, GFP_KERNEL | GFP_DMA);
    if (!data->src1) {
        dev_err(data->dev, "Failed to allocate DMA-coherent buffer\n");
        return;
    }

    data->src1[0] = count;
    memcpy(data->src1 + 1, data->tx_buf, count);

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg |= MCSPI_CHCONF_FORCE;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

    reinit_completion(&data->tx_completion);
    tx_desc = dmaengine_prep_slave_single(data->tx_chan, dma_addr, count + 1,
                                            DMA_MEM_TO_DEV, DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
    if (!tx_desc) {
        dev_err(data->dev, "Failed to prepare TX DMA descriptor\n");
        goto free_buf;
    }

    tx_desc->callback = spi0_tx_dma_callback;
    tx_desc->callback_param = data;

    dmaengine_submit(tx_desc);

    dma_async_issue_pending(data->tx_chan);

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg |= 1 << 14; // DMA write request
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

    ret = wait_for_completion_timeout(&data->tx_completion, msecs_to_jiffies(1000));
    if (ret == 0) {
        dev_err(data->dev, "DMA TX timeout\n");
        dmaengine_terminate_sync(data->tx_chan);
        goto free_buf;
    }

    udelay(500); // wait 50 us to let DMA transfer is done

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg &=~ MCSPI_CHCONF_FORCE;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

free_buf:
    dma_unmap_single(data->dev, dma_addr, count, DMA_TO_DEVICE);

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg &=~ MCSPI_CHCONF_FORCE;
    ch0cfg &= ~(1 << 14); // DMA write request cleared
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

}

/* File operations */
static int spi_device_open(struct inode *inode, struct file *file)
{
    struct spi_device_data *data = container_of(inode->i_cdev, struct spi_device_data, cdev);
    file->private_data = data;
    return 0;
}

static int spi_device_release(struct inode *inode, struct file *filp)
{
    struct spi_device_data *spi = container_of(inode->i_cdev, struct spi_device_data, cdev);
    filp->private_data = spi;

    spi_hw_deinit(spi);

    if (spi->cfg.use_dma){
        DMA_deinit(spi);
    }
    return 0;
}

static ssize_t spi_device_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct spi_device_data *data = filp->private_data;
    char *kbuf;

    if (!data->done_configured){
        return -EFAULT;
    }

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    data->DMA_IRQ = 0;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    data->tx_buf = kbuf;
    data->count = count;

    if (data->cfg.use_dma)
        Dma_write(data);
    else
        Non_dma_write(data);

#if (PRINT_DEBUG_TX)
    dev_info(data->dev, "Done writing %zu bytes: %*ph\n", count, (int)count, data->tx_buf);
#endif
    return count;
}

static ssize_t spi_device_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    struct spi_device_data *data = filp->private_data;

    dev_info(data->dev, "count = %d\n", data->count);

    if (data->DMA_IRQ) dev_info(data->dev, "TX DMA callback called\n");

    data->DMA_IRQ = 0;

    return 0;
}

static long spi_device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct spi_device_data *spi = filp->private_data;
    struct bbb_spi_config config;
    int ret = 0;

    switch (cmd) {
    case BBB_SPI_SET_CONFIG:
        if (copy_from_user(&config,
                           (struct bbb_spi_config __user *)arg,
                           sizeof(config))) {
            return -EFAULT;
        }

        /* Validate the configuration. */
        if (config.clk_divided < 0x00 || config.clk_divided > 0x0F) {
            return -EINVAL;
        }

        pr_info("BBB SPI: clk_divided=%u\n",
                config.clk_divided);

        memset(spi->TX, 0, sizeof(spi->TX));
        spi->tx_buf = NULL;
        spi->count = 0;
        spi->count_check = 0;
        spi->use_dma = 0;
        spi->DMA_IRQ = 0;
        memset(&spi->cfg, 0, sizeof(spi->cfg));
        spi->done_configured = 0;

        /*
         * Apply the configuration to the SPI hardware.
         */
        spi_hw_init(spi, &config);

        spi->cfg.use_dma = config.use_dma;
        if (spi->cfg.use_dma){
            ret = DMA_init(spi);
        }

        spi->done_configured = true;

        return ret;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations spi_device_fops = {
    .owner =            THIS_MODULE,
    .open =             spi_device_open,
    .release =          spi_device_release,
    .write =            spi_device_write,
    .read =             spi_device_read,
    .unlocked_ioctl =   spi_device_ioctl,
};

/* Probe - unprobe */
static int spi_device_probe(struct platform_device *pdev)
{
    struct spi_device_data *data;
    int ret;

    dev_info(&pdev->dev, "SPI device probed successfully with compatible 'spi0-based'\n");

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    platform_set_drvdata(pdev, data);

    data->count = 0;

    data->base = ioremap(SPI0_BASE, 0x10000);
    data->dev = &pdev->dev;
    data->DMA_IRQ = 0;

    /* Clock setup (assuming this part is unchanged) */
    data->clk = devm_clk_get(&pdev->dev, "fck-spi0");
    if (IS_ERR(data->clk)) {
        dev_err(&pdev->dev, "Failed to get clock: %ld\n", PTR_ERR(data->clk));
        return PTR_ERR(data->clk);
    }
    ret = clk_prepare_enable(data->clk);
    if (ret) {
        dev_err(&pdev->dev, "Failed to enable clock: %d\n", ret);
        return ret;
    }
    dev_info(&pdev->dev, "SPI0 clock rate: %lu Hz\n", clk_get_rate(data->clk));

    data->done_configured = false;

    data->irq = platform_get_irq(pdev, 0);
    if (data->irq < 0) {
        dev_err(&pdev->dev, "Failed Formatted: Unable to get IRQ: %d\n", data->irq);
        return data->irq;
    }
    ret = devm_request_irq(&pdev->dev, data->irq, irqHandler, 0, "SPI0", data);
    if (ret < 0) {
        dev_err(&pdev->dev, "Unable to request IRQ %d: %d\n", data->irq, ret);
        return ret;
    }

    ret = alloc_chrdev_region(&data->dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to allocate chrdev region: %d\n", ret);
        return ret;
    }

    cdev_init(&data->cdev, &spi_device_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->dev_num, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to add cdev: %d\n", ret);
        iounmap(data->base);
        return ret;
    }

    data->class = class_create(THIS_MODULE, "spi0_class");
    if (IS_ERR(data->class)) {
        dev_err(&pdev->dev, "Failed to create class: %ld\n", PTR_ERR(data->class));
        cdev_del(&data->cdev);
        iounmap(data->base);
        return PTR_ERR(data->class);
    }

    data->char_dev = device_create(data->class, &pdev->dev, data->dev_num, NULL, "spi0");
    if (IS_ERR(data->char_dev)) {
        dev_err(&pdev->dev, "Failed to create device: %ld\n", PTR_ERR(data->char_dev));
        class_destroy(data->class);
        cdev_del(&data->cdev);
        iounmap(data->base);
        return PTR_ERR(data->char_dev);
    }

    dev_info(&pdev->dev, "Created /dev/%s\n", DEVICE_NAME);

    return 0;
}

static int spi_device_remove(struct platform_device *pdev)
{
    struct spi_device_data *data = platform_get_drvdata(pdev);

    if (data->tx_chan)
        dma_release_channel(data->tx_chan);

    if (data->dev)
        device_destroy(data->class, data->dev_num);
    if (data->class)
        class_destroy(data->class);

    cdev_del(&data->cdev);
    unregister_chrdev_region(data->dev_num, 1);
    dev_info(&pdev->dev, "SPI device removed\n");
    return 0;
}

static const struct of_device_id spi_device_of_match[] = {
    { .compatible = "spi0-based" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spi_device_of_match);

static struct platform_driver spi_device_driver = {
    .probe = spi_device_probe,
    .remove = spi_device_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = spi_device_of_match,
    },
};

module_platform_driver(spi_device_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Custom SPI Device Driver for BeagleBone Black SPI0");
