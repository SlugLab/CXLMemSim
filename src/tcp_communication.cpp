#include "../include/tcp_communication.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// Reliable send: keeps sending until all bytes are transmitted
int TCPConnection::send_all(int fd, const void* buf, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }
            return -1;
        }
        if (sent == 0) return -1;
        ptr += sent;
        remaining -= sent;
    }
    return 0;
}

// Reliable recv: keeps receiving until all bytes are read
int TCPConnection::recv_all(int fd, void* buf, size_t len) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t received = ::recv(fd, ptr, remaining, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }
            return -1;
        }
        if (received == 0) return -1; // Connection closed
        ptr += received;
        remaining -= received;
    }
    return 0;
}

TCPConnection::TCPConnection()
    : sock_fd_(-1), running_(false), connected_(false) {
}

TCPConnection::~TCPConnection() {
    disconnect();
}

int TCPConnection::send_message(const TCPMessage& msg) {
    if (!connected_) return -1;
    return send_all(sock_fd_, &msg, sizeof(msg));
}

int TCPConnection::receive_message(TCPMessage& msg) {
    if (!connected_) return -1;
    return recv_all(sock_fd_, &msg, sizeof(msg));
}

void TCPConnection::disconnect() {
    connected_ = false;
    running_ = false;
    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
}

// ---- TCPServer ----

TCPServer::TCPServer(const std::string& addr, uint16_t port)
    : bind_addr_(addr), port_(port), listen_fd_(-1), client_fd_(-1) {
}

TCPServer::~TCPServer() {
    stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

int TCPServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "Failed to create TCP server socket" << std::endl;
        return -1;
    }

    // SO_REUSEADDR
    int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set SO_REUSEADDR" << std::endl;
    }

    // TCP_NODELAY on listen socket (inherited by accepted sockets on some systems)
    if (::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set TCP_NODELAY on listen socket" << std::endl;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr);
    }

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind TCP address: " << strerror(errno) << std::endl;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    if (::listen(listen_fd_, 10) < 0) {
        std::cerr << "Failed to listen on TCP socket: " << strerror(errno) << std::endl;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return -1;
    }

    running_ = true;
    std::cout << "TCP server listening on " << bind_addr_ << ":" << port_ << std::endl;
    return 0;
}

int TCPServer::accept_connection() {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    client_fd_ = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd_ < 0) {
        if (running_) {
            std::cerr << "Failed to accept connection: " << strerror(errno) << std::endl;
        }
        return -1;
    }

    // TCP_NODELAY on accepted client fd
    int opt = 1;
    if (::setsockopt(client_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set TCP_NODELAY on client fd" << std::endl;
    }

    sock_fd_ = client_fd_;
    connected_ = true;
    std::cout << "TCP connection established" << std::endl;
    return 0;
}

void TCPServer::handle_client() {
    while (running_ && connected_) {
        TCPMessage recv_msg, send_msg;

        if (receive_message(recv_msg) < 0) {
            break;
        }

        if (message_handler_) {
            message_handler_(recv_msg, send_msg);
        } else {
            send_msg.response.status = 0;
            send_msg.response.latency_ns = 100;
        }

        if (send_message(send_msg) < 0) {
            break;
        }
    }

    connected_ = false;
}

void TCPServer::stop() {
    running_ = false;
    disconnect();
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

// ---- TCPClient ----

TCPClient::TCPClient(const std::string& addr, uint16_t port)
    : server_addr_(addr), server_port_(port) {
}

TCPClient::~TCPClient() {
    disconnect();
}

int TCPClient::connect() {
    sock_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        std::cerr << "Failed to create TCP client socket" << std::endl;
        return -1;
    }

    // TCP_NODELAY
    int opt = 1;
    if (::setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        std::cerr << "Failed to set TCP_NODELAY" << std::endl;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port_);
    inet_pton(AF_INET, server_addr_.c_str(), &addr.sin_addr);

    // Connect with timeout using non-blocking + poll
    // Set non-blocking
    int flags = fcntl(sock_fd_, F_GETFL, 0);
    fcntl(sock_fd_, F_SETFL, flags | O_NONBLOCK);

    int ret = ::connect(sock_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        std::cerr << "Failed to connect to " << server_addr_ << ":" << server_port_
                  << ": " << strerror(errno) << std::endl;
        ::close(sock_fd_);
        sock_fd_ = -1;
        return -1;
    }

    if (ret < 0) {
        // Wait for connection with timeout (2 seconds)
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock_fd_, &wfds);
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        ret = ::select(sock_fd_ + 1, nullptr, &wfds, nullptr, &tv);
        if (ret <= 0) {
            std::cerr << "Connection to " << server_addr_ << ":" << server_port_
                      << " timed out" << std::endl;
            ::close(sock_fd_);
            sock_fd_ = -1;
            return -1;
        }

        // Check for connection errors
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(sock_fd_, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            std::cerr << "Connection failed: " << strerror(err) << std::endl;
            ::close(sock_fd_);
            sock_fd_ = -1;
            return -1;
        }
    }

    // Set back to blocking
    fcntl(sock_fd_, F_SETFL, flags);

    connected_ = true;
    running_ = true;
    std::cout << "TCP client connected to " << server_addr_ << ":" << server_port_ << std::endl;

    return 0;
}

int TCPClient::send_request(const TCPRequest& req, TCPResponse& resp) {
    if (!connected_) return -1;

    TCPMessage msg;
    msg.request = req;
    memset(&msg.response, 0, sizeof(msg.response));

    if (send_message(msg) < 0) {
        return -1;
    }

    if (receive_message(msg) < 0) {
        return -1;
    }

    resp = msg.response;
    return 0;
}
