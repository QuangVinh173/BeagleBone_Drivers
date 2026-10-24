#include "usb_msc.h"

#include <cstdint>
#include <cstring>
#include <iostream>


int main()
{
    MSCUSB usb;

    std::cout << "====================================\n"
              << "          BBB USB MSC APP\n"
              << "====================================\n";

    if (!usb.openDevice())
        return 1;

    while (true) {
        std::cout << "\n"
                  << "1. Check connection\n"
                  << "2. Check info\n"
                  << "3. Run FAT32 example\n"
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
            usb.handleFilesFolders();
        } else if (choice == "0") {
            break;
        } else {
            std::cout << "Invalid option.\n";
        }
    }

    std::cout << "USB MSC application closed.\n";
    return 0;
}

