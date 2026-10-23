#include "usb_hid.h"

#include <cstdint>
#include <cstring>
#include <iostream>


int main()
{
    HIDUSB usb;

    std::cout << "====================================\n"
              << "          BBB USB HID APP\n"
              << "====================================\n";

    if (!usb.openDevice())
        return 1;

    while (true) {
        std::cout << "\n"
                  << "1. Check connection\n"
                  << "2. Check info\n"
                  << "3. Wait for mouse neutral report\n"
                  << "4. Capture movement\n"
                  << "5. Stop capturing movement\n"
                  << "0. Exit\n"
                  << "Select option: ";

        std::string choice;
        if (!std::getline(std::cin, choice))
            break;

        if (choice == "1") {
            usb.checkConnection();
        } else if (choice == "2") {
            usb.checkInfo();
        } else if (choice == "3") {
            usb.readMouseNeutralOption();
        } else if (choice == "4") {
            usb.CaptureOption();
        } else if (choice == "5") {
            usb.NoCaptureOption();

        } else if (choice == "0") {
            break;
        } else {
            std::cout << "Invalid option.\n";
        }
    }

    std::cout << "USB HID application closed.\n";
    return 0;
}

