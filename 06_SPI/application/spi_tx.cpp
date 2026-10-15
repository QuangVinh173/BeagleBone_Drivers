#include "spi.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>

int main() {
    std::cout << "=== BeagleBone SPI Test Application ===\n\n";

    Spi spi("/dev/spi0");     

    // Initialize SPI 
    struct bbb_spi_config config = {
        .clk_divided = 6, /* Divide by 64 (2^6 = 64) -> clk =  48 000 000 / 64 = 750 000 Hz */
        .mode = SPI_MODE_0,
        .use_dma = true,
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
    std::cout << "  send <message>   - Send message to SPI\n";
    std::cout << "  quit             - Exit\n\n";

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "quit" || input == "exit") {
            break;
        }
        else if (input.substr(0, 5) == "send ") {
            std::string message = input.substr(5);
            if (!message.empty()) {
                // Add newline for better SPI communication
                message += "\n";
                ssize_t bytes = spi.write(message);
                if (bytes > 0) {
                    std::cout << "Sent " << bytes << " bytes: " << message;
                } else {
                    std::cerr << "Failed to send data!\n";
                }
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

