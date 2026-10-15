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
#include <linux/wait.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include "main.h"
#include "spi_ioctl.h"

#define DRIVER_NAME "spi1_device_driver"
#define DEVICE_NAME "spi1"
#define SLK 500000

#define SPI1_BASE 0x481a0000

#define MCSPI_SYSCONFIG   0x110
#define MCSPI_SYSSTATUS   0x114

#define MCSPI_CHCONF0     0x12c
#define MCSPI_CHSTAT0     0x130
#define MCSPI_CHCTRL0     0x134
#define MCSPI_TX0         0x138
#define MCSPI_RX0         0x13c

#define MCSPI_MODULCTRL   0x128
#define MCSPI_IRQSTATUS   0x118
#define MCSPI_IRQENABLE   0x11c

#define MCSPI_DAFRX       0x1a0
#define MCSPI_XFERLEVEL   0x17c
#define RX_TRIGGER 10

#define MCSPI_CHCONF_POL      BIT(1)  // Clock polarity
#define MCSPI_CHCONF_PHA      BIT(0)  // Clock phase
#define MCSPI_CHCONF_WL_MASK  (0x1f << 7)  // Word length mask
#define MCSPI_CHCONF_TRM_RX_ONLY BIT(12)  // Receive-only mode
#define MCSPI_CHCONF_DPE0     BIT(16) // Data pin 0 enable
#define MCSPI_CHCONF_DPE1     BIT(17) // Data pin 1 enable
#define MCSPI_CHCONF_IS       BIT(18) // Input select (MISO)
#define MCSPI_CHCONF_FORCE    BIT(20) // Force SPI_EN (CS)
#define MCSPI_CHCONF_EPOL     BIT(6)  // Chip select polarity

#define MCSPI_CHSTAT_EOT      BIT(2)  // End of frame
#define MCSPI_CHSTAT_TXS      BIT(1)  // TX register empty
#define MCSPI_CHSTAT_RXS      BIT(0)  // RX register full

#define MCSPI_CHCTRL_EN       BIT(0)  // Channel enable

#define MCSPI_MODULCTRL_MS    BIT(2)  // Master/Slave mode

#define MCSPI_CHSTAT_RX1_FULL BIT(6)  // RX1 full interrupt
#define MCSPI_CHSTAT_RX0_FULL BIT(2)  // RX0 full interrupt
#define MCSPI_CHSTAT_RX0_OVERFLOW BIT(3)  // RX0 overflow interrupt
#define MCSPI_CHSTAT_EOWKE BIT(17)

#define MCSPI_CHSTAT_RXFFE   BIT(5)          /* RX FIFO empty */
#define MCSPI_IRQ_RX0_FULL   BIT(2)
#define MCSPI_IRQ_RX0_OVF    BIT(3)
#define RX_IRQ_MASK          (MCSPI_IRQ_RX0_FULL | MCSPI_IRQ_RX0_OVF)
#define RX_DRAIN_MAX         64              /* max FIFO depth on AM335x, safety bound */

#define SPI_RECV_INTERVAL (2 * HZ)
#define MS_DELAY 3000 // increarw the time from 2000 to 3000 to make sure the completion of DMA received data
#define FIFO_USED 1

struct spi_device_data {
    dev_t dev_num;
    struct cdev cdev;
    struct class *class;
    struct device *dev;
    struct device *char_dev;
    void __iomem *base;  // Mapped base address of SPI1 registers
    struct clk *clk;
    int irq;
    struct task_struct *recv_thread;
    wait_queue_head_t recv_wait;
    u8 rx_buffer[256];  // Buffer for received data
    bool data_ready;    // Flag for new data

    u8 index;
    u8 is_get_count;

    /* DMA-related fields */
    struct dma_chan *rx_chan;
    struct completion rx_completion;
    bool use_dma; /* Flag to indicate if DMA is used */

    size_t count;

    struct work_struct re_request_work;

    struct bbb_spi_config cfg;
    bool done_configured;

    spinlock_t   lock;          /* NEW: protects rx_buffer/index/count/flags */
};

/* Declaration */
int DMA_init(struct spi_device_data *data);
void Dma_read(struct spi_device_data* data);

/* SPI hw init-deinit */
void spi_hw_init(struct spi_device_data* data, struct bbb_spi_config* config){
    u32 chconf = 0;

    iowrite32(BIT(1), data->base + MCSPI_SYSCONFIG);
    while (!(ioread32(data->base + MCSPI_SYSSTATUS) & BIT(0)))
        cpu_relax();

    iowrite32(MCSPI_MODULCTRL_MS, data->base + MCSPI_MODULCTRL);

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
    chconf |= MCSPI_CHCONF_DPE0; // D0 not driven (MISO input)
    chconf &= ~MCSPI_CHCONF_DPE1; // D1 as output (not use here)

    chconf |= MCSPI_CHCONF_EPOL; // Active-low CS
    chconf |= MCSPI_CHCONF_TRM_RX_ONLY; // Receive-only mode

    chconf |= FIFO_USED << 28; // The FIFO buffer is used to receive data.

    iowrite32(chconf, data->base + MCSPI_CHCONF0);

    iowrite32((data->cfg.FIFO_trigger - 1) << 8, data->base + MCSPI_XFERLEVEL);

    iowrite32(MCSPI_CHCTRL_EN, data->base + MCSPI_CHCTRL0);

    iowrite32(0x2444C, data->base + MCSPI_IRQENABLE);

}

void spi_hw_deinit(struct spi_device_data* data){
    iowrite32(0, data->base + MCSPI_MODULCTRL);

    iowrite32(0, data->base + MCSPI_IRQENABLE);

    iowrite32(0, data->base + MCSPI_CHCONF0); // clear cfg

    iowrite32(0, data->base + MCSPI_CHCTRL0);

    iowrite32(1<<1, data->base + MCSPI_SYSCONFIG);

}

/* IRQ handler */
static void re_request_irq_work(struct work_struct *work)
{
    struct spi_device_data *data = container_of(work, struct spi_device_data, re_request_work);
    Dma_read(data);
}

static irqreturn_t irqHandler(int irq, void *d)
{
    struct spi_device_data *data = d;
    u32 irqsts;
    int n;

    irqsts = ioread32(data->base + MCSPI_IRQSTATUS) & RX_IRQ_MASK;
    if (!irqsts)
        return IRQ_NONE;

#if (PRINT_DEBUG_RX)
    dev_warn_ratelimited(data->dev, "1. irqsts = 0x%x\n", irqsts);
#endif

    /* Clear BEFORE draining: a byte that arrives during the drain
     * sets the flag again, so a new IRQ fires and nothing is lost. */
    iowrite32(irqsts, data->base + MCSPI_IRQSTATUS);

    if (!data->done_configured)
        return IRQ_HANDLED;

    if (irqsts & MCSPI_IRQ_RX0_OVF)
        dev_warn_ratelimited(data->dev, "RX0 overflow\n");

    if (!(irqsts & MCSPI_IRQ_RX0_FULL))
        return IRQ_HANDLED;

    spin_lock(&data->lock);

    if (data->cfg.use_dma) {
        /* Length byte by CPU (completes the AFL = 1 window),
         * payload by DMA in process context. */
        data->count = ioread32(data->base + MCSPI_RX0) & 0xFF;
        iowrite32(0, data->base + MCSPI_IRQENABLE);
        spin_unlock(&data->lock);
        schedule_work(&data->re_request_work);
        return IRQ_HANDLED;
    }

    /* CPU mode: take only what is in the FIFO NOW. Never wait for more. */
    for (n = 0; n < RX_DRAIN_MAX; n++) {
        u8 b;
        u32 sta;

        b = ioread32(data->base + MCSPI_RX0) & 0xFF;

        sta = ioread32(data->base + MCSPI_CHSTAT0);

        if (!data->is_get_count) {                  /* 1st byte = length */
            data->count = b;
            data->is_get_count = 1;

        } else {
            data->rx_buffer[data->index++] = b;
        }

        if (data->is_get_count && data->index >= data->count) {
            dev_warn_ratelimited(data->dev, "Data ready\n");
            data->data_ready = true;                /* frame complete */
            wake_up_interruptible(&data->recv_wait);
        }

        if (sta & MCSPI_CHSTAT_RXFFE)
            break;
    }

    spin_unlock(&data->lock);
    return IRQ_HANDLED;
}

/* DMA seting */
static int spi1_configure_dma(struct spi_device_data *data)
{
    struct dma_slave_config rx_conf = {0};
    int ret;

    if (data->rx_chan) {
        rx_conf.direction = DMA_DEV_TO_MEM;
        rx_conf.src_addr = (dma_addr_t)(SPI1_BASE + MCSPI_RX0);
        rx_conf.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE; /* UART uses 8-bit transfers */
        rx_conf.src_maxburst = 1; /* Single byte per transfer */
        ret = dmaengine_slave_config(data->rx_chan, &rx_conf);
        if (ret) {
            dev_err(data->dev, "Failed to configure RX DMA channel: %d\n", ret);
            return ret;
        }
        dev_info(data->dev, "RX DMA channel configured\n");
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
    ret = clk_prepare_enable(edma_clk);
    if (ret) {
        dev_err(data->dev, "Failed to enable eDMA clock: %d\n", ret);
        return ret;
    }
    dev_info(data->dev, "Edma clock rate: %lu Hz\n", clk_get_rate(edma_clk));

    /* Request DMA channels */
    data->rx_chan = dma_request_chan(data->dev, "rx1");
    if (IS_ERR(data->rx_chan)) {
        dev_info(data->dev, "No RX DMA channel: %ld\n",
                 PTR_ERR(data->rx_chan));
        data->rx_chan = NULL;
    }

    /* COnfig parameters for DMA */
    data->use_dma = (data->rx_chan);
    if (data->use_dma){
        dev_info(data->dev, "Using DMA for SPI transfers\n");
        ret = spi1_configure_dma(data);
        if (ret) {
            dev_info(data->dev, "Failed to configure DMA, falling back to non-DMA mode\n");
            if (data->rx_chan)
                dma_release_channel(data->rx_chan);
            data->rx_chan = NULL;
            data->use_dma = false;
        }
        else {
            dev_info(data->dev, "Succeed to configure DMA\n");
        }
    }
    else
        dev_info(data->dev, "Using interrupt-driven transfers\n");

    init_completion(&data->rx_completion);

    return ret;
}

void DMA_deinit(struct spi_device_data *data){
    /* =============== DMA ================= */
    if (data->rx_chan){
        dma_release_channel(data->rx_chan);
        data->rx_chan = NULL;
        data->use_dma = false;
    }

}

/* DMA operation */
static void spi1_rx_dma_callback(void *data)
{
    struct spi_device_data *spi1 = data;
    complete(&spi1->rx_completion);
    dev_info(spi1->dev, "[DMA] DMA read is done -> /dev/spi1\n");
}

void Dma_read(struct spi_device_data* data){
    void *kbuf;
    dma_addr_t dma_dst_addr;
    struct dma_async_tx_descriptor *rx_desc;
    int ret;
    u32 ch0cfg;

#if (PRINT_DEBUG_RX)

    dev_info(data->dev, "Size to allocate: %d\n", data->count);
#endif
    /* Allocate DMA-coherent buffer */
    kbuf = dma_alloc_coherent(data->dev, data->count, &dma_dst_addr, GFP_KERNEL | GFP_DMA);
    if (!kbuf) {
        dev_err(data->dev, "Failed to allocate DMA-coherent buffer\n");
        return;
    }
    memset(kbuf, 0x40, data->count); // @

    reinit_completion(&data->rx_completion);
    rx_desc = dmaengine_prep_slave_single(data->rx_chan, dma_dst_addr, data->count,
                                            DMA_DEV_TO_MEM, DMA_CTRL_ACK | DMA_PREP_INTERRUPT);
    if (!rx_desc) {
        dev_err(data->dev, "Failed to prepare RX DMA descriptor\n");
        goto free_buf;
    }

    rx_desc->callback = spi1_rx_dma_callback;
    rx_desc->callback_param = data;

    dev_info(data->dev, "Submitting RX DMA descriptor\n");
    dmaengine_submit(rx_desc);

    dev_info(data->dev, "Issuing RX DMA pending\n");
    dma_async_issue_pending(data->rx_chan);

    /* Do not need to read the first byte manually here due to it's already done by reading data->count */
    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg |= 1u << 15; // DMA request read;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

    ret = wait_for_completion_timeout(&data->rx_completion, msecs_to_jiffies(MS_DELAY));
    if (ret == 0) {
        dev_err(data->dev, "DMA RX timeout\n");
        dmaengine_terminate_sync(data->rx_chan);
        goto free_buf;
    }

    goto free_buf;

free_buf:

    /* DMA get data */
    if (data->index < 256){
        memcpy(data->rx_buffer + data->index, ((char*)kbuf), data->count);
        data->index += data->count;
    }
    else {
        dev_err(data->dev, "Buffer ovreflow\n");
    }

    dma_free_coherent(data->dev, data->count, kbuf, dma_dst_addr);

    ch0cfg = ioread32(data->base + MCSPI_CHCONF0);
    ch0cfg &= ~(1u << 15); // Disable DMA request read;
    iowrite32(ch0cfg, data->base + MCSPI_CHCONF0);

    kfree(kbuf);

    data->data_ready = true;                /* frame complete */
    wake_up_interruptible(&data->recv_wait);

    iowrite32(MCSPI_CHSTAT_RX0_FULL, data->base + MCSPI_IRQSTATUS);

    iowrite32(0x2444C, data->base + MCSPI_IRQENABLE); // enable IRQ for next time

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

    spi->index = 0;

    spi_hw_deinit(spi);

    if (spi->cfg.use_dma){
        DMA_deinit(spi);
    }
    return 0;
}

static ssize_t spi_device_read(struct file *filp, char __user *buf,
                               size_t len, loff_t *f_pos)
{
    struct spi_device_data *data = filp->private_data;
    u8 tmp[sizeof(data->rx_buffer)];
    unsigned long flags;
    unsigned int n;
    long ret;

    printk("RX: %*ph\n", data->index, data->rx_buffer);

    /* Sleeps; IRQs are enabled here, so jiffies advances and the timeout works */
    /* Or wait_event_timeout if don't use Ctrl+C -> stop a process blocked in read() until the timeout ended */
    /* If spi_device_read is called during transfer (data->data_ready == false), it blocks */
    /* The condition is only re-evaluated after a wake-up */
    ret = wait_event_interruptible_timeout(data->recv_wait, data->data_ready,
                                           msecs_to_jiffies(3000));
    if (ret == 0)
        return -ETIMEDOUT;
    if (ret < 0)
        return ret;                                 /* signal */

    spin_lock_irqsave(&data->lock, flags);
    n = min_t(unsigned int, data->index, len);
    memcpy(tmp, data->rx_buffer, n);
    data->index        = 0;                         /* ready for next frame */
    data->is_get_count = 0;
    data->data_ready   = false;
    spin_unlock_irqrestore(&data->lock, flags);

    /* copy_to_user can sleep → must be outside the spinlock */
    if (copy_to_user(buf, tmp, n))
        return -EFAULT;
    return n;
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

        /*
         * Apply the configuration to the SPI hardware.
         */
        spi->index        = 0;                         /* ready for next frame */
        spi->is_get_count = 0;
        spi->data_ready   = false;
        spi->done_configured = false;

        spi->cfg.FIFO_trigger = config.FIFO_trigger;
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
    .read =             spi_device_read,
    .unlocked_ioctl =   spi_device_ioctl,
};

/* Probe - unprobe */
static int spi_device_probe(struct platform_device *pdev)
{
    struct spi_device_data *data;
    int ret;
    dev_info(&pdev->dev, "SPI1 device probed successfully with compatible 'spi1-based'\n");

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    platform_set_drvdata(pdev, data);

    data->index = 0;
    data->is_get_count = 0;

    data->base = ioremap(SPI1_BASE, 0x400);
    if (!data->base) {
        dev_err(&pdev->dev, "Failed to map resource\n");
        return -ENOMEM;
    }
    data->dev = &pdev->dev;

    data->clk = devm_clk_get(&pdev->dev, "fck-spi1");
    if (IS_ERR(data->clk)) {
        dev_err(&pdev->dev, "Failed to get clock: %ld\n", PTR_ERR(data->clk));
        iounmap(data->base);
        return PTR_ERR(data->clk);
    }
    ret = clk_prepare_enable(data->clk);
    if (ret) {
        dev_err(&pdev->dev, "Failed to enable clock: %d\n", ret);
        iounmap(data->base);
        return ret;
    }
    dev_info(&pdev->dev, "SPI1 clock rate: %lu Hz\n", clk_get_rate(data->clk));

    INIT_WORK(&data->re_request_work, re_request_irq_work);

    data->done_configured = false;

    spin_lock_init(&data->lock);
    init_waitqueue_head(&data->recv_wait);
    data->data_ready = false;

    data->irq = platform_get_irq(pdev, 0);
    if (data->irq < 0) {
        dev_err(&pdev->dev, "Failed to get IRQ: %d\n", data->irq);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return data->irq;
    }
    dev_info(data->dev, "swirq of SPI1 = %d\n", data->irq);
    ret = devm_request_irq(&pdev->dev, data->irq, irqHandler, 0, "spi1", data);
    if (ret < 0) {
        dev_err(&pdev->dev, "Unable to request IRQ %d: %d\n", data->irq, ret);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return ret;
    }

    ret = alloc_chrdev_region(&data->dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to allocate chrdev region: %d\n", ret);
        kthread_stop(data->recv_thread);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return ret;
    }

    cdev_init(&data->cdev, &spi_device_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->dev_num, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to add cdev: %d\n", ret);
        unregister_chrdev_region(data->dev_num, 1);
        kthread_stop(data->recv_thread);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return ret;
    }

    data->class = class_create(THIS_MODULE, "spi1_class");
    if (IS_ERR(data->class)) {
        dev_err(&pdev->dev, "Failed to create class: %ld\n", PTR_ERR(data->class));
        cdev_del(&data->cdev);
        unregister_chrdev_region(data->dev_num, 1);
        kthread_stop(data->recv_thread);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return PTR_ERR(data->class);
    }

    data->char_dev = device_create(data->class, &pdev->dev, data->dev_num, NULL, "spi1");
    if (IS_ERR(data->char_dev)) {
        dev_err(&pdev->dev, "Failed to create device: %ld\n", PTR_ERR(data->char_dev));
        class_destroy(data->class);
        cdev_del(&data->cdev);
        unregister_chrdev_region(data->dev_num, 1);
        kthread_stop(data->recv_thread);
        clk_disable_unprepare(data->clk);
        iounmap(data->base);
        return PTR_ERR(data->char_dev);
    }

    dev_info(&pdev->dev, "Created /dev/%s\n", DEVICE_NAME);
    return 0;
}

static int spi_device_remove(struct platform_device *pdev)
{
    struct spi_device_data *data = platform_get_drvdata(pdev);

    cancel_work_sync(&data->re_request_work);

    iowrite32(0, data->base + MCSPI_IRQENABLE);

    if (data->rx_chan){
        dma_release_channel(data->rx_chan);
    }

    if (data->recv_thread)
        kthread_stop(data->recv_thread);

    iowrite32(0, data->base + MCSPI_CHCTRL0);

    if (data->dev)
        device_destroy(data->class, data->dev_num);
    if (data->class)
        class_destroy(data->class);
    cdev_del(&data->cdev);
    unregister_chrdev_region(data->dev_num, 1);

    clk_disable_unprepare(data->clk);
    iounmap(data->base);

    dev_info(&pdev->dev, "SPI1 device removed\n");
    return 0;
}

static const struct of_device_id spi_device_of_match[] = {
    { .compatible = "spi1-based" },
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
MODULE_DESCRIPTION("Custom SPI Slave Device Driver for BeagleBone Black SPI1");
