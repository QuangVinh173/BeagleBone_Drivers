#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h> // alloc_chrdev_region
#include <linux/pci.h> // ioremap
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include "usb1.h"
#include "main.h"

/* ================ Const data packet =======================*/
const u8 GetDesc_pkt[8] = {
    0x80,       // bmRequestType: Device-to-host, Standard, Device
    0x06,       // bRequest: USB_REQ_GET_DESCRIPTOR
    0x00, 0x01, // wValue: Descriptor Index = 0 (LOW), Type = DEVICE (1) (HIGH)
    0x00, 0x00, // wIndex: 0
    0x12, 0x00  // wLength: 18 bytes (Device descriptor length)
};

const u8 GetDesc_pkt2[8] = {
    0x80,       // bmRequestType: Device-to-host, Standard, Device
    0x06,       // bRequest: USB_REQ_GET_DESCRIPTOR
    0x00, 0x02, // wValue: Descriptor Index = 0 (LOW), Type = DEVICE (1) (HIGH)
    0x00, 0x00, // wIndex: 0
    0x09, 0x00  // wLength: 18 bytes (Device descriptor length)
};

const u8 GetDescif0_pkt[8] = {
    0x80,       // bmRequestType: Device-to-host, Standard, 
    0x06,       // bRequest: USB_REQ_GET_DESCRIPTOR
    0x00, 0x02, // wValue: Index=0 (LOW), Type=INTERFACE (4) (HIGH) → 0x0400
    0x00, 0x00, // wIndex: Interface number 0
    0x3e, 0x00  // wLength: 9 bytes (Interface descriptor length)
};

const u8 SetAddr_pkt[8] = {
    0x00,       // bmRequestType: Host-to-device, Standard, Device
    0x05,       // bRequest: USB_REQ_SET_ADDRESS
    0x01, 0x00, // wValue: Device address: 1
    0x00, 0x00, // wIndex: 0
    0x00, 0x00  // wLength: 0
};

const u8 GetConf_pkt[8] = {
    0x80,       // bmRequestType: Device-to-host, Standard, Device
    0x08,       // bRequest: USB_REQ_GET_CONFIGURATION
    0x00, 0x01, // wValue: Descriptor Index = 0 (LOW), Type = DEVICE (1) (HIGH)
    0x00, 0x00, // wIndex: 0
    0x01, 0x00  // wLength: 9 bytes (Configuration descriptor length)
};

const u8 SetConf_pkt[8] = {
    0x00,       // bmRequestType: Host-to-Device, Standard, Device
    0x09,       // bRequest: USB_REQ_SET_CONFIGURATION
    0x01, 0x00, // wValue: Configuration value 1 (LOW), 0 (HIGH) → 0x0001
    0x00, 0x00, // wIndex: 0
    0x00, 0x00  // wLength: 0 (no data phase)
};

const u8 SetCoding_pkt[8] = {
    0x21,       // bmRequestType: Host-to-Device, Class, Interface 0
    0x20,       // bRequest: SET_LINE_CODING
    0x00, 0x00, // wValue
    0x00, 0x00, // wIndex
    0x07, 0x00  // wLength: 7-byte data: DWORD baud, byte stop bits, byte parity, byte data bits
};

const u8 SetCodingData[] = {
    // 0x80, 0x25, 0x00, 0x00,  // Baud: 9600 (little-endian DWORD)
    0x00, 0xC2, 0x01, 0x00,  // Baud: 115200 (little-endian DWORD)
    0x00,                    // Stop bits: 0 (1 stop bit)
    0x00,                    // Parity: 0 (none)
    0x08                     // Data bits: 8
};

const u8 SetCtrlLine_pkt[8] = {
    0x21,  // bmRequestType: Host-to-Device, Class, Interface
    0x22,  // bRequest: SET_CONTROL_LINE_STATE
    0x03, 0x00,  // wValue: DTR=1, RTS=1
    0x00, 0x00,  // wIndex: Interface 0
    0x00, 0x00   // wLength: 0
};

/* Transfer state shared with the IRQ handler. */
u16 Tx1_flag = 0, Rx1_flag = 0;
u8 ret;
u16 count;

/* MAP: indexed register
USBCORE1 0x47401C00:
@1C00-1C0F: faddr(1 byte), power(1), ..., index(1), testmode(1).
@1C10-1C1F: EPx control + status register.
@1C20-....: EP0_FIFO_entry(4)-...
 */

/* ================== Utils for control transfer ===================== */
void setIndex(struct usb_device_data *data, u8 epnum){
    iowrite8(epnum, data->base_usb1core + MUSB_INDEX); // @1C0E
}

void setRxEP0(struct usb_device_data *data, u8 epnum, u8 speed, u8 transfer_type){
    u8 RxType = 0;

    RxType = (speed << 6)&MUSB_TYPE_SPEED;
    RxType |= (transfer_type << 4)&MUSB_TYPE_PROTO; // Control type
    RxType |= (epnum << 0)&MUSB_TYPE_REMOTE_END;

    iowrite8(RxType, data->base_usb1core + 0x10 + MUSB_TYPE0);
}

void setFifo(struct usb_device_data *data){
    u8 babblectl = 0;

    // TX
    iowrite8(3, data->base_usb1core + MUSB_TXFIFOSZ); // sz = 3 -> fifo size = 2^(sz+3) = 64 bytes for TX FIFO0
    iowrite16(0x00, data->base_usb1core + MUSB_TXFIFOADD); 

    // RX
    iowrite8(3, data->base_usb1core + MUSB_RXFIFOSZ); // sz = 3 -> fifo size = 2^(sz+3) = 64 bytes for RX FIFO0
    iowrite16(0x00, data->base_usb1core + MUSB_RXFIFOADD); 

    // fifo type0 (8-bit)
    setRxEP0(data, 0, data->full_speed ? FULL_SPEED : LOW_SPEED, CONTROL_TYPE);

    babblectl = ioread8(data->base_usb1core + MUSB_BABBLE_CTL);
    if (babblectl&MUSB_BABBLE_RCV_DISABLE){
		babblectl |= MUSB_BABBLE_SW_SESSION_CTRL;
        iowrite8(babblectl, data->base_usb1core + MUSB_BABBLE_CTL);
	}
}

u32 fifo_offset(u8 epnum)
{
	return 0x20 + (epnum * 4); // @1C20
}

static void copy_bytes(u8 *dst, const u8 *src, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
        dst[i] = src[i];
}

int wait_register_update(struct usb_device_data *data,
                         void __iomem *mem,
                         u16 reg_offset,
                         u16 bit_offset,
                         u8 bit_val,
                         u16 delay_ms,
                         u8 *name_register)
{
    long ret;
    u32 mask;
    u32 expected;
    void __iomem *reg;

    if (!data || !mem || bit_offset >= 32 || bit_val > 1)
        return -EINVAL;

    reg = (u8 __iomem *)mem + reg_offset;
    mask = BIT(bit_offset);
    expected = bit_val ? mask : 0;

    ret = wait_event_interruptible_timeout(
        data->irq_wait,
        (ioread32(reg) & mask) == expected,
        msecs_to_jiffies(delay_ms)
    );

    if (ret < 0) {
        dev_info(data->dev,
                "Wait for register %s interrupted: %ld\n",
                name_register, ret);
        return (int)ret;
    }

    if (ret == 0) {
        dev_err(data->dev,
                "Reg Timeout %s: bit %u expected %u\n",
                name_register, bit_offset, bit_val);
        return -ETIMEDOUT;
    }

    return 0;
}

int wait_val_update(
    struct usb_device_data *data,
    u16 *var,
    u16 val,
    u16 delay_ms,
    u8* name_val)
{
    long ret;
    unsigned long timeout;

    if (!data || !var || val == 0)
        return -EINVAL;

    timeout = msecs_to_jiffies(delay_ms);

    /*
     * Sleep until the requested event bit is set,
     * a signal interrupts the wait, or timeout expires.
     */
    ret = wait_event_interruptible_timeout(
        data->irq_wait,
        (READ_ONCE(*var) & val) != 0,
        timeout);

    if (ret < 0) {
        // dev_info(data->dev,
        //          "Val - Wait for %s interrupted: %ld\n",
        //          name_val, ret);
        return (int)ret;
    }

    if (ret == 0) {
        // dev_err(data->dev,
        //         "Val - Timeout waiting for %s (mask=0x%04x, flags=0x%04x)\n",
        //         name_val, val, READ_ONCE(*var));
        return -ETIMEDOUT;
    }

    return 0;
}

void USB1_SetToken(struct usb_device_data *data, const u8* TokenSet){
    data->InsReq.bRequestType = TokenSet[0];
    data->InsReq.bRequest = TokenSet[1];
    data->InsReq.wValue = (TokenSet[3] << 8 | TokenSet[2]);
    data->InsReq.wIndex = (TokenSet[5] << 8 | TokenSet[4]);
    data->InsReq.wLength = (TokenSet[7] << 8 | TokenSet[6]);
}

void USB1_ClrToken(struct usb_device_data *data){
    u16 i = 0;

    u8* p = (u8*)(&data->InsReq);
    u16 s = sizeof(data->InsReq);

    for (i = 0; i < s; i++){
        p[i] = 0;
    }
}

void USB1_ApplyToken(struct usb_device_data *data, u8 epnum){
    u32 FIFO0_offset = fifo_offset(epnum);
    u32 val[2]; // entry to FIFO has size of 8 bytes

    val[0] = (data->InsReq.wValue << 16) | (data->InsReq.bRequest << 8) | (data->InsReq.bRequestType);
    val[1] = (data->InsReq.wLength << 16) | (data->InsReq.wIndex);
#if (PRINT_DEBUG)
    printk("val[0] = 0x%x, val[1] = 0x%x\n", val[0], val[1]);
#endif
    iowrite32(val[0], data->base_usb1core + FIFO0_offset);
    iowrite32(val[1], data->base_usb1core + FIFO0_offset);

}

u32 USB1_ReadFIFO(struct usb_device_data *data, u8 epnum){
    u32 FIFO0_offset = fifo_offset(epnum);
    return ioread32(data->base_usb1core + FIFO0_offset);
}

u8 USB1_ReadU8FIFO(struct usb_device_data *data, u8 epnum){
    u32 FIFO0_offset = fifo_offset(epnum);
    return ioread8(data->base_usb1core + FIFO0_offset);
}

void USB1_WriteU8DataFIFO(struct usb_device_data *data, u8 epnum, u8 dataX){
    u32 FIFO0_offset = fifo_offset(epnum);
    iowrite8(dataX, data->base_usb1core + FIFO0_offset);
}

/* ================== Handler =================*/
void stop_reset(struct usb_device_data *data){
    u32 power = 0;

    power = ioread32(data->base_usb1core + MUSB_POWER);
    // release reset
    power &=~ MUSB_POWER_RESET;
    iowrite8(power, data->base_usb1core + MUSB_POWER);
}
void reset(struct usb_device_data *data){
    u32 power = 0;

    power = ioread32(data->base_usb1core + MUSB_POWER);
    // release reset
    power |= MUSB_POWER_RESET;
    iowrite8(power, data->base_usb1core + MUSB_POWER);
}

irqreturn_t USB1_handler(int irq, void *d){
    struct usb_device_data *data = d;
    u32 irqst0, irqst1;
    u16 int_tx, int_rx;

    irqst0 = ioread32(data->base_usb1ctl + USB1CTL_IRQST0);
    irqst1 = ioread32(data->base_usb1ctl + USB1CTL_IRQST1);
    int_tx = irqst0&0xFF;
    int_rx = (irqst0 >> 16)&0xFE;

    if (irqst0)
        iowrite32(irqst0, data->base_usb1ctl + USB1CTL_IRQST0);
    if (irqst1)
        iowrite32(irqst1, data->base_usb1ctl + USB1CTL_IRQST1);


    if (irqst0){
        if (int_tx){
            WRITE_ONCE(Tx1_flag, READ_ONCE(Tx1_flag) | int_tx);
#if (PRINT_DEBUG)
            printk("ISR -> TX[0x%x]\n", int_tx);
#endif
        }
        if (int_rx){
            WRITE_ONCE(Rx1_flag, READ_ONCE(Rx1_flag) | int_rx);
#if (PRINT_DEBUG)
            printk("ISR -> RX[0x%x]\n", int_rx);
#endif
        }

        /*
        * Notify threads waiting for USB endpoint events.
        */
        if (int_tx || int_rx)
            wake_up_interruptible(&data->irq_wait);
    }
    if (irqst1){
        if ((irqst1)&(1u << 7)){
            printk("< VBUS valid threshold>\n");
        } 
        if ((irqst1)&(1u << 2)){
            printk("Babble detected\n");
        } 
        if ((irqst1)&(1u << 4)){
            printk("Device connected\n");
            schedule_work(&data->descriptor_work);
        }
            
        if ((irqst1)&(1u << 5)){
            printk("Device disconnected\n");
            schedule_work(&data->descriptor_clear_work);
        }     
        if ((irqst1)&(1u << 8)){
            printk("DRVVBUS level change\n");
        }   
        if ((irqst1)&(1u << 6)){
            printk("SRF ...\n");
        }    
    }

    return IRQ_HANDLED;
}

/* ================ Init funcs ============= */
void USB1_reset(struct usb_device_data *data){
    iowrite32((1u << 0) | (1u << 5), data->base_usb1ctl + USB1CTL_CTRL); // soft reset + isolation
    wait_register_update(data, data->base_usb1ctl, USB1CTL_CTRL, 0, 0, 2000, "RESET"); // wait reset
    wait_register_update(data, data->base_usb1ctl, USB1CTL_CTRL, 5, 0, 2000, "RESET ISOLATION"); // wait reset

}

int USB1_init(struct usb_device_data *data){
    u32 usbcore_pwr = 0, usbcore_testmode = 0;
    u8 power = 0;
    power &= 0xf0;

    // disable testmode
    usbcore_testmode = 0x00;
    iowrite32(usbcore_testmode, data->base_usb1core + MUSB_TESTMODE);

    // init high speed
    usbcore_pwr = MUSB_POWER_ISOUPDATE;
    usbcore_pwr &=~ (MUSB_POWER_HSENAB); /* full speed */
    iowrite32(usbcore_pwr, data->base_usb1core + MUSB_POWER);

    // host mode by sw
    iowrite32(1u << 7, data->base_usb1ctl + USB1CTL_MODE); // host mode by sw

    // test mode

    // enable all interrupts after session
    iowrite32(0xFFFEFFFF, data->base_usb1ctl + USB1CTL_IRQEN0);
    iowrite32(0x1F7, data->base_usb1ctl + USB1CTL_IRQEN1);

    iowrite16(0xFFFF, data->base_usb1core + MUSB_INTRTXE); // enable TX ep0 and 15 eps
    iowrite16(0xFFFE, data->base_usb1core + MUSB_INTRRXE); // enable RX 15 eps
    iowrite8(0xF7, data->base_usb1core + MUSB_INTRUSBE); // 

    // session
    iowrite8(MUSB_DEVCTL_SESSION, data->base_usb1core + MUSB_DEVCTL); // When the USB controller go into session, it will assume the role of a host
    ret = wait_register_update(data, data->base_usb1core, MUSB_DEVCTL, 0, MUSB_DEVCTL_SESSION, 2000, "DEVCTL_SESSION"); // wait DEVCTL_SESSION is set to 1
    if (ret < 0) return -1;

    return 0;
}

void PHY1_init(struct usb_device_data *data){
    u32 usb1_ctrl = 0;

    // return 0 if not clocked
    usb1_ctrl = ioread32(data->base_usb1ctl + USB1CTL_REV);
    if (!usb1_ctrl){
        printk("REV = 0 -> error\n");
        return;
    }

    // usb1 ctrl
    usb1_ctrl = ioread32(data->base_con_usb1ctrl1 + USB_CTRL1);
    usb1_ctrl &= ~(USBPHY_CM_PWRDN | USBPHY_OTG_PWRDN | USBPHY_OTGVDET_EN); // power: normal mode, no Vbus detect as host mode
    usb1_ctrl |= USBPHY_OTGSESSEND_EN;

    iowrite32(usb1_ctrl, data->base_con_usb1ctrl1 + USB_CTRL1);

    msleep(1); // Give the PHY ~1ms to complete the power up operation.
}

void USB1_deinit(struct usb_device_data *data){
    iowrite32(0x1ff, data->base_usb1ctl + USB1CTL_COREINT_CLR);
    iowrite32(0xfffeffff, data->base_usb1ctl + USB1CTL_EPINT_CLR);
	
    /* disable interrupts */
    iowrite8(0, data->base_usb1core + MUSB_INTRUSBE);
    iowrite16(0, data->base_usb1core + MUSB_INTRTXE);
    iowrite16(0, data->base_usb1core + MUSB_INTRRXE);

    /*  flush pending interrupts */
    iowrite8(0xff, data->base_usb1core + MUSB_INTRUSB);
    iowrite16(0xffff, data->base_usb1core + MUSB_INTRTX);
    iowrite16(0xffff, data->base_usb1core + MUSB_INTRRX);

    iowrite8(0, data->base_usb1core + MUSB_DEVCTL);
}

void PHY1_deinit(struct usb_device_data *data){
    u32 usb1_ctrl = 0;

    // return 0 if not clocked
    usb1_ctrl = ioread32(data->base_usb1ctl + USB1CTL_REV);
    if (!usb1_ctrl){
        printk("REV = 0 -> error\n");
        return;
    }

    iowrite32(0, data->base_con_usb1ctrl1 + USB_CTRL1);
}

/* ================== API for Control Transfer ===================== */
/* First, reset */
void USB1_Reset_Speed(struct usb_device_data *data){
    u8 devctl = 0;

    reset(data);
    msleep(5);
    stop_reset(data);

    devctl = ioread8(data->base_usb1core + MUSB_DEVCTL);
    if (devctl&MUSB_DEVCTL_FSDEV) printk("Full speed\n");
    else if (devctl&MUSB_DEVCTL_LSDEV) printk("Low speed\n");
    else printk("Undefined speed\n");

    if (devctl&MUSB_DEVCTL_FSDEV) data->full_speed = true;
    else data->full_speed = false;

    // set ptr
    data->DeviceDescriptorPtr = (u8*)(&data->InsDeviceDescriptor);
}

/* Second -> getting  */
int USB1_SETUP_Phase_GetDesc(struct usb_device_data *data, const u8* pkt, u16 addr, u8* string){
    u16 host_csr0 = 0;
    int ret = 0;

    setIndex(data, 0);
    setFifo(data);

    // flush FIFO if needed (maybe 5 times)
    iowrite16(MUSB_CSR0_FLUSHFIFO, data->base_usb1core + 0x10 + MUSB_TXCSR);

    USB1_ClrToken(data);
    USB1_SetToken(data, pkt);
    USB1_ApplyToken(data, 0); // Load the 8 bytes of the required Device request command into the Endpoint 0 FIFO

    iowrite16(addr, data->base_usb1core + MUSB_TXFUNCADDR);

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0);
    host_csr0 |= MUSB_CSR0_H_SETUPPKT | MUSB_CSR0_TXPKTRDY; // Set SETUPPKT and TXPKTRDY 
    iowrite16(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for INT endpoint 0
    ret = wait_val_update(data, &Tx1_flag, BIT(0), 2000, "SETUP: Token + Data0/1");
    if (ret < 0) return -1;
    Tx1_flag = 0;

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    // Check error for sending SETUP stage
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL for %s\n", string);
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR for %s\n", string); // send additional 2 times
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT for %s\n", string); // .... consider later
        return -1;
    } 

    // set length for getting descriptor
    data->InsReq.leftLength = data->InsReq.wLength; // 

    // if USB_REQ_GET_DESCRIPTOR -> update ptr to fill device descriptor
    if (data->InsReq.bRequest == USB_REQ_GET_DESCRIPTOR) {
        // get the whole descriptor, update ptr at the start + 0x12
        if (data->InsReq.wLength == 0x3e) {
            // as it starts sends data from Device descriptor (bLength2 = 0x9)
            data->DeviceDescriptorPtr = (u8*)(&data->InsDeviceDescriptor) + 0x12; // reset ptr
        }
    }
    return 0;
}

int USB1_IN_Phase_GetDesc(struct usb_device_data *data){
    u16 host_csr0 = 0;
    int ret = 0;
    u32 tmp[2];

    iowrite16(MUSB_CSR0_H_REQPKT, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for Endpoint 0 interrupt (Data packet)
    ret = wait_val_update(data, &Tx1_flag, BIT(0), 2000, "IN: Data0/1");
    if (ret < 0) return -1;
    Tx1_flag = 0;
    
    count = ioread16(data->base_usb1core + 0x10 + MUSB_COUNT0)&0xFFFF;
    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    // Check error
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL - DATA\n");
        return -1;
    }
    else if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR\n"); // the controller has tried to send the required IN token three times without getting any response
        return -1;
    }
    else if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT\n"); // .... consider later
        return -1;
    } 
    else if (host_csr0 == MUSB_CSR0_RXPKTRDY) {
#if (PRINT_DEBUG)
        printk("Reading IN with ACKed!\n");
#endif
        tmp[0] = USB1_ReadFIFO(data, 0);
        tmp[1] = USB1_ReadFIFO(data, 0);

        if (count == 8) {
            copy_bytes(data->DeviceDescriptorPtr, (const u8 *)&tmp[0], 4);
            data->DeviceDescriptorPtr+=4;
            copy_bytes(data->DeviceDescriptorPtr, (const u8 *)&tmp[1], 4);
            data->DeviceDescriptorPtr+=4;
        }
        else if (count <= 4){
            copy_bytes(data->DeviceDescriptorPtr, (const u8 *)&tmp[0], count);
            data->DeviceDescriptorPtr+=(count);
        }
        else {
            copy_bytes(data->DeviceDescriptorPtr, (const u8 *)&tmp[0], 4);
            data->DeviceDescriptorPtr+=4;
            copy_bytes(data->DeviceDescriptorPtr, (const u8 *)&tmp[1], count - 4);
            data->DeviceDescriptorPtr+=(count - 4);
        }

        //printk("0x%x 0x%x\n", tmp[0], tmp[1]);


        // clear RXPKTRDY
        host_csr0 = ioread32(data->base_usb1core + 0x10 + MUSB_CSR0);
        host_csr0 &=~ MUSB_CSR0_RXPKTRDY; 
        iowrite32(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);
    } 
      
    // handle Length
    data->InsReq.maxLengthEntryFIFO = count; // usually 8 bytes for endpoint 0
    if (data->InsReq.leftLength < 8){
        data->InsReq.leftLength = 0;
    } 
    else {
        data->InsReq.leftLength = data->InsReq.leftLength - data->InsReq.maxLengthEntryFIFO;
    }
    return 0;
}

int USB1_STATUS_Phase_GetDesc(struct usb_device_data *data){
    u16 host_csr0 = 0;

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0);
    host_csr0 |= MUSB_CSR0_H_STATUSPKT | MUSB_CSR0_TXPKTRDY; // Set STATUSPKT and TXPKTRDY 
    iowrite16(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for INT endpoint 0
    ret = wait_val_update(data, &Tx1_flag, BIT(0), 2000, "SETUP: Token + Data0/1");
    if (ret < 0) return -1;
    Tx1_flag = 0;

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    // Check error for sending SETUP stage
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL\n");
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR\n"); // send additional 2 times
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT\n"); // .... consider later
        return -1;
    } 
    return 0;
}

/* Third -> setting */
int USB1_SETUP_Phase_SetAddr(struct usb_device_data *data, const u8* pkt, u16 addr, u8* string){
    u16 host_csr0 = 0;
    int ret = 0;

    setIndex(data, 0);
    setFifo(data);

    // flush FIFO if needed (maybe 5 times)
    iowrite16(MUSB_CSR0_FLUSHFIFO, data->base_usb1core + 0x10 + MUSB_TXCSR);

    USB1_ClrToken(data);
    USB1_SetToken(data, pkt);
    USB1_ApplyToken(data, 0); // Load the 8 bytes of the required Device request command into the Endpoint 0 FIFO

    iowrite16(addr, data->base_usb1core + MUSB_TXFUNCADDR);
    iowrite16(addr, data->base_usb1core + MUSB_RXFUNCADDR);

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0);
    host_csr0 |= MUSB_CSR0_H_SETUPPKT | MUSB_CSR0_TXPKTRDY; // Set SETUPPKT and TXPKTRDY 
    iowrite16(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for INT endpoint 0
    ret = wait_val_update(data, &Tx1_flag, BIT(0), 2000, "SETUP: Token + Data0/1 vvv");
    if (ret < 0) return -1;
    Tx1_flag = 0;

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    // Check error for sending SETUP stage
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL for %s\n", string);
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR for %s\n", string); // send additional 2 times
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT for %s\n", string); // .... consider later
        return -1;
    } 

    // set length for getting descriptor
    data->InsReq.leftLength = data->InsReq.wLength; // 0 bytes
    //data->DeviceDescriptorPtr = (u8*)(&data->InsDeviceDescriptor);

    return 0;
}

int USB1_OUT_Phase_GetDesc(struct usb_device_data *data, const u8* dataX, u32 len){
    u16 host_csr0 = 0;
    int ret = 0, i = 0;
    int epointNum = 0;

    for (i = 0; i < len; i++){
        USB1_WriteU8DataFIFO(data, epointNum, dataX[i]);
    }

    iowrite16(MUSB_CSR0_TXPKTRDY, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for Endpoint 0 interrupt (Data packet)
    ret = wait_val_update(data, &Tx1_flag, BIT(epointNum), 2000, "IN: Data0/1");
    if (ret < 0) return -1;
    Tx1_flag = 0;
    
    count = ioread16(data->base_usb1core + 0x10 + MUSB_COUNT0)&0xFFFF;
    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    // Check error
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL - DATA\n");
        return -1;
    }
    else if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR\n"); // the controller has tried to send the required IN token three times without getting any response
        return -1;
    }
    else if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT\n"); // .... consider later
        return -1;
    } 
     
    return 0;
}

int USB1_STATUS_Phase_SetAddr(struct usb_device_data *data){
    u16 host_csr0 = 0;
    int ret = 0;

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0);
    host_csr0 = MUSB_CSR0_H_STATUSPKT | MUSB_CSR0_H_REQPKT; // Set STATUSPKT and TXPKTRDY 
    iowrite16(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);

    // wait for INT endpoint 0
    ret = wait_val_update(data, &Tx1_flag, BIT(0), 2000, "SETUP: Token + Data0/1 yyy");
    Tx1_flag = 0;
    if (ret < 0) {
        return -1;
    }

    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_CSR0)&0xFF;
    //printk("ret = %x\n", host_csr0);

    // Check error for sending SETUP stage
    if (host_csr0 == MUSB_CSR0_H_RXSTALL) {
        dev_info(data->dev, "RXSTALL\n");
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_ERROR) {
        dev_info(data->dev, "ERROR\n"); // send additional 2 times
        return -1;
    }
    if (host_csr0 == MUSB_CSR0_H_NAKTIMEOUT) {
        dev_info(data->dev, "NAK_TIMEOUT\n"); 
        return -1;
    } 
    else if (host_csr0&MUSB_CSR0_RXPKTRDY) {
#if (PRINT_DEBUG)
        printk("Sending STATUS with ACKed!\n");
#endif
        // clear RXPKTRDY
        host_csr0 = ioread32(data->base_usb1core + 0x10 + MUSB_CSR0);
        host_csr0 &=~ MUSB_CSR0_RXPKTRDY; 
        iowrite32(host_csr0, data->base_usb1core + 0x10 + MUSB_CSR0);
    } 
    return 0;
}

/* Read + Write */
int USB1_READ_Transaction(struct usb_device_data *data, const u8* pkt, u16 addr, u8* string){

    int ret = 0;
    ret = USB1_SETUP_Phase_GetDesc(data, pkt, addr, string);

    if (ret == 0){
        do {
            ret = USB1_IN_Phase_GetDesc(data);
        } while(data->InsReq.leftLength && (ret >= 0));
    }

    if (ret == 0){
        ret = USB1_STATUS_Phase_GetDesc(data);
    }

    return ret;
}

int USB1_WRITE_Transaction(struct usb_device_data *data, const u8* pkt, u16 addr, const u8* dataX, u32 len, u8* string){

    int ret = 0;
    ret = USB1_SETUP_Phase_SetAddr(data, pkt, addr, string);

    if (ret == 0){
        if (len) {
            ret = USB1_OUT_Phase_GetDesc(data, dataX, len);
        }
    }   

    if (ret == 0){
        ret = USB1_STATUS_Phase_SetAddr(data);
    }

    return ret;
}

/* Main */
int USB1_GetDesc_Transfer(struct usb_device_data *data){
    int ret;

    /* Getting descriptor with default address 0 */
    ret = USB1_READ_Transaction(data, GetDesc_pkt, DEFAULT_ADDR, "GetDesc_pkt");

    /* Setting address 0x1 with default address 0 */
    if (ret == 0){
        ret = USB1_WRITE_Transaction(data, SetAddr_pkt, DEFAULT_ADDR, NULL, 0, "SetAddr_pkt");
    }

    /* Getting descriptor with new address 0x1 */
    if (ret == 0){
        ret = USB1_READ_Transaction(data, GetDesc_pkt, ARDUINO_DEVICE_ADDR, "GetDesc_pkt");
    }

    /* Getting descriptor with new address 0x1 */
    if (ret == 0){
        ret = USB1_READ_Transaction(data, GetDesc_pkt2, ARDUINO_DEVICE_ADDR, "GetDesc_pkt2");
    }

    /* Getting descriptor (Interface 0: Communications Class (CDC)) with new address 0x1 */
    if (ret == 0){
        ret = USB1_READ_Transaction(data, GetDescif0_pkt, ARDUINO_DEVICE_ADDR, "GetDescif0_pkt");
    }

    /* Setting configuration with new address 0x1 */
    if (ret == 0){
        ret = USB1_WRITE_Transaction(data, SetConf_pkt, ARDUINO_DEVICE_ADDR, NULL, 0, "SetConf_pkt");
    }

    /* Getting configuration with new address 0x1, data recieved should be 0x1 */
    if (ret == 0){
        ret = USB1_READ_Transaction(data, GetConf_pkt, ARDUINO_DEVICE_ADDR, "GetConf_pkt");
    }

    /* Setting line coding with new address 0x1 */
    if (ret == 0){
        ret = USB1_WRITE_Transaction(data, SetCoding_pkt, ARDUINO_DEVICE_ADDR, SetCodingData, 7, "SetCoding_pkt");
    }

    /* Setting control line coding with new address 0x1 */
    if (ret == 0){
        ret = USB1_WRITE_Transaction(data, SetCtrlLine_pkt, ARDUINO_DEVICE_ADDR, NULL, 0, "SetCtrlLine_pkt");
    }

    return ret;
}

/* Print result */
void USB1_Print_DeviceDescriptor(struct usb_device_data *data){
    printk("# ----------------------------------- #\n");
    printk("# bLength1 = 0x%x\n", data->InsDeviceDescriptor.bLength1);
    printk("# bDescriptorType1 = 0x%x\n", data->InsDeviceDescriptor.bDescriptorType1);
    printk("# bcdUSB = 0x%x\n", data->InsDeviceDescriptor.bcdUSB);
    printk("# bDeviceClass = 0x%x\n", data->InsDeviceDescriptor.bDeviceClass);
    printk("# bDeviceSubClass = 0x%x\n", data->InsDeviceDescriptor.bDeviceSubClass);
    printk("# bDeviceProtocol = 0x%x\n", data->InsDeviceDescriptor.bDeviceProtocol);
    printk("# bMaxPacketSize = 0x%x\n", data->InsDeviceDescriptor.bMaxPacketSize);
    printk("# idVendor = 0x%x\n", data->InsDeviceDescriptor.idVendor);
    printk("# idProduct = 0x%x\n", data->InsDeviceDescriptor.idProduct);
    printk("# bcdDevice = 0x%x\n", data->InsDeviceDescriptor.bcdDevice);
    printk("# iManufacturer = 0x%x\n", data->InsDeviceDescriptor.iManufacturer);
    printk("# iProduct = 0x%x\n", data->InsDeviceDescriptor.iProduct);
    printk("# iSerialNumber = 0x%x\n", data->InsDeviceDescriptor.iSerialNumber);
    printk("# bNumConfigurations = 0x%x\n", data->InsDeviceDescriptor.bNumConfigurations);
    printk("# ----------------------------------- #\n");
}

void USB1_Print_DeviceDescriptor2(struct usb_device_data *data){
    printk("# --------Device descriptor----------- #\n");
    printk("# bLength2 = 0x%x\n", data->InsDeviceDescriptor.bLength2);
    printk("# bDescriptorType2 = 0x%x\n", data->InsDeviceDescriptor.bDescriptorType2);
    printk("# wTotalLenght = 0x%x\n", data->InsDeviceDescriptor.wTotalLenght);
    printk("# bNumInterfaces = 0x%x\n", data->InsDeviceDescriptor.bNumInterfaces);
    printk("# bConfigurationValue = 0x%x\n", data->InsDeviceDescriptor.bConfigurationValue);
    printk("# iConfiguration = 0x%x\n", data->InsDeviceDescriptor.iConfiguration);
    printk("# bmAttributes = 0x%x\n", data->InsDeviceDescriptor.bmAttributes);
    printk("# bMaxPower = 0x%x\n", data->InsDeviceDescriptor.bMaxPower);
    printk("# ----------------------------------- #\n");
}

void USB1_Print_DeviceDescriptorIf0(struct usb_device_data *data){
    printk("# --------Device descriptor----------- #\n");
    printk("# if0_bLength = 0x%x\n", data->InsDeviceDescriptor.if0_bLength);
    printk("# if0_bDescriptorType = 0x%x\n", data->InsDeviceDescriptor.if0_bDescriptorType);
    printk("# if0_bInterfaceNumber = 0x%x\n", data->InsDeviceDescriptor.if0_bInterfaceNumber);
    printk("# if0_bAlternateSetting = 0x%x\n", data->InsDeviceDescriptor.if0_bAlternateSetting);
    printk("# if0_bNumEndpoints = 0x%x\n", data->InsDeviceDescriptor.if0_bNumEndpoints);
    printk("# if0_bInterfaceClass = 0x%x\n", data->InsDeviceDescriptor.if0_bInterfaceClass);
    printk("# if0_bInterfaceSubClass = 0x%x\n", data->InsDeviceDescriptor.if0_bInterfaceSubClass);
    printk("# if0_bInterfaceProtocol = 0x%x\n", data->InsDeviceDescriptor.if0_bInterfaceProtocol);
    printk("# if0_iInterface = 0x%x\n", data->InsDeviceDescriptor.if0_iInterface);

    printk("\n");
    printk("# ep_int_bLength = 0x%x\n", data->InsDeviceDescriptor.ep_int_bLength);
    printk("# if1_bLength = 0x%x\n", data->InsDeviceDescriptor.if1_bLength);

    printk("\n");
    printk("# ep_bulk_out_bLength = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_bLength);
    printk("# ep_bulk_out_bDescriptorType = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_bDescriptorType);
    printk("# ep_bulk_out_bEndpointAddress = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_bEndpointAddress);
    printk("# ep_bulk_out_bmAttributes = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_bmAttributes);
    printk("# ep_bulk_out_wMaxPacketSize = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_wMaxPacketSize);
    printk("# ep_bulk_out_bInterval = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_out_bInterval);

    printk("\n");
    printk("# ep_bulk_in_bLength = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_bLength);
    printk("# ep_bulk_in_bDescriptorType = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_bDescriptorType);
    printk("# ep_bulk_in_bEndpointAddress = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_bEndpointAddress);
    printk("# ep_bulk_in_bmAttributes = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_bmAttributes);
    printk("# ep_bulk_in_wMaxPacketSize = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_wMaxPacketSize);
    printk("# ep_bulk_in_bInterval = 0x%x\n", data->InsDeviceDescriptor.ep_bulk_in_bInterval);
    printk("# ----------------------------------- #\n");
}

void USB1_PrintCoreConfig(struct usb_device_data *data)
{
    void __iomem *core = data->base_usb1core;
    u8 cfg, raminfo;
 
    setIndex(data, 0);                                /* required: CONFIGDATA only at INDEX 0 */
    cfg     = ioread8(core + 0x10 + MUSB_CONFIGDATA); /* 0x1F */
    raminfo = ioread8(core + 0x79);                   /* RAMINFO */
 
    dev_info(data->dev, "CONFIGDATA = 0x%02x\n", cfg);
    dev_info(data->dev, "  Dynamic FIFO : %s\n",
             (cfg & MUSB_CONFIGDATA_DYNFIFO) ? "yes" : "no");
    dev_info(data->dev, "  Bulk split TX: %s, combine RX: %s\n",
             (cfg & MUSB_CONFIGDATA_MPTXE) ? "yes" : "no",
             (cfg & MUSB_CONFIGDATA_MPRXE) ? "yes" : "no");
    dev_info(data->dev, "  UTMI width   : %d-bit\n",
             (cfg & MUSB_CONFIGDATA_UTMIDW) ? 16 : 8);
    dev_info(data->dev, "FIFO RAM = %u bytes, DMA channels = %u\n",
             (1u << (raminfo & 0x0F)) * 4, raminfo >> 4);
}

/* ================== Utils for bulk transfer ===================== */

/* TX */
void USB1_Bulk_SetAddrTx(struct usb_device_data *data, u8 addr, u8 epnum){
    iowrite16(addr, data->base_usb1core + MUSB_TXFUNCADDR + (0x08 * epnum));
}
void USB1_Bulk_SetTypeTx(struct usb_device_data *data, u8 epnum, u8 speed, u8 transfer_type){
    u8 TxType = 0;

    TxType = (speed << 6)&MUSB_TYPE_SPEED;
    TxType |= (transfer_type << 4)&MUSB_TYPE_PROTO; // bulk type
    TxType |= (epnum << 0)&MUSB_TYPE_REMOTE_END;

    iowrite8(TxType, data->base_usb1core + 0x10 + MUSB_TXTYPE);
}
void USB1_Bulk_SetTxMaxp(struct usb_device_data *data, u16 maxp){
    iowrite16(maxp, data->base_usb1core + 0x10 + MUSB_TXMAXP);
}
void USB1_Bulk_SetTxInterval(struct usb_device_data *data, u8 txInterval){
    iowrite8(txInterval, data->base_usb1core + 0x10 + MUSB_TXINTERVAL);
}
void USB1_Bulk_SetTXCSR(struct usb_device_data *data){
    u16 txcsr = 0;

    txcsr = MUSB_TXCSR_MODE; // MODE bit (bit 13) to 1 to ensure the FIFO is enabled
    txcsr &=~ MUSB_TXCSR_FRCDATATOG; // 0 to allow normal data toggle operations
    txcsr &=~ MUSB_TXCSR_AUTOSET; // 
    iowrite8(txcsr, data->base_usb1core + 0x10 + MUSB_TXCSR);
}

/* RX */
void USB1_Bulk_SetAddrRx(struct usb_device_data *data, u8 addr, u8 epnum){
    iowrite16(addr, data->base_usb1core + MUSB_RXFUNCADDR + (0x08 * epnum));
}
void USB1_Bulk_SetTypeRx(struct usb_device_data *data, u8 epnum, u8 speed, u8 transfer_type){
    u8 RxType = 0;

    RxType = (speed << 6)&MUSB_TYPE_SPEED;
    RxType |= (transfer_type << 4)&MUSB_TYPE_PROTO; // bulk type
    RxType |= (epnum << 0)&MUSB_TYPE_REMOTE_END;

    iowrite8(RxType, data->base_usb1core + 0x10 + MUSB_RXTYPE);
}
void USB1_Bulk_SetRxMaxp(struct usb_device_data *data, u16 maxp){
    iowrite16(maxp, data->base_usb1core + 0x10 + MUSB_RXMAXP);
}
void USB1_Bulk_SetRxInterval(struct usb_device_data *data, u8 rxInterval){
    iowrite8(rxInterval, data->base_usb1core + 0x10 + MUSB_RXINTERVAL);
}

void USB1_SetupBulkFifos(struct usb_device_data *data, u8 ep_out, u8 ep_in)
{
    void __iomem *core = data->base_usb1core;
    u16 MaxPacketSzTX, TXInterval;
    u8 transfer_typeTX;
    u16 MaxPacketSzRX, RXInterval;
    u8 transfer_typeRX;

    // TX
    MaxPacketSzTX = data->InsDeviceDescriptor.ep_bulk_out_wMaxPacketSize;
    TXInterval = data->InsDeviceDescriptor.ep_bulk_out_bInterval;
    transfer_typeTX = data->InsDeviceDescriptor.ep_bulk_out_bmAttributes;

    // RX
    MaxPacketSzRX = data->InsDeviceDescriptor.ep_bulk_in_wMaxPacketSize;
    RXInterval = data->InsDeviceDescriptor.ep_bulk_in_bInterval;
    transfer_typeRX = data->InsDeviceDescriptor.ep_bulk_in_bmAttributes;
 
    // ====================================
    setIndex(data, ep_out);                              /* EP4 TX */
    iowrite8(3, core + MUSB_TXFIFOSZ);                   /* 64 bytes */
    iowrite16(64 >> 3, core + MUSB_TXFIFOADD);           /* RAM 64..127 */
    //iowrite16(MUSB_TXCSR_MODE | MUSB_TXCSR_CLRDATATOG, core + 0x10 + MUSB_TXCSR);

    USB1_Bulk_SetAddrTx(data, ARDUINO_DEVICE_ADDR, ep_out);
    USB1_Bulk_SetTypeTx(data, ep_out, (data->full_speed) ? FULL_SPEED : LOW_SPEED, transfer_typeTX); 
    USB1_Bulk_SetTxMaxp(data, MaxPacketSzTX); // max packet is 64 bytes
    USB1_Bulk_SetTxInterval(data, TXInterval); // as ep_bulk_out_bInterval = 0x1
    USB1_Bulk_SetTXCSR(data);

    // ======================================

    setIndex(data, ep_in);  
    iowrite8(3, core + MUSB_RXFIFOSZ);                   /* 64 bytes */
    iowrite16(128 >> 3, core + MUSB_RXFIFOADD);          /* RAM 128..191 */
    //iowrite16(MUSB_RXCSR_CLRDATATOG, core + 0x10 + MUSB_RXCSR);

    USB1_Bulk_SetAddrRx(data, ARDUINO_DEVICE_ADDR, ep_in);
    USB1_Bulk_SetTypeRx(data, ep_in, (data->full_speed) ? FULL_SPEED : LOW_SPEED, transfer_typeRX); 
    USB1_Bulk_SetRxMaxp(data, MaxPacketSzRX); // max packet (usually 64 byres)
    USB1_Bulk_SetRxInterval(data, RXInterval); // as ep_bulk_in_bInterval = 0x1
}

static void tx_flag_clear(u8 ep)
{
    unsigned long flags;
 
    local_irq_save(flags);
    Tx1_flag &= ~BIT(ep);
    local_irq_restore(flags);
}
 
/* Caller holds hw_lock and INDEX = epnum. Clears error bits, drops a loaded packet. */
static void USB1_TxRecover(struct usb_device_data *data, u8 epnum)
{
    void __iomem *csr = data->base_usb1core + 0x10 + MUSB_TXCSR;
    u16 v = ioread16(csr);
 
    v &= ~TXCSR_H_ERRS;                              /* write 0 = clear error bits */
    if (v & (MUSB_TXCSR_TXPKTRDY | MUSB_TXCSR_FIFONOTEMPTY))
        v |= MUSB_TXCSR_FLUSHFIFO | MUSB_TXCSR_TXPKTRDY;
    iowrite16(v, csr);
    tx_flag_clear(epnum);                            /* flush raises a TX IRQ */
}
 
/* One packet. 0 = ACKed, -ETIMEDOUT = device NAKed (retry), -EPIPE = stall, -EIO = error */
static int USB1_OUT_Packet(struct usb_device_data *data, u8 epnum,
                           const u8 *buf, u16 len)
{
    void __iomem *csr = data->base_usb1core + 0x10 + MUSB_TXCSR;
    u16 v;
    int ret, i;
 
    /* ---- start (locked) ---- */
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_lock(&data->hw_lock);
#endif
    setIndex(data, epnum);
 
    v = ioread16(csr);
    if (v & TXCSR_H_ERRS)                            /* stale errors from before */
        iowrite16(v & ~TXCSR_H_ERRS, csr);
 
    for (i = 0; i < len; i++)
        USB1_WriteU8DataFIFO(data, epnum, buf[i]);
 
    tx_flag_clear(epnum);                            /* before starting */
    v = ioread16(csr) | MUSB_TXCSR_TXPKTRDY | MUSB_TXCSR_H_WZC_BITS;
    iowrite16(v, csr);
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_unlock(&data->hw_lock);
#endif
 
    /* ---- wait (not locked) ---- */
    ret = wait_val_update(data, &Tx1_flag, BIT(epnum), 500, "OUT Bulk");
 
    /* ---- check (locked, INDEX re-selected) ---- */
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_lock(&data->hw_lock);
#endif
    setIndex(data, epnum);
    v = ioread16(csr);                               /* full 16 bits, fresh value */
    tx_flag_clear(epnum);
 
    if (ret == 0 && !(v & (TXCSR_H_ERRS | MUSB_TXCSR_TXPKTRDY))) {
#if (!BUG_NOT_ENOUGH_BYTE_RX)
        mutex_unlock(&data->hw_lock);
#endif
        return 0;                                    /* ACKed */
    }

    dev_warn_ratelimited(data->dev, "NOT ACKed\n");
 
    USB1_TxRecover(data, epnum);                     /* still under the lock */
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_unlock(&data->hw_lock);
#endif
 
    if (v & MUSB_TXCSR_H_RXSTALL)
        return -EPIPE;
    if ((v & MUSB_TXCSR_H_NAKTIMEOUT) || (v & MUSB_TXCSR_TXPKTRDY))
        return -ETIMEDOUT;                           /* not sent: data toggle unchanged */
    dev_warn_ratelimited(data->dev, "OUT EP%u error, TXCSR=0x%04x\n", epnum, v);
    return -EIO;
}
 
/* ================== API for Bulk Transfer ===================== */
/* Returns bytes sent (>0) or a negative errno if nothing was sent */
int USB1_OUT_Phase_Bulk(struct usb_device_data *data, u8 epnum, u8 addr,
                        const u8 *dataX, u32 len)
{
    u16 maxp = data->InsDeviceDescriptor.ep_bulk_out_wMaxPacketSize;
    u32 off = 0;
    int ret = 0, tries;
 
    while (off < len) {
        u16 pkt = min_t(u32, maxp, len - off);
 
        for (tries = 0; ; tries++) {
            ret = USB1_OUT_Packet(data, epnum, dataX + off, pkt);
            if (ret != -ETIMEDOUT || tries >= 100)
                break;

            printk("TRY OUT AGAIN\n");
            usleep_range(1000, 2000);               /* RX thread drains the 16U2 */
        }
        if (ret)
            break;
        off += pkt;
    }
    return off ? (int)off : ret;
}

int USB1_IN_Phase_Bulk(struct usb_device_data* data, u8 epnum, u8 addr, u8 *dataX, u16* len){
    u16 host_csr0 = 0, rxcsr = 0;
    int ret = 0, i = 0;
    u16 MaxPacketSz, RXInterval;
    u8 transfer_type;
    *len = 0;
 
    MaxPacketSz = data->InsDeviceDescriptor.ep_bulk_in_wMaxPacketSize;
    RXInterval = data->InsDeviceDescriptor.ep_bulk_in_bInterval;
    transfer_type = data->InsDeviceDescriptor.ep_bulk_in_bmAttributes;

#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_lock(&data->hw_lock); /***** */
#endif
    setIndex(data, epnum);
 
    // 1. forget any old event
    Rx1_flag &= ~BIT(epnum);
 
    //USB1_RxFlush(data, epnum);
    // 2. start this packet
    rxcsr = MUSB_RXCSR_H_REQPKT;
    iowrite16(rxcsr, data->base_usb1core + 0x10 + MUSB_RXCSR);
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_unlock(&data->hw_lock); /***** */
#endif
 
    // 3. wait for Endpoint interrupt (Data packet)
    ret = wait_val_update(data, &Rx1_flag, BIT(epnum), 200, "IN Bulk: Data0/1");
    if (ret < 0) return -1;
    Rx1_flag &= ~BIT(epnum);

 
    setIndex(data, epnum); /* re-select index */
    // wait MUSB_RXCSR_RXPKTRDY
    ret = wait_register_update(data, data->base_usb1core, 0x10 + MUSB_RXCSR, 0, 1, 200, "MUSB_RXCSR_RXPKTRDY");
    if (ret < 0) {
        return -1;
    }

#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_lock(&data->hw_lock); /***** */
#endif
    setIndex(data, epnum); /* re-select index */
 
    count = ioread16(data->base_usb1core + 0x10 + MUSB_RXCOUNT)&0xFFFF;
 
    host_csr0 = ioread16(data->base_usb1core + 0x10 + MUSB_RXCSR)&0xFF;
    *len = count;
#if (PRINT_DEBUG)
    printk("host_csr0 = 0x%x, count = 0x%x\n", host_csr0, count);
#endif
    // Check error
    if (host_csr0&MUSB_RXCSR_H_RXSTALL) {
        dev_warn_ratelimited(data->dev, "RXSTALL - DATA\n");
#if (!BUG_NOT_ENOUGH_BYTE_RX)
        mutex_unlock(&data->hw_lock); /***** */
#endif
        return -1;
    }
    if (host_csr0&MUSB_RXCSR_H_ERROR) {
        dev_warn_ratelimited(data->dev, "ERROR\n"); // the controller has tried to send the required IN token three times without getting any response
#if (!BUG_NOT_ENOUGH_BYTE_RX)
        mutex_unlock(&data->hw_lock); /***** */
#endif
        return -1;
    }
 
    /* can set to 0x08 */
    if (host_csr0&MUSB_CSR0_H_NAKTIMEOUT) {
        dev_warn_ratelimited(data->dev, "NAK_TIMEOUT\n"); // .... consider later
#if (!BUG_NOT_ENOUGH_BYTE_RX)
        mutex_unlock(&data->hw_lock); /***** */
#endif
        return -1;
    }
 
    if (host_csr0&MUSB_RXCSR_RXPKTRDY){
#if (PRINT_DEBUG_TX_RX_SYNC)
        printk("IN ACKed\n");
#endif
        for (i = 0; i < count; i++) {
            dataX[i] = USB1_ReadU8FIFO(data, epnum);
        }
 
        // clear RXPKTRDY
        host_csr0 = ioread32(data->base_usb1core + 0x10 + MUSB_RXCSR);
        host_csr0 &=~ MUSB_RXCSR_RXPKTRDY; 
        iowrite32(host_csr0, data->base_usb1core + 0x10 + MUSB_RXCSR);
    }
 
#if (!BUG_NOT_ENOUGH_BYTE_RX)
    mutex_unlock(&data->hw_lock); /***** */
#endif
    return 0;
}
 
// Producer: the thread polls IN and fills the kfifo:
int usb1_rx_thread(void *arg)
{
    struct usb_device_data *data = arg;
    u8 ep;
    u8 pkt[64];
    u16 n;
    int ret;

    ep = data->InsDeviceDescriptor
             .ep_bulk_in_bEndpointAddress & 0x0F;

    while (!kthread_should_stop()) {

        n = 0;

#if (BUG_NOT_ENOUGH_BYTE_RX)
        mutex_lock(&data->hw_lock);
#endif

        ret = USB1_IN_Phase_Bulk(
            data,
            ep,
            ARDUINO_DEVICE_ADDR,
            pkt,
            &n);

#if (BUG_NOT_ENOUGH_BYTE_RX)
        mutex_unlock(&data->hw_lock);
#endif

        if (ret < 0) {
            continue;
        }

        if (n > 0) {
            unsigned int put;

#if (PRINT_DEBUG_TX_RX_SYNC)
            printk("IN packet = %u bytes\n", n);
            //printk("Last char = 0x%x\n", pkt[n - 1]);

            if (n >= 3) {
                printk(KERN_INFO "RX packet tail: 0x%02x 0x%02x 0x%02x\n",
                    pkt[0],
                    pkt[1],
                    pkt[2]);
            }
#endif

#if (!BUG_NOT_ENOUGH_BYTE_RX)
            mutex_lock(&data->rx_fifo_lock);
#endif
            put = kfifo_in(
                &data->rx_fifo,
                pkt,
                n);
#if (!BUG_NOT_ENOUGH_BYTE_RX)
            mutex_unlock(&data->rx_fifo_lock);
#endif

            if (put != n) {
                dev_warn_ratelimited(
                    data->dev,
                    "RX software FIFO full");
            }

            if (put)
                wake_up_interruptible(&data->rx_wait);

        } else {
            //usleep_range(200, 500);
        }
    }

    return 0;
}
