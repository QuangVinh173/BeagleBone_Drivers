#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>
#include <atomic>
#include <net/if.h>
#include <sys/ioctl.h>

class NetApp {
private:
    int sock = -1;
    int client_sock = -1;
    std::string interface;
    std::atomic<bool> running{false};

public:
    ~NetApp() { closeAll(); }

    bool setInterface(const std::string& iface);
    bool printInterfaceInfo();
    bool startServer(int port = 12345);
    bool connectToServer(const std::string& ip, int port = 12345);
    
    ssize_t sendData(const std::string& data);
    ssize_t receiveData(char* buffer, size_t max_len);

    void startChatMode();
    void closeAll();
};

bool NetApp::setInterface(const std::string& iface) {
    interface = iface;
    std::cout << "Interface set to: " << interface << std::endl;
    return true;
}

bool NetApp::printInterfaceInfo() {
    if (interface.empty()) {
        std::cout << "No interface selected.\n";
        return false;
    }

    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    strncpy(ifr.ifr_name, interface.c_str(), IFNAMSIZ);

    std::cout << "=== " << interface << " Info ===\n";
    
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        std::cout << "Status : " << ((ifr.ifr_flags & IFF_UP) ? "UP" : "DOWN") << std::endl;
    }

    // Get IP
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, ip, sizeof(ip));
        std::cout << "IP     : " << ip << std::endl;
    }
    close(s);
    return true;
}

bool NetApp::startServer(int port) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "Socket creation failed\n";
        return false;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;        // Listen on all interfaces

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed\n";
        return false;
    }

    if (listen(sock, 1) < 0) {
        std::cerr << "Listen failed\n";
        return false;
    }

    std::cout << "Server listening on " << interface << " port " << port << "...\n";
    
    struct sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    client_sock = accept(sock, (struct sockaddr*)&client_addr, &len);

    if (client_sock < 0) return false;

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    std::cout << "Client connected: " << client_ip << "\n";
    return true;
}

bool NetApp::connectToServer(const std::string& ip, int port) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    std::cout << "Connecting to " << ip << " ..." << std::endl;
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Connection failed\n";
        return false;
    }
    std::cout << "Connected successfully!\n";
    return true;
}

ssize_t NetApp::sendData(const std::string& data) {
    std::string msg = data + "\n";
    int fd = (client_sock > 0) ? client_sock : sock;
    return send(fd, msg.c_str(), msg.length(), 0);
}

ssize_t NetApp::receiveData(char* buffer, size_t max_len) {
    int fd = (client_sock > 0) ? client_sock : sock;
    return recv(fd, buffer, max_len - 1, 0);
}

void NetApp::startChatMode() {
    running = true;
    char buffer[1024];

    std::thread receiver([this, &buffer]() {
        while (running) {
            ssize_t bytes = receiveData(buffer, sizeof(buffer));
            if (bytes > 0) {
                buffer[bytes] = '\0';
                std::cout << "\n[RX] " << buffer;
            } else if (bytes <= 0) {
                std::cout << "\nConnection closed.\n";
                running = false;
                break;
            }
        }
    });

    std::string input;
    while (running) {
        std::getline(std::cin, input);
        if (input == "quit" || input == "exit") {
            running = false;
            break;
        }
        if (!input.empty()) {
            sendData(input);
        }
    }

    if (receiver.joinable()) receiver.join();
}

void NetApp::closeAll() {
    running = false;
    if (client_sock > 0) close(client_sock);
    if (sock > 0) close(sock);
}

int main() {
    NetApp net;
    std::string iface, mode, ip;
    int port = 12345;

    std::cout << "=== USB0 / eth0 / wlan0 Network Tool (PC <-> BBB) ===\n\n";

    std::cout << "Enter interface (usb0 / eth0 / wlan0): ";
    std::getline(std::cin, iface);
    net.setInterface(iface);

    net.printInterfaceInfo();

    std::cout << "\nChoose mode:\n1. Server\n2. Client\n> ";
    std::cin >> mode;
    std::cin.ignore();

    if (mode == "1" || mode == "server") {
        net.startServer(port);
    } else {
        std::cout << "Enter remote IP: ";
        std::getline(std::cin, ip);
        net.connectToServer(ip, port);
    }

    std::cout << "\n=== Chat Mode Started (type 'quit' to exit) ===\n";
    net.startChatMode();

    return 0;
}
