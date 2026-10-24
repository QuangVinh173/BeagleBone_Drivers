# BeagleBone Black USB Projects

This repository contains three USB-host projects for the **BeagleBone Black (BBB)**. Each project uses a custom Linux USB1 kernel driver and a userspace application to test a different USB device class: **CDC**, **HID**, or **Mass Storage (MSC)**.

## Project Structure

```text
09_USB/
├── README.md
├── 01_cdc/
│   └── README.md
├── 02_hid/
│   └── README.md
└── 03_msc/
    ├── README.md
    └── images/
```

See each project README for its setup steps, menu options, screenshots, and specific test instructions:

- **[01_cdc — USB CDC](01_cdc/README.md)**
- **[02_hid — USB HID](02_hid/README.md)**
- **[03_msc — USB Mass Storage](03_msc/README.md)**

## Project Overview

| Project | USB device under test | Main purpose | Userspace application |
|---|---|---|---|
| `01_cdc` | Arduino configured as a USB CDC device | Check connection and device information; test write-only, read, and write/read communication | `./cdc_usb_app` |
| `02_hid` | USB HID mouse | Check connection and device information; capture and display mouse reports | `./main` |
| `03_msc` | USB flash drive with a FAT32 volume | Check connection and device information; test FAT32 file and directory operations | `./b.sh` |

The applications and menu options are project-specific. Refer to the corresponding README before running a test.

## Common Setup

1. Connect the device for the project you want to test to the BBB USB host port used by the custom USB1 driver.
2. Configure and build the corresponding custom USB1 kernel driver from the kernel project.
3. Load the driver and check that the device node `/dev/usb1` exists.
4. Build the userspace application from that project directory:

   ```bash
   make
   ```

5. Confirm that your user has permission to access `/dev/usb1`; use `sudo` where required.
6. Follow the project's README for any extra device or firmware setup, then run its application.

## Project-Specific Notes

### USB CDC — `01_cdc`

The BBB acts as the USB host and an Arduino acts as the CDC device. The application tests communication commands: `[write]`, `[read]`, and `[writeread]`. The Arduino responder firmware must use the same command prefixes and newline-terminated messages.

### USB HID — `02_hid`

The BBB acts as the USB host for a USB HID mouse. The application can check connection/device information, start or stop mouse-report capture, and wait for a neutral mouse report. Start capture before expecting report data in the report-viewing option.

### USB Mass Storage — `03_msc`

The BBB acts as the USB host for a USB flash drive. The application checks device status and information and runs a FAT32 test that creates or updates files and directories. **Back up the drive before running the FAT32 example**, because it writes to the filesystem. The example output and available files depend on the drive's current contents.

## Troubleshooting

- If a device is not detected, check the physical connection, driver state, `/dev/usb1`, and kernel log.
- If the application cannot open `/dev/usb1`, check device-node permissions and confirm that the kernel driver created it.
- If a test does not behave as expected, verify the device-specific setup and follow the troubleshooting notes in that project's README.
- The three projects share the `/dev/usb1` device-node name; use the driver and userspace application built for the project being tested.

## Detailed Documentation

- [USB CDC guide](01_cdc/README.md)
- [USB HID guide](02_hid/README.md)
- [USB MSC / FAT32 guide](03_msc/README.md)
