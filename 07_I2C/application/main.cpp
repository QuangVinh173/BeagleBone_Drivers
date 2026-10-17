#include "i2c.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

/* ---------------- DS3231 definitions ---------------- */
static constexpr uint8_t DS3231_ADDR   = 0x68;
static constexpr uint8_t REG_SECONDS   = 0x00;
static constexpr uint8_t REG_MINUTES   = 0x01;
static constexpr uint8_t REG_HOURS     = 0x02;
static constexpr uint8_t HOUR_12H_MODE = 0x40;
static constexpr uint8_t HOUR_PM       = 0x20;

/* Set by SIGINT handler when Ctrl+C is pressed. */
static volatile std::sig_atomic_t stopMonitoring = 0;

static void signalHandler(int signal)
{
    if (signal == SIGINT)
        stopMonitoring = 1;
}

/* ---------------- Helpers ---------------- */
static uint8_t toBcd(int value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

static int fromBcd(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}

static std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

static void printTime(int hour, int minute, int second)
{
    std::cout << std::setfill('0')
              << std::setw(2) << hour << ':'
              << std::setw(2) << minute << ':'
              << std::setw(2) << second
              << std::setfill(' ');
}

/* Write one byte to one DS3231 register. */
static bool writeReg(I2c& i2c, uint8_t reg, uint8_t value)
{
    return i2c.write(&value, DS3231_ADDR, reg, 1) == 1;
}

/* Read one byte from one DS3231 register. */
static bool readReg(I2c& i2c, uint8_t reg, uint8_t& value)
{
    return i2c.read(&value, DS3231_ADDR, reg, 1) == 1;
}

/* ---------------- Commands ---------------- */
static bool setTime(I2c& i2c)
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};

    if (localtime_r(&now, &local) == nullptr) {
        std::cerr << "Failed to get host time!\n";
        return false;
    }

    const bool ok =
        writeReg(i2c, REG_SECONDS, toBcd(local.tm_sec)) &&
        writeReg(i2c, REG_MINUTES, toBcd(local.tm_min)) &&
        writeReg(i2c, REG_HOURS, toBcd(local.tm_hour));

    if (!ok) {
        std::cerr << "Failed to set time!\n";
        return false;
    }

    std::cout << "Time set to ";
    printTime(local.tm_hour, local.tm_min, local.tm_sec);
    std::cout << '\n';
    return true;
}

/* Read and display the DS3231 time once. */
static bool readTime(I2c& i2c)
{
    uint8_t raw[3] = {};

    /* Try a 3-byte burst read, then fall back to individual registers. */
    const ssize_t n = i2c.read(raw, DS3231_ADDR, REG_SECONDS, sizeof(raw));

    if (n != static_cast<ssize_t>(sizeof(raw))) {
        if (!readReg(i2c, REG_SECONDS, raw[0]) ||
            !readReg(i2c, REG_MINUTES, raw[1]) ||
            !readReg(i2c, REG_HOURS, raw[2])) {
            std::cerr << "Failed to read time!\n";
            return false;
        }
    }

    const int second = fromBcd(raw[0] & 0x7F);
    const int minute = fromBcd(raw[1] & 0x7F);
    int hour;

    if (raw[2] & HOUR_12H_MODE) {
        hour = fromBcd(raw[2] & 0x1F);
        const bool pm = (raw[2] & HOUR_PM) != 0;

        if (hour == 12)
            hour = pm ? 12 : 0;
        else if (pm)
            hour += 12;
    } else {
        hour = fromBcd(raw[2] & 0x3F);
    }

    std::cout << "DS3231 time: ";
    printTime(hour, minute, second);
    std::cout << '\n';
    return true;
}

/* Read the DS3231 repeatedly, approximately once per second. */
static void monitorTime(I2c& i2c)
{
    stopMonitoring = 0;

    std::cout << "\nMonitoring DS3231 time every 1 second.\n"
              << "Press Ctrl+C to stop monitoring.\n\n";

    while (!stopMonitoring) {
        if (!readTime(i2c)) {
            std::cerr << "Failed to read DS3231 time; stopping monitor.\n";
            break;
        }

        /* Check the stop flag every 100 ms while waiting between reads. */
        for (int i = 0; i < 10 && !stopMonitoring; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nMonitoring stopped.\n\n";
}

static void printHelp()
{
    std::cout << "Commands:\n"
              << "  set      - Write current host time to DS3231\n"
              << "  read     - Display DS3231 time once\n"
              << "  monitor  - Display DS3231 time every 1 second\n"
              << "  help     - Display this help message\n"
              << "  quit     - Exit\n\n";
}

/* ---------------- Main ---------------- */
int main()
{
    std::cout << "=== BeagleBone DS3231 RTC Application ===\n\n";

    I2c i2c1("/dev/i2c1");
    I2c i2c = std::move(i2c1);

    struct bbb_i2c_config config = {};
    config.speed = 100; /* kHz: 100 = standard, 400 = fast */
    config.slave_addr = DS3231_ADDR;
    config.use_dma = true;

    if (!i2c.init(&config)) {
        std::cerr << "Failed to initialize I2C!\n";
        return 1;
    }

    std::cout << "I2C initialized at " << config.speed << " kHz\n\n";

    std::signal(SIGINT, signalHandler);
    printHelp();

    std::string input;

    while (true) {
        std::cout << "> " << std::flush;

        if (!std::getline(std::cin, input))
            break; /* EOF / Ctrl+D */

        const std::string command = toLower(input);

        if (command == "quit" || command == "exit") {
            break;
        } else if (command == "set") {
            setTime(i2c);
        } else if (command == "read") {
            readTime(i2c);
        } else if (command == "monitor") {
            monitorTime(i2c);
        } else if (command == "help") {
            printHelp();
        } else if (!command.empty()) {
            std::cout << "Unknown command. Type 'help' for available commands.\n";
        }
    }

    std::cout << "\nClosing I2C...\n";
    return 0;
}
