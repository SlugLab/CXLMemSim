#ifndef TCP_COMMUNICATION_H
#define TCP_COMMUNICATION_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#define TCP_BUFFER_SIZE 4096
#define TCP_CACHELINE_SIZE 64

enum TCPOpType { TCP_OP_READ = 0, TCP_OP_WRITE = 1, TCP_OP_READ_RESP = 2, TCP_OP_WRITE_RESP = 3 };

struct TCPRequest {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint8_t host_id;
    uint64_t auth_token;
    uint64_t virtual_addr;
    uint8_t data[TCP_CACHELINE_SIZE];
} __attribute__((packed));

// Returns a FNV-1a hash of CXL_CLUSTER_SECRET env var, or 0 if unset.
static inline uint64_t tcp_get_auth_token() {
    const char *secret = std::getenv("CXL_CLUSTER_SECRET");
    if (!secret || secret[0] == '\0')
        return 0;
    uint64_t hash = 14695981039346656037ULL;
    for (const char *p = secret; *p; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct TCPResponse {
    uint8_t status;
    uint64_t latency_ns;
    uint8_t cache_state;
    uint8_t data[TCP_CACHELINE_SIZE];
} __attribute__((packed));

struct TCPMessage {
    TCPRequest request;
    TCPResponse response;
} __attribute__((packed));

class TCPConnection {
public:
    using MessageHandler = std::function<void(const TCPMessage &, TCPMessage &)>;

protected:
    int sock_fd_;
    MessageHandler message_handler_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;

    // Reliable send/recv helpers for fixed-size messages
    static int send_all(int fd, const void *buf, size_t len);
    static int recv_all(int fd, void *buf, size_t len);

public:
    TCPConnection();
    virtual ~TCPConnection();

    void set_message_handler(MessageHandler handler) { message_handler_ = handler; }
    int send_message(const TCPMessage &msg);
    int receive_message(TCPMessage &msg);
    bool is_connected() const { return connected_.load(); }
    void disconnect();
};

class TCPServer : public TCPConnection {
private:
    std::string bind_addr_;
    uint16_t port_;
    int listen_fd_;
    int client_fd_;

public:
    TCPServer(const std::string &addr, uint16_t port);
    ~TCPServer();

    int start();
    int accept_connection();
    void handle_client();
    void stop();
};

class TCPClient : public TCPConnection {
private:
    std::string server_addr_;
    uint16_t server_port_;

public:
    TCPClient(const std::string &addr, uint16_t port);
    ~TCPClient();

    int connect();
    int send_request(const TCPRequest &req, TCPResponse &resp);
};

class TCPTransport {
public:
    enum Mode { MODE_TCP, MODE_SHM, MODE_RDMA };

    static Mode get_transport_mode() {
        const char *mode = std::getenv("CXL_TRANSPORT_MODE");
        if (mode) {
            if (std::string(mode) == "tcp")
                return MODE_TCP;
            if (std::string(mode) == "shm")
                return MODE_SHM;
            if (std::string(mode) == "rdma")
                return MODE_RDMA;
        }
        return MODE_TCP;
    }

    static bool is_tcp_available() {
        return true; // TCP is always available
    }
};

#endif // TCP_COMMUNICATION_H
