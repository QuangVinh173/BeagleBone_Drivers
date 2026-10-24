#include <linux/module.h>
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
#include "mass_storage.h"
#include "fat32.h"
#include "msc_ctl.h"

#define DRIVER_NAME "usb1_driver"
#define DEVICE_NAME "usb1"

/*
|-[ ] 'Protocols'
   |-[ ] 'i2c'
      |---- 'data.txt'
         |-[.] 'i2c is a handy protocol.'
   |-[ ] 'spi'
      |---- 'data1.txt'
         |-[.] 'spi is a fast protocol.'
|---- 'Readme.txt'
   |-[.] 'Lists of protocols.'
*/
void Test_phase_1(struct usb_device_data *data){
    int ret;

    /* Protocols folder */
    printk("======= ./Protocols =======\n");
    ret = USB1_f_Make_Dir(data, "./Protocols");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }

    /* i2c */
    printk("======= ./Protocols/i2c =======\n");
    ret = USB1_f_Make_Dir(data, "./Protocols/i2c");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }

    printk("======= ./Protocols/i2c/data.txt =======\n");
    ret = USB1_f_Make_File(data, "./Protocols/i2c/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }

    printk("======= i2c is a handy protocol. =======\n");
    ret = USB1_f_Add_Content(data, "./Protocols/i2c/data.txt", "i2c is a handy protocol.");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }

    /* spi */
    printk("======= ./Protocols/spi =======\n");
    ret = USB1_f_Make_Dir(data, "./Protocols/spi");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }

    printk("======= ./Protocols/spi/data1.txt =======\n");
    ret = USB1_f_Make_File(data, "./Protocols/spi/data1.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }

    printk("======= spi is a fast protocol. =======\n");
    ret = USB1_f_Add_Content(data, "./Protocols/spi/data1.txt", "spi is a fast protocol.");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }

    /* readme */
    printk("======= ./Readme.txt =======\n");
    ret = USB1_f_Make_File(data, "./Readme.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }

    printk("======= Lists of protocols. =======\n");
    ret = USB1_f_Add_Content(data, "./Readme.txt", "Lists of protocols.");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }
}

/*
|---- 'Readme.txt'
   |-[.] 'Lists of protocols.'
|-[ ] 'Languages'
*/
void Test_phase_2(struct usb_device_data *data){
    int ret;

    ret = USB1_f_Make_Dir(data, "./Countries");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }
    ret = USB1_f_Make_Dir(data, "./Languages");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }
    ret = USB1_f_Remove_Dir(data, "./Countries");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }
    
}

void Test_phase_3(struct usb_device_data *data){
    int ret;

    ret = USB1_f_Make_File(data, "./Countries.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }
    ret = USB1_f_Make_File(data, "./Languages.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }
    ret = USB1_f_Remove_File(data, "./Countries.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Remove_File\n");
    }
    ret = USB1_f_Add_Content(data, "./Languages.txt", "Lanaguages are funny hahaha!!");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
    }
}

void Test_phase_4(struct usb_device_data *data){
    int ret;

    printk("======= ./Countries =======\n");
    ret = USB1_f_Make_Dir(data, "./Countries");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }

    printk("======= ./.Hidden_Languages =======\n");
    ret = USB1_f_Make_Dir(data, "./.Hidden_Languages");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }

    printk("======= ./Countries/data.txt =======\n");
    ret = USB1_f_Make_File(data, "./Countries/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
    }
    printk("======= ./Countries/data.txt/'content' =======\n");
    ret = USB1_f_Add_Content(data, "./Countries/data.txt", "This is a hidden language");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    // printk("======= ./.Trash-1000' =======\n");
    // ret = USB1_f_Remove_Dir(data, "./.Trash-1000");
    // if (ret < 0){
    //     printk("Failed at USB1_f_Remove_Dir\n");
    // }
}

void Test_phase_5(struct usb_device_data *data){
    int ret;
    int i = 0;

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_Dir(data, "./Languages");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./Languages/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./Languages/data.txt", "Lanaguages are funny hahaha!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Read_File(data, "./Languages/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Read_File\n");
    }


    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_Dir(data, "./Countries");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./Countries/VietNam.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }
    ret = USB1_f_Add_Content(data, "./Countries/VietNam.txt", "VietNam is a great country!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./Countries/China.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./Countries/China.txt", "China is next to VietNam!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_Dir(data, "./.Hidden_item");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
        return;
    }
    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./.Hidden_item/tea.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }
    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./.Hidden_item/tea.txt", "Tea is made in China!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./.Hidden_item/coffee.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }
    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./.Hidden_item/coffee.txt", "coffee is made in Western!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Read_All(data);
    if (ret < 0){
        printk("Failed at USB1_f_Read_All\n");
        return;
    }







    printk("STEP%d\n", ++i);
    ret = USB1_f_Delete_Content(data, "./Languages/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Delete_Content\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Remove_File(data, "./Languages/data.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Remove_File\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Remove_Dir(data, "./Languages");
    if (ret < 0){
        printk("Failed at USB1_f_Remove_Dir\n");
        return;
    }




    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_Dir(data, "./Languages_new");
    if (ret < 0){
        printk("Failed at USB1_f_Make_Dir\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Make_File(data, "./Languages_new/data_new.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Make_File\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./Languages_new/data_new.txt", "Lanaguages NEW new are funny hahaha!!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Read_File(data, "./Languages_new/data_new.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Read_File\n");
    }


    printk("STEP%d\n", ++i);
    ret = USB1_f_Delete_Content(data, "./Countries/China.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Delete_Content\n");
        return;
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./Countries/China.txt", "CHINA is a northen country of VietNam!");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Delete_Content(data, "./.Hidden_item/coffee.txt");
    if (ret < 0){
        printk("Failed at USB1_f_Delete_Content\n");
        return;
    }
    printk("STEP%d\n", ++i);
    ret = USB1_f_Add_Content(data, "./.Hidden_item/coffee.txt", "Sorry, coffee is from everywhere..");
    if (ret < 0){
        printk("Failed at USB1_f_Add_Content\n");
    }

    printk("STEP%d\n", ++i);
    ret = USB1_f_Read_All(data);
    if (ret < 0){
        printk("Failed at USB1_f_Read_All\n");
        return;
    }
}

/* Descriptor work */
static void descriptor_work_func(struct work_struct *work){
    struct usb_device_data *data = container_of(work, struct usb_device_data, descriptor_work);
    int ret;
    u8 ep_out, ep_in;

    msleep(1000);
    /* Reset */
    USB1_Reset_Speed(data);

    /* Getting descriptor */
    ret = USB1_GetDesc_Transfer(data);
    if (ret < 0) {
        printk("Fail USB1_GetDesc_Transfer\n");
    }
    else {
#if (PRINT_DESCRIPTOR)
        USB1_Print_DeviceDescriptor(data);
        USB1_Print_DeviceDescriptor2(data);
        USB1_Print_DeviceDescriptorIf0(data);
#endif

        // Setup FIFO, config once
        ep_out = data->InsDeviceDescriptor.ep_bulk_out_bEndpointAddress & 0x0F;
        ep_in = data->InsDeviceDescriptor.ep_bulk_in_bEndpointAddress & 0x0F;
        USB1_SetupBulkFifos(data, ep_out, ep_in);

        WRITE_ONCE(data->done_configured, true);

        ret = USB1_CBW(data);
        if (ret < 0) {
            printk("Fail USB1_CBW\n");
        }

#if (PRINT_TEST_KERNEL_FAT32)
        if (ret == 0) schedule_work(&data->re_request_work);
#endif
    }
    
}

static void descriptor_clear_work_func(struct work_struct *work){
    struct usb_device_data *data = container_of(work, struct usb_device_data, descriptor_clear_work);

    WRITE_ONCE(data->done_configured, false);
    memset(&data->InsDeviceDescriptor, 0, sizeof(data->InsDeviceDescriptor));
}

static void requested_work(struct work_struct *work){
#if (PRINT_TEST_KERNEL_FAT32)
    struct usb_device_data *data = container_of(work, struct usb_device_data, re_request_work);
    int ret;


    ret = USB1_f_Mount(data);
    if (ret < 0){
        printk("Failed at USB1_f_Mount\n");
        return;
    }

    /* Put the func/test for handling files/folders here */
    if (ret == 0){
        // Test_phase_1(data);
        // Test_phase_2(data);
        // Test_phase_3(data);
        // Test_phase_4(data);

        
        Test_phase_5(data);
    }

/*
    Output:
        |-[ ] 'Languages_new'
           |---- 'data_new.txt'
              |-[.] 'Lanaguages NEW new are funny hahaha!!'
        |-[ ] 'Countries'
           |---- 'VietNam.txt'
              |-[.] 'VietNam is a great country!!'
           |---- 'China.txt'
              |-[.] 'CHINA is a northen country of VietNam!'


*/
#endif
}

static int usb1_open(struct inode *inode, struct file *file){
    struct usb_device_data *data = container_of(inode->i_cdev, struct usb_device_data, cdev);
    file->private_data = data;
    return 0;
}

static long usb1_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct usb_device_data *data = file->private_data;
    int ret;
    struct msc_command cmd1;

    if (!data)
        return -ENODEV;

    switch (cmd) {
        case BBB_MSC_CONNECTION:
            struct msc_status status;
            status.connected  = data->done_configured;
            status.speed_state = data->speed_state;

            if (copy_to_user((void __user *)arg,
                            &status,
                            sizeof(status)))
                return -EFAULT;

            return 0;

        case BBB_MSC_INFO:
            struct msc_info info;
            info.bcdUSB         = data->InsDeviceDescriptor.bcdUSB;
            info.idVendor       = data->InsDeviceDescriptor.idVendor;
            info.idProduct      = data->InsDeviceDescriptor.idProduct;
            info.bcdDevice      = data->InsDeviceDescriptor.bcdDevice;
            info.iManufacturer  = data->InsDeviceDescriptor.iManufacturer;
            info.iProduct       = data->InsDeviceDescriptor.iProduct;
            info.iSerialNumber  = data->InsDeviceDescriptor.iSerialNumber;

            if (copy_to_user((void __user *)arg,
                            &info,
                            sizeof(info)))
                return -EFAULT;

            return 0;

        case BBB_MSC_MOUNT:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }

            ret = USB1_f_Mount(data);

            return ret;

        case BBB_MSC_READ_ALL:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Read_All(data);

            return ret;

        case BBB_MSC_READ_FILE:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            //printk("cmd.string = '%s'\n", cmd.string);
            ret = USB1_f_Read_File(data, cmd1.string);

            return ret;

        case BBB_MSC_READ_DIR:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            //printk("cmd.string = '%s'\n", cmd.string);
            ret = USB1_f_Read_Dir(data, cmd1.string);

            return ret;

        case BBB_MSC_ADD_CONTENT:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Add_Content(data, cmd1.string, cmd1.content);

            return ret;

        case BBB_MSC_DELETE_CONTENT:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            //printk("cmd.string = '%s'\n", cmd.string);
            ret = USB1_f_Delete_Content(data, cmd1.string);

            return ret;


        case BBB_MSC_MAKE_FILE:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Make_File(data, cmd1.string);

            return ret;

        case BBB_MSC_REMV_FILE:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Remove_File(data, cmd1.string);

            return ret;



        case BBB_MSC_MAKE_DIR:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Make_Dir(data, cmd1.string);

            return ret;

        case BBB_MSC_REMV_DIR:
            if (copy_from_user(&cmd1,
                            (struct msc_command __user *)arg,
                            sizeof(cmd1))) {
                return -EFAULT;
            }
            ret = USB1_f_Remove_Dir(data, cmd1.string);

            return ret;

        default:
            printk("UNDEFINED cmd\n");
            return -ENOTTY;
    }
}

static const struct file_operations usb_device_fops = {
    .owner =            THIS_MODULE,
    .open =             usb1_open,
    .unlocked_ioctl =   usb1_ioctl,
};

static int usb1_probe(struct platform_device *pdev)
{
    struct usb_device_data *data;
    int ret;

    //dev_info(&pdev->dev, "Probed\n");
    printk("probed\n");

    data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    platform_set_drvdata(pdev, data);

    data->dev = &pdev->dev;
    data->base_usbss = ioremap(BASE_USBSS, 0x1000);
    data->base_usb1ctl = ioremap(BASE_USB1CTL, 0x200);
    data->base_usb1phy = ioremap(BASE_USB1PHY, 0x100);
    data->base_usb1core = ioremap(BASE_USB1CORE, 0x400);
#if (!INDEX_USED)
    data->base_usb1ep0 = ioremap(USB1EP0_base, 0x10); // 16 bytes
#else
    data->base_usb1ep0 = ioremap(BASE_USB1CORE + 0x10, 0x10); // 16 bytes
#endif
    data->base_con_usb1ctrl1 = ioremap(CONTROL_MODULE, 0x1000);

    /* ===== Clock setup (assuming this part is unchanged) */
    data->clk = devm_clk_get(&pdev->dev, "fck-usb1");
    if (IS_ERR(data->clk)) {
        dev_err(&pdev->dev, "Failed to get clock: %ld\n", PTR_ERR(data->clk));
        return PTR_ERR(data->clk);
    }
    ret = clk_prepare_enable(data->clk);
    if (ret) {
        dev_err(&pdev->dev, "Failed to enable clock: %d\n", ret);
        return ret;
    }
    dev_info(&pdev->dev, "can0 clock rate: %lu Hz\n", clk_get_rate(data->clk));

    // ========== Request IRQ (hwirq 19 maps to swirq x on AM33xx) ==========
    data->irq1 = platform_get_irq(pdev, 0);
    if (data->irq1 < 0) {
        dev_err(&pdev->dev, "Failed Formatted: Unable to get IRQ: %d\n", data->irq1);
        return data->irq1;
    }

    //ret = request_irq(data->irq1, USB1_handler, IRQF_SHARED, "usb1", data);
    ret = devm_request_irq(&pdev->dev, data->irq1, USB1_handler, 0, "usb1", data);
    if (ret < 0) {
        dev_err(&pdev->dev, "Unable to request IRQ %d: %d\n", data->irq1, ret);
        return ret;
    }
    dev_info(&pdev->dev, "IRQ1 num = %d\n", data->irq1);

    // ===== Create character device
    ret = alloc_chrdev_region(&data->dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to allocate chrdev region: %d\n", ret);
        return ret;
    }

    cdev_init(&data->cdev, &usb_device_fops);
    data->cdev.owner = THIS_MODULE;
    ret = cdev_add(&data->cdev, data->dev_num, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to add cdev: %d\n", ret);
        //unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base_usbss);
        iounmap(data->base_usb1ctl);
        iounmap(data->base_usb1phy);
        iounmap(data->base_usb1core);
        return ret;
    }

    data->class = class_create(THIS_MODULE, "usb1_class");
    if (IS_ERR(data->class)) {
        dev_err(&pdev->dev, "Failed to create class: %ld\n", PTR_ERR(data->class));
        cdev_del(&data->cdev);
        //unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base_usbss);
        iounmap(data->base_usb1ctl);
        iounmap(data->base_usb1phy);
        iounmap(data->base_usb1core);
        return PTR_ERR(data->class);
    }

    data->dev = device_create(data->class, &pdev->dev, data->dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(data->dev)) {
        dev_err(&pdev->dev, "Failed to create device: %ld\n", PTR_ERR(data->dev));
        class_destroy(data->class);
        cdev_del(&data->cdev);
        //unregister_chrdev_region(&data->dev_num, 1);
        iounmap(data->base_usbss);
        iounmap(data->base_usb1ctl);
        iounmap(data->base_usb1phy);
        iounmap(data->base_usb1core);
        return PTR_ERR(data->dev);
    }

    dev_info(&pdev->dev, "Created /dev/%s\n", DEVICE_NAME);

    data->InsDeviceDescriptor.ep_bulk_in_bEndpointAddress = 0xff;

    // ===== USB1 init
    USB1_reset(data);
    PHY1_init(data);
    USB1_init(data);
    data->isEnd = 0;
    data->RX_index = 0;
    data->usb1_cbw.dCBWTag = 0; // initialize tag 
    data->done_mounted = false;


    INIT_WORK(&data->re_request_work, requested_work);
    atomic_set(&data->should_stop, 0); // Initialize to 0 (false)

    INIT_WORK(&data->descriptor_work, descriptor_work_func);
    INIT_WORK(&data->descriptor_clear_work, descriptor_clear_work_func);

    init_waitqueue_head(&data->irq_wait);

    return 0;
}

static int usb1_remove(struct platform_device *pdev)
{
    struct usb_device_data *data = platform_get_drvdata(pdev);

    atomic_set(&data->should_stop, 1); // Set to 1 (true)
    smp_mb(); // Memory barrier to ensure should_stop is visible

    cancel_work_sync(&data->re_request_work);

    cancel_work_sync(&data->descriptor_work);
    cancel_work_sync(&data->descriptor_clear_work);

    /* Mark the device unavailable first. */
    WRITE_ONCE(data->done_configured, false);
    wake_up_interruptible(&data->irq_wait);

    USB1_exit(data);

    iounmap(data->base_usbss);
    iounmap(data->base_usb1ctl);
    iounmap(data->base_usb1phy);
    iounmap(data->base_usb1core);
    iounmap(data->base_usb1ep0);
    iounmap(data->base_con_usb1ctrl1);

    if (data->dev)
        device_destroy(data->class, data->dev_num);
    if (data->class)
        class_destroy(data->class);
    cdev_del(&data->cdev);

    return 0;
}

static const struct of_device_id usb_device_of_match[] = {
    { .compatible = "usb1-based" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, usb_device_of_match);

static struct platform_driver usb_device_driver = {
    .probe = usb1_probe,
    .remove = usb1_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = usb_device_of_match,
    },
};

module_platform_driver(usb_device_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Soviet");
MODULE_DESCRIPTION("Custom USB1 Device Driver for BeagleBone Black USB1");

