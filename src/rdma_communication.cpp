#include "../include/rdma_communication.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>

RDMAConnection::RDMAConnection()
    : running_(false), connected_(false) {
#ifdef HAS_RDMA
    cm_id_ = nullptr;
    event_channel_ = nullptr;
    memset(&conn_info_, 0, sizeof(conn_info_));
    conn_info_.connected = false;
#endif
}

RDMAConnection::~RDMAConnection() {
    disconnect();
#ifdef HAS_RDMA
    cleanup_resources();
#endif
}

#ifdef HAS_RDMA

int RDMAConnection::setup_connection_resources() {
    conn_info_.pd = ibv_alloc_pd(conn_info_.context);
    if (!conn_info_.pd) {
        std::cerr << "Failed to allocate protection domain" << std::endl;
        return -1;
    }

    conn_info_.comp_channel = ibv_create_comp_channel(conn_info_.context);
    if (!conn_info_.comp_channel) {
        std::cerr << "Failed to create completion channel" << std::endl;
        return -1;
    }

    conn_info_.send_cq = ibv_create_cq(conn_info_.context, RDMA_CQ_SIZE,
                                       nullptr, conn_info_.comp_channel, 0);
    if (!conn_info_.send_cq) {
        std::cerr << "Failed to create send CQ" << std::endl;
        return -1;
    }

    conn_info_.recv_cq = ibv_create_cq(conn_info_.context, RDMA_CQ_SIZE,
                                       nullptr, conn_info_.comp_channel, 0);
    if (!conn_info_.recv_cq) {
        std::cerr << "Failed to create receive CQ" << std::endl;
        return -1;
    }

    if (ibv_req_notify_cq(conn_info_.recv_cq, 0)) {
        std::cerr << "Failed to request CQ notification" << std::endl;
        return -1;
    }

    return register_memory_region();
}

int RDMAConnection::register_memory_region() {
    conn_info_.buffer_size = RDMA_BUFFER_SIZE * sizeof(RDMAMessage);
    conn_info_.buffer = malloc(conn_info_.buffer_size);
    if (!conn_info_.buffer) {
        std::cerr << "Failed to allocate buffer" << std::endl;
        return -1;
    }

    memset(conn_info_.buffer, 0, conn_info_.buffer_size);

    conn_info_.mr = ibv_reg_mr(conn_info_.pd, conn_info_.buffer, conn_info_.buffer_size,
                               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_READ);
    if (!conn_info_.mr) {
        std::cerr << "Failed to register memory region" << std::endl;
        free(conn_info_.buffer);
        return -1;
    }

    return 0;
}

int RDMAConnection::setup_qp_parameters(struct ibv_qp_init_attr& qp_attr) {
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = conn_info_.send_cq;
    qp_attr.recv_cq = conn_info_.recv_cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.sq_sig_all = 1;

    qp_attr.cap.max_send_wr = RDMA_MAX_WR;
    qp_attr.cap.max_recv_wr = RDMA_MAX_WR;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 256;

    return 0;
}

int RDMAConnection::post_receive() {
    struct ibv_recv_wr wr, *bad_wr = nullptr;
    struct ibv_sge sge;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = reinterpret_cast<uintptr_t>(conn_info_.buffer);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    sge.addr = reinterpret_cast<uintptr_t>(conn_info_.buffer);
    sge.length = sizeof(RDMAMessage);
    sge.lkey = conn_info_.mr->lkey;

    if (ibv_post_recv(conn_info_.qp, &wr, &bad_wr)) {
        std::cerr << "Failed to post receive" << std::endl;
        return -1;
    }

    return 0;
}

int RDMAConnection::post_send(const RDMAMessage* msg) {
    struct ibv_send_wr wr, *bad_wr = nullptr;
    struct ibv_sge sge;

    memcpy(conn_info_.buffer, msg, sizeof(RDMAMessage));

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = reinterpret_cast<uintptr_t>(conn_info_.buffer);
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = reinterpret_cast<uintptr_t>(conn_info_.buffer);
    sge.length = sizeof(RDMAMessage);
    sge.lkey = conn_info_.mr->lkey;

    if (ibv_post_send(conn_info_.qp, &wr, &bad_wr)) {
        std::cerr << "Failed to post send" << std::endl;
        return -1;
    }

    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(conn_info_.send_cq, 1, &wc);
    } while (ne == 0);

    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        std::cerr << "Send failed with status: " << wc.status << std::endl;
        return -1;
    }

    return 0;
}

void RDMAConnection::cleanup_resources() {
    if (conn_info_.qp) {
        ibv_destroy_qp(conn_info_.qp);
        conn_info_.qp = nullptr;
    }

    if (conn_info_.recv_cq) {
        ibv_destroy_cq(conn_info_.recv_cq);
        conn_info_.recv_cq = nullptr;
    }

    if (conn_info_.send_cq) {
        ibv_destroy_cq(conn_info_.send_cq);
        conn_info_.send_cq = nullptr;
    }

    if (conn_info_.comp_channel) {
        ibv_destroy_comp_channel(conn_info_.comp_channel);
        conn_info_.comp_channel = nullptr;
    }

    if (conn_info_.mr) {
        ibv_dereg_mr(conn_info_.mr);
        conn_info_.mr = nullptr;
    }

    if (conn_info_.buffer) {
        free(conn_info_.buffer);
        conn_info_.buffer = nullptr;
    }

    if (conn_info_.pd) {
        ibv_dealloc_pd(conn_info_.pd);
        conn_info_.pd = nullptr;
    }

    if (cm_id_) {
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
    }

    if (event_channel_) {
        rdma_destroy_event_channel(event_channel_);
        event_channel_ = nullptr;
    }
}

#endif // HAS_RDMA

int RDMAConnection::send_message(const RDMAMessage& msg) {
    if (!connected_) return -1;
#ifdef HAS_RDMA
    return post_send(&msg);
#else
    (void)msg;
    return -1;
#endif
}

int RDMAConnection::receive_message(RDMAMessage& msg) {
    if (!connected_) return -1;
#ifdef HAS_RDMA
    struct ibv_wc wc;
    int ne;

    do {
        ne = ibv_poll_cq(conn_info_.recv_cq, 1, &wc);
    } while (ne == 0);

    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        std::cerr << "Receive failed with status: " << wc.status << std::endl;
        return -1;
    }

    memcpy(&msg, reinterpret_cast<void*>(wc.wr_id), sizeof(RDMAMessage));

    return post_receive();
#else
    (void)msg;
    return -1;
#endif
}

void RDMAConnection::disconnect() {
    connected_ = false;
    running_ = false;
#ifdef HAS_RDMA
    if (cm_id_) {
        rdma_disconnect(cm_id_);
    }
#endif
}

// ---- RDMAServer ----

RDMAServer::RDMAServer(const std::string& addr, uint16_t port)
    : bind_addr_(addr), port_(port) {
#ifdef HAS_RDMA
    listen_id_ = nullptr;
#endif
}

RDMAServer::~RDMAServer() {
    stop();
#ifdef HAS_RDMA
    if (listen_id_) {
        rdma_destroy_id(listen_id_);
    }
#endif
}

int RDMAServer::start() {
#ifdef HAS_RDMA
    struct sockaddr_in addr;

    event_channel_ = rdma_create_event_channel();
    if (!event_channel_) {
        std::cerr << "Failed to create event channel" << std::endl;
        return -1;
    }

    if (rdma_create_id(event_channel_, &listen_id_, nullptr, RDMA_PS_TCP)) {
        std::cerr << "Failed to create RDMA ID" << std::endl;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr);
    }

    if (rdma_bind_addr(listen_id_, reinterpret_cast<struct sockaddr*>(&addr))) {
        std::cerr << "Failed to bind RDMA address" << std::endl;
        return -1;
    }

    if (rdma_listen(listen_id_, 10)) {
        std::cerr << "Failed to listen on RDMA" << std::endl;
        return -1;
    }

    running_ = true;
    std::cout << "RDMA server listening on " << bind_addr_ << ":" << port_ << std::endl;
    return 0;
#else
    std::cerr << "RDMA not available (compiled without HAS_RDMA)" << std::endl;
    return -1;
#endif
}

int RDMAServer::accept_connection() {
#ifdef HAS_RDMA
    struct rdma_cm_event* event = nullptr;

    if (rdma_get_cm_event(event_channel_, &event)) {
        std::cerr << "Failed to get CM event" << std::endl;
        return -1;
    }

    if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
        cm_id_ = event->id;
        conn_info_.context = cm_id_->verbs;

        if (setup_connection_resources() < 0) {
            rdma_ack_cm_event(event);
            return -1;
        }

        struct ibv_qp_init_attr qp_attr;
        setup_qp_parameters(qp_attr);

        if (rdma_create_qp(cm_id_, conn_info_.pd, &qp_attr)) {
            std::cerr << "Failed to create QP" << std::endl;
            rdma_ack_cm_event(event);
            return -1;
        }

        conn_info_.qp = cm_id_->qp;

        for (int i = 0; i < RDMA_MAX_WR; i++) {
            if (post_receive() < 0) {
                break;
            }
        }

        struct rdma_conn_param conn_param;
        memset(&conn_param, 0, sizeof(conn_param));
        conn_param.initiator_depth = 1;
        conn_param.responder_resources = 1;

        if (rdma_accept(cm_id_, &conn_param)) {
            std::cerr << "Failed to accept connection" << std::endl;
            rdma_ack_cm_event(event);
            return -1;
        }
    }

    rdma_ack_cm_event(event);

    if (rdma_get_cm_event(event_channel_, &event)) {
        return -1;
    }

    if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
        conn_info_.connected = true;
        connected_ = true;
        std::cout << "RDMA connection established" << std::endl;
    }

    rdma_ack_cm_event(event);
    return 0;
#else
    return -1;
#endif
}

void RDMAServer::handle_client() {
    while (running_ && connected_) {
        RDMAMessage recv_msg, send_msg;

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

void RDMAServer::stop() {
    running_ = false;
    disconnect();
}

// ---- RDMAClient ----

RDMAClient::RDMAClient(const std::string& addr, uint16_t port)
    : server_addr_(addr), server_port_(port) {
}

RDMAClient::~RDMAClient() {
    disconnect();
}

int RDMAClient::connect() {
#ifdef HAS_RDMA
    struct sockaddr_in addr;

    event_channel_ = rdma_create_event_channel();
    if (!event_channel_) {
        std::cerr << "Failed to create event channel" << std::endl;
        return -1;
    }

    if (rdma_create_id(event_channel_, &cm_id_, nullptr, RDMA_PS_TCP)) {
        std::cerr << "Failed to create RDMA ID" << std::endl;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port_);
    inet_pton(AF_INET, server_addr_.c_str(), &addr.sin_addr);

    if (rdma_resolve_addr(cm_id_, nullptr,
                         reinterpret_cast<struct sockaddr*>(&addr), 2000)) {
        std::cerr << "Failed to resolve RDMA address" << std::endl;
        return -1;
    }

    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(event_channel_, &event)) {
        std::cerr << "Failed to get CM event" << std::endl;
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        std::cerr << "Address resolution failed" << std::endl;
        rdma_ack_cm_event(event);
        return -1;
    }

    rdma_ack_cm_event(event);

    conn_info_.context = cm_id_->verbs;
    if (setup_connection_resources() < 0) {
        return -1;
    }

    struct ibv_qp_init_attr qp_attr;
    setup_qp_parameters(qp_attr);

    if (rdma_create_qp(cm_id_, conn_info_.pd, &qp_attr)) {
        std::cerr << "Failed to create QP" << std::endl;
        return -1;
    }

    conn_info_.qp = cm_id_->qp;

    for (int i = 0; i < RDMA_MAX_WR; i++) {
        if (post_receive() < 0) {
            break;
        }
    }

    if (rdma_resolve_route(cm_id_, 2000)) {
        std::cerr << "Failed to resolve route" << std::endl;
        return -1;
    }

    if (rdma_get_cm_event(event_channel_, &event)) {
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        std::cerr << "Route resolution failed" << std::endl;
        rdma_ack_cm_event(event);
        return -1;
    }

    rdma_ack_cm_event(event);

    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;

    if (rdma_connect(cm_id_, &conn_param)) {
        std::cerr << "Failed to connect" << std::endl;
        return -1;
    }

    if (rdma_get_cm_event(event_channel_, &event)) {
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        std::cerr << "Connection failed" << std::endl;
        rdma_ack_cm_event(event);
        return -1;
    }

    rdma_ack_cm_event(event);

    conn_info_.connected = true;
    connected_ = true;
    running_ = true;
    std::cout << "RDMA client connected to " << server_addr_ << ":" << server_port_ << std::endl;

    return 0;
#else
    std::cerr << "RDMA not available (compiled without HAS_RDMA)" << std::endl;
    return -1;
#endif
}

int RDMAClient::send_request(const RDMARequest& req, RDMAResponse& resp) {
    if (!connected_) return -1;

    RDMAMessage msg;
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
