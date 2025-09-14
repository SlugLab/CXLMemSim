#include "../include/rdma_communication.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <unistd.h>

RDMAConnection::RDMAConnection()
    : cm_id(nullptr), event_channel(nullptr), running(false) {
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.connected = false;
}

RDMAConnection::~RDMAConnection() {
    disconnect();
    cleanup_resources();
}

int RDMAConnection::setup_connection_resources() {
    conn_info.pd = ibv_alloc_pd(conn_info.context);
    if (!conn_info.pd) {
        std::cerr << "Failed to allocate protection domain" << std::endl;
        return -1;
    }

    conn_info.comp_channel = ibv_create_comp_channel(conn_info.context);
    if (!conn_info.comp_channel) {
        std::cerr << "Failed to create completion channel" << std::endl;
        return -1;
    }

    conn_info.send_cq = ibv_create_cq(conn_info.context, RDMA_CQ_SIZE,
                                      nullptr, conn_info.comp_channel, 0);
    if (!conn_info.send_cq) {
        std::cerr << "Failed to create send CQ" << std::endl;
        return -1;
    }

    conn_info.recv_cq = ibv_create_cq(conn_info.context, RDMA_CQ_SIZE,
                                      nullptr, conn_info.comp_channel, 0);
    if (!conn_info.recv_cq) {
        std::cerr << "Failed to create receive CQ" << std::endl;
        return -1;
    }

    if (ibv_req_notify_cq(conn_info.recv_cq, 0)) {
        std::cerr << "Failed to request CQ notification" << std::endl;
        return -1;
    }

    return register_memory_region();
}

int RDMAConnection::register_memory_region() {
    conn_info.buffer_size = RDMA_BUFFER_SIZE * sizeof(RDMAMessage);
    conn_info.buffer = malloc(conn_info.buffer_size);
    if (!conn_info.buffer) {
        std::cerr << "Failed to allocate buffer" << std::endl;
        return -1;
    }

    memset(conn_info.buffer, 0, conn_info.buffer_size);

    conn_info.mr = ibv_reg_mr(conn_info.pd, conn_info.buffer, conn_info.buffer_size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                              IBV_ACCESS_REMOTE_READ);
    if (!conn_info.mr) {
        std::cerr << "Failed to register memory region" << std::endl;
        free(conn_info.buffer);
        return -1;
    }

    return 0;
}

int RDMAConnection::setup_qp_parameters(struct ibv_qp_init_attr& qp_attr) {
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = conn_info.send_cq;
    qp_attr.recv_cq = conn_info.recv_cq;
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
    wr.wr_id = reinterpret_cast<uintptr_t>(conn_info.buffer);
    wr.sg_list = &sge;
    wr.num_sge = 1;

    sge.addr = reinterpret_cast<uintptr_t>(conn_info.buffer);
    sge.length = sizeof(RDMAMessage);
    sge.lkey = conn_info.mr->lkey;

    if (ibv_post_recv(conn_info.qp, &wr, &bad_wr)) {
        std::cerr << "Failed to post receive" << std::endl;
        return -1;
    }

    return 0;
}

int RDMAConnection::post_send(const RDMAMessage* msg) {
    struct ibv_send_wr wr, *bad_wr = nullptr;
    struct ibv_sge sge;

    memcpy(conn_info.buffer, msg, sizeof(RDMAMessage));

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = reinterpret_cast<uintptr_t>(conn_info.buffer);
    wr.opcode = IBV_WR_SEND;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;

    sge.addr = reinterpret_cast<uintptr_t>(conn_info.buffer);
    sge.length = sizeof(RDMAMessage);
    sge.lkey = conn_info.mr->lkey;

    if (ibv_post_send(conn_info.qp, &wr, &bad_wr)) {
        std::cerr << "Failed to post send" << std::endl;
        return -1;
    }

    struct ibv_wc wc;
    int ne;
    do {
        ne = ibv_poll_cq(conn_info.send_cq, 1, &wc);
    } while (ne == 0);

    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        std::cerr << "Send failed with status: " << wc.status << std::endl;
        return -1;
    }

    return 0;
}

int RDMAConnection::send_message(const RDMAMessage& msg) {
    if (!conn_info.connected) {
        return -1;
    }
    return post_send(&msg);
}

int RDMAConnection::receive_message(RDMAMessage& msg) {
    if (!conn_info.connected) {
        return -1;
    }

    struct ibv_wc wc;
    int ne;

    do {
        ne = ibv_poll_cq(conn_info.recv_cq, 1, &wc);
    } while (ne == 0);

    if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
        std::cerr << "Receive failed with status: " << wc.status << std::endl;
        return -1;
    }

    memcpy(&msg, reinterpret_cast<void*>(wc.wr_id), sizeof(RDMAMessage));

    return post_receive();
}

void RDMAConnection::disconnect() {
    conn_info.connected = false;
    running = false;

    if (cm_id) {
        rdma_disconnect(cm_id);
    }
}

void RDMAConnection::cleanup_resources() {
    if (conn_info.qp) {
        ibv_destroy_qp(conn_info.qp);
        conn_info.qp = nullptr;
    }

    if (conn_info.recv_cq) {
        ibv_destroy_cq(conn_info.recv_cq);
        conn_info.recv_cq = nullptr;
    }

    if (conn_info.send_cq) {
        ibv_destroy_cq(conn_info.send_cq);
        conn_info.send_cq = nullptr;
    }

    if (conn_info.comp_channel) {
        ibv_destroy_comp_channel(conn_info.comp_channel);
        conn_info.comp_channel = nullptr;
    }

    if (conn_info.mr) {
        ibv_dereg_mr(conn_info.mr);
        conn_info.mr = nullptr;
    }

    if (conn_info.buffer) {
        free(conn_info.buffer);
        conn_info.buffer = nullptr;
    }

    if (conn_info.pd) {
        ibv_dealloc_pd(conn_info.pd);
        conn_info.pd = nullptr;
    }

    if (cm_id) {
        rdma_destroy_id(cm_id);
        cm_id = nullptr;
    }

    if (event_channel) {
        rdma_destroy_event_channel(event_channel);
        event_channel = nullptr;
    }
}

// RDMAServer Implementation
RDMAServer::RDMAServer(const std::string& addr, uint16_t port)
    : bind_addr(addr), port(port), listen_id(nullptr) {
}

RDMAServer::~RDMAServer() {
    stop();
    if (listen_id) {
        rdma_destroy_id(listen_id);
    }
}

int RDMAServer::start() {
    struct sockaddr_in addr;

    event_channel = rdma_create_event_channel();
    if (!event_channel) {
        std::cerr << "Failed to create event channel" << std::endl;
        return -1;
    }

    if (rdma_create_id(event_channel, &listen_id, nullptr, RDMA_PS_TCP)) {
        std::cerr << "Failed to create RDMA ID" << std::endl;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr == "0.0.0.0" || bind_addr.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr);
    }

    if (rdma_bind_addr(listen_id, reinterpret_cast<struct sockaddr*>(&addr))) {
        std::cerr << "Failed to bind RDMA address" << std::endl;
        return -1;
    }

    if (rdma_listen(listen_id, 10)) {
        std::cerr << "Failed to listen on RDMA" << std::endl;
        return -1;
    }

    running = true;
    std::cout << "RDMA server listening on " << bind_addr << ":" << port << std::endl;
    return 0;
}

int RDMAServer::accept_connection() {
    struct rdma_cm_event* event = nullptr;

    if (rdma_get_cm_event(event_channel, &event)) {
        std::cerr << "Failed to get CM event" << std::endl;
        return -1;
    }

    if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
        cm_id = event->id;
        conn_info.context = cm_id->verbs;

        if (setup_connection_resources() < 0) {
            rdma_ack_cm_event(event);
            return -1;
        }

        struct ibv_qp_init_attr qp_attr;
        setup_qp_parameters(qp_attr);

        if (rdma_create_qp(cm_id, conn_info.pd, &qp_attr)) {
            std::cerr << "Failed to create QP" << std::endl;
            rdma_ack_cm_event(event);
            return -1;
        }

        conn_info.qp = cm_id->qp;

        for (int i = 0; i < RDMA_MAX_WR; i++) {
            if (post_receive() < 0) {
                break;
            }
        }

        struct rdma_conn_param conn_param;
        memset(&conn_param, 0, sizeof(conn_param));
        conn_param.initiator_depth = 1;
        conn_param.responder_resources = 1;

        if (rdma_accept(cm_id, &conn_param)) {
            std::cerr << "Failed to accept connection" << std::endl;
            rdma_ack_cm_event(event);
            return -1;
        }
    }

    rdma_ack_cm_event(event);

    if (rdma_get_cm_event(event_channel, &event)) {
        return -1;
    }

    if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
        conn_info.connected = true;
        std::cout << "RDMA connection established" << std::endl;
    }

    rdma_ack_cm_event(event);
    return 0;
}

void RDMAServer::handle_client() {
    while (running && conn_info.connected) {
        RDMAMessage recv_msg, send_msg;

        if (receive_message(recv_msg) < 0) {
            break;
        }

        if (message_handler) {
            message_handler(recv_msg, send_msg);
        } else {
            send_msg.response.status = 0;
            send_msg.response.latency_ns = 100;
        }

        if (send_message(send_msg) < 0) {
            break;
        }
    }

    conn_info.connected = false;
}

void RDMAServer::stop() {
    running = false;
    disconnect();
}

// RDMAClient Implementation
RDMAClient::RDMAClient(const std::string& addr, uint16_t port)
    : server_addr(addr), server_port(port) {
}

RDMAClient::~RDMAClient() {
    disconnect();
}

int RDMAClient::connect() {
    struct sockaddr_in addr;

    event_channel = rdma_create_event_channel();
    if (!event_channel) {
        std::cerr << "Failed to create event channel" << std::endl;
        return -1;
    }

    if (rdma_create_id(event_channel, &cm_id, nullptr, RDMA_PS_TCP)) {
        std::cerr << "Failed to create RDMA ID" << std::endl;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_addr.c_str(), &addr.sin_addr);

    if (rdma_resolve_addr(cm_id, nullptr,
                         reinterpret_cast<struct sockaddr*>(&addr), 2000)) {
        std::cerr << "Failed to resolve RDMA address" << std::endl;
        return -1;
    }

    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(event_channel, &event)) {
        std::cerr << "Failed to get CM event" << std::endl;
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        std::cerr << "Address resolution failed" << std::endl;
        rdma_ack_cm_event(event);
        return -1;
    }

    rdma_ack_cm_event(event);

    conn_info.context = cm_id->verbs;
    if (setup_connection_resources() < 0) {
        return -1;
    }

    struct ibv_qp_init_attr qp_attr;
    setup_qp_parameters(qp_attr);

    if (rdma_create_qp(cm_id, conn_info.pd, &qp_attr)) {
        std::cerr << "Failed to create QP" << std::endl;
        return -1;
    }

    conn_info.qp = cm_id->qp;

    for (int i = 0; i < RDMA_MAX_WR; i++) {
        if (post_receive() < 0) {
            break;
        }
    }

    if (rdma_resolve_route(cm_id, 2000)) {
        std::cerr << "Failed to resolve route" << std::endl;
        return -1;
    }

    if (rdma_get_cm_event(event_channel, &event)) {
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

    if (rdma_connect(cm_id, &conn_param)) {
        std::cerr << "Failed to connect" << std::endl;
        return -1;
    }

    if (rdma_get_cm_event(event_channel, &event)) {
        return -1;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        std::cerr << "Connection failed" << std::endl;
        rdma_ack_cm_event(event);
        return -1;
    }

    rdma_ack_cm_event(event);

    conn_info.connected = true;
    running = true;
    std::cout << "RDMA client connected to " << server_addr << ":" << server_port << std::endl;

    return 0;
}

int RDMAClient::send_request(const RDMARequest& req, RDMAResponse& resp) {
    if (!conn_info.connected) {
        return -1;
    }

    RDMAMessage msg;
    msg.request = req;

    if (send_message(msg) < 0) {
        return -1;
    }

    if (receive_message(msg) < 0) {
        return -1;
    }

    resp = msg.response;
    return 0;
}