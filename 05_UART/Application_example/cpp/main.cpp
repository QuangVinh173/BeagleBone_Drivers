#include "uart_app.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>

int main() {
    std::cout << "=== BeagleBone UART Test Application ===\n\n";

    Uart uart1("/dev/uart1");        // Change to your UART device if needed

    // move
    Uart uart = std::move(uart1);

    // Initialize UART
    if (!uart.init()) {
        std::cerr << "Failed to initialize UART!\n";
        return 1;
    }

    std::cout << "UART initialized successfully!\n\n";

    std::string input;
    char buffer[256] = {0};

    std::cout << "Commands:\n";
    std::cout << "  send <message>   - Send message to UART\n";
    std::cout << "  recv             - Read once\n";
    std::cout << "  loop             - Continuous read mode\n";
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
                // Add newline for better UART communication
                message += "\n";
                ssize_t bytes = uart.write(message);
                if (bytes > 0) {
                    std::cout << "Sent " << bytes << " bytes: " << message;
                } else {
                    std::cerr << "Failed to send data!\n";
                }
            }
        }
        else if (input == "recv") {
            ssize_t bytes = uart.read(buffer, sizeof(buffer) - 1);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                std::cout << "Received (" << bytes << " bytes): " << buffer << "\n";
            } else if (bytes == 0) {
                std::cout << "No data available.\n";
            } else {
                std::cerr << "Read error!\n";
            }
        }
        else if (input == "loop") {
            std::cout << "Entering continuous read mode (Ctrl+C to exit)...\n";
            while (true) {
                ssize_t bytes = uart.readTimeout(buffer, sizeof(buffer) - 1, 1000);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    std::cout << "[RX] " << buffer;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        else if (input == "help") {
            std::cout << "Available commands: send <msg>, recv, loop, quit\n";
        }
        else {
            std::cout << "Unknown command. Type 'help' for available commands.\n";
        }
    }

    std::cout << "\nClosing UART...\n";
    return 0;
}
