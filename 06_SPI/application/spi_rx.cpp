#include "spi.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>

int main() {
    std::cout << "=== BeagleBone SPI Test Application ===\n\n";

    Spi spi("/dev/spi1");     

    // Initialize SPI 
    struct bbb_spi_config config = {
        /* follow master clk */
        .mode = SPI_MODE_0, /* match with master */
        .use_dma = false,
        .FIFO_trigger = 1,
    };

    if (!spi.init(&config)) {
        std::cerr << "Failed to initialize SPI!\n";
        return 1;
    }

    std::cout << "SPI initialized successfully!\n\n";

    std::string input;
    char buffer[256] = {0};

    std::cout << "Commands:\n";
    std::cout << "  recv             - Read once\n";
    std::cout << "  quit             - Exit\n\n";

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "quit" || input == "exit") {
            break;
        }
        else if (input == "recv") {
            ssize_t bytes = spi.read(buffer, sizeof(buffer) - 1);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                std::cout << "Received (" << bytes << " bytes): " << buffer << "\n";
            } else if (bytes == 0) {
                std::cout << "No data available.\n";
            } else {
                std::cerr << "Read error!\n";
            }
        }
        else if (input == "help") {
            std::cout << "Available commands: send <msg>, recv, loop, quit\n";
        }
        else {
            std::cout << "Unknown command. Type 'help' for available commands.\n";
        }
    }

    std::cout << "\nClosing SPI...\n";
    return 0;
}

