#include "usb_cdc.h"

#include <iostream>
#include <string>

int main()
{
    CDCUSB usb;

    std::cout << "====================================\n"
              << "       BBB USB CDC-ACM APP\n"
              << "====================================\n";

    if (!usb.openDevice())
        return 1;

    while (true) {
        std::cout << "\n"
                  << "1. Check connection\n"
                  << "2. Check info\n"
                  << "3. Write only ([write])\n"
                  << "4. Read repeatedly ([read])\n"
                  << "5. Write and read once ([writeread])\n"
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
            usb.writeOption();
        } else if (choice == "4") {
            usb.readOption();
        } else if (choice == "5") {
            usb.writeReadOption();
        } else if (choice == "0") {
            break;
        } else {
            std::cout << "Invalid option.\n";
        }
    }

    std::cout << "USB application closed.\n";
    return 0;
}

