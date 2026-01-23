/*
 * Distributed Multi-Memory Server Implementation
 * Provides inter-node communication for distributed CXL memory simulation
 * using shared memory message passing between nodes.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#include "../include/distributed_server.h"
#include "cxlcontroller.h"
#include "coherency_engine.h"
#include "hdm_decoder.h"
#include "shared_memory_manager.h"
#include <spdlog/spdlog.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <numeric>

/* ============================================================================
 * DistributedMessageManager Implementation
 * ============================================================================ */

DistributedMessageManager::DistributedMessageManager(const std::string& shm_name, uint32_t node_id)
    : shm_name_(shm_name), shm_fd_(-1), shm_header_(nullptr),
      local_node_id_(node_id), is_coordinator_(false),
      next_msg_id_(1), running_(false) {
}

DistributedMessageManager::~DistributedMessageManager() {
    cleanup();
}

bool DistributedMessageManager::initialize(bool create_new) {
    if (create_new) {
        // Remove existing shared memory
        shm_unlink(shm_name_.c_str());

        // Create new shared memory
        shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (shm_fd_ < 0) {
            SPDLOG_ERROR("Failed to create distributed SHM {}: {}", shm_name_, strerror(errno));
            return false;
        }

        // Set size
        if (ftruncate(shm_fd_, DIST_SHM_SIZE) < 0) {
            SPDLOG_ERROR("Failed to set distributed SHM size: {}", strerror(errno));
            close(shm_fd_);
            shm_unlink(shm_name_.c_str());
            return false;
        }

        SPDLOG_INFO("Created distributed SHM: {} ({} bytes)", shm_name_, DIST_SHM_SIZE);
    } else {
        // Open existing shared memory
        shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
        if (shm_fd_ < 0) {
            SPDLOG_ERROR("Failed to open distributed SHM {}: {}", shm_name_, strerror(errno));
            return false;
        }
    }

    // Map shared memory
    void* mapped = mmap(NULL, DIST_SHM_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd_, 0);
    if (mapped == MAP_FAILED) {
        SPDLOG_ERROR("Failed to mmap distributed SHM: {}", strerror(errno));
        close(shm_fd_);
        if (create_new) shm_unlink(shm_name_.c_str());
        return false;
    }

    shm_header_ = static_cast<dist_shm_header_t*>(mapped);

    if (create_new) {
        // Initialize header
        memset(shm_header_, 0, DIST_SHM_SIZE);
        shm_header_->magic = DIST_SHM_MAGIC;
        shm_header_->version = DIST_SHM_VERSION;
        shm_header_->num_nodes = 0;
        shm_header_->coordinator_node = local_node_id_;
        shm_header_->global_epoch = 0;
        shm_header_->system_ready = 0;
        shm_header_->shutdown_requested = 0;

        // Initialize all queues
        for (int i = 0; i < DIST_MAX_NODES * DIST_MAX_NODES; i++) {
            shm_header_->queues[i].head = 0;
            shm_header_->queues[i].tail = 0;
            shm_header_->queues[i].msg_count = 0;
            shm_header_->queues[i].capacity = DIST_MSG_QUEUE_SIZE;
            shm_header_->queues[i].total_sent = 0;
            shm_header_->queues[i].total_received = 0;
            shm_header_->queues[i].total_dropped = 0;
        }

        is_coordinator_ = true;
        SPDLOG_INFO("Initialized as coordinator node {}", local_node_id_);
    } else {
        // Validate existing header
        if (shm_header_->magic != DIST_SHM_MAGIC) {
            SPDLOG_ERROR("Invalid distributed SHM magic: 0x{:x}", shm_header_->magic);
            munmap(shm_header_, DIST_SHM_SIZE);
            close(shm_fd_);
            return false;
        }
        if (shm_header_->version != DIST_SHM_VERSION) {
            SPDLOG_ERROR("Incompatible distributed SHM version: {}", shm_header_->version);
            munmap(shm_header_, DIST_SHM_SIZE);
            close(shm_fd_);
            return false;
        }
        SPDLOG_INFO("Joined existing distributed SHM as node {}", local_node_id_);
    }

    return true;
}

void DistributedMessageManager::cleanup() {
    stop_processing();

    if (shm_header_) {
        munmap(shm_header_, DIST_SHM_SIZE);
        shm_header_ = nullptr;
    }

    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }

    if (is_coordinator_) {
        shm_unlink(shm_name_.c_str());
    }
}

bool DistributedMessageManager::register_node(const DistNodeInfo& info) {
    if (!shm_header_) return false;
    if (info.node_id >= DIST_MAX_NODES) return false;

    dist_node_status_t* status = &shm_header_->nodes[info.node_id];

    status->node_id = info.node_id;
    status->state = NODE_STATE_READY;
    status->last_heartbeat = std::chrono::steady_clock::now().time_since_epoch().count();
    status->memory_base = info.memory_base;
    status->memory_size = info.memory_size;
    status->active_connections = 0;
    status->flags = 0;
    strncpy(status->hostname, info.hostname.c_str(), sizeof(status->hostname) - 1);

    __atomic_fetch_add(&shm_header_->num_nodes, 1, __ATOMIC_SEQ_CST);

    SPDLOG_INFO("Registered node {}: {} (memory: 0x{:x}-0x{:x})",
                info.node_id, info.hostname, info.memory_base,
                info.memory_base + info.memory_size);

    return true;
}

bool DistributedMessageManager::deregister_node(uint32_t node_id) {
    if (!shm_header_ || node_id >= DIST_MAX_NODES) return false;

    dist_node_status_t* status = &shm_header_->nodes[node_id];
    status->state = NODE_STATE_OFFLINE;
    status->last_heartbeat = 0;

    __atomic_fetch_sub(&shm_header_->num_nodes, 1, __ATOMIC_SEQ_CST);

    SPDLOG_INFO("Deregistered node {}", node_id);
    return true;
}

bool DistributedMessageManager::is_node_active(uint32_t node_id) const {
    if (!shm_header_ || node_id >= DIST_MAX_NODES) return false;
    return shm_header_->nodes[node_id].state == NODE_STATE_READY ||
           shm_header_->nodes[node_id].state == NODE_STATE_BUSY;
}

std::vector<uint32_t> DistributedMessageManager::get_active_nodes() const {
    std::vector<uint32_t> active;
    if (!shm_header_) return active;

    for (uint32_t i = 0; i < DIST_MAX_NODES; i++) {
        if (is_node_active(i)) {
            active.push_back(i);
        }
    }
    return active;
}

bool DistributedMessageManager::enqueue_message(uint32_t dst_node, const dist_message_t& msg) {
    if (!shm_header_ || dst_node >= DIST_MAX_NODES) return false;

    uint32_t queue_idx = local_node_id_ * DIST_MAX_NODES + dst_node;
    dist_node_queue_t* queue = &shm_header_->queues[queue_idx];

    // Check if queue is full
    uint32_t head = __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&queue->tail, __ATOMIC_ACQUIRE);
    uint32_t next_head = (head + 1) % queue->capacity;

    if (next_head == tail) {
        // Queue full
        __atomic_fetch_add(&queue->total_dropped, 1, __ATOMIC_RELAXED);
        SPDLOG_WARN("Message queue to node {} full, dropping message", dst_node);
        return false;
    }

    // Copy message to queue
    memcpy(&queue->messages[head], &msg, sizeof(dist_message_t));
    __atomic_thread_fence(__ATOMIC_RELEASE);

    // Update head
    __atomic_store_n(&queue->head, next_head, __ATOMIC_RELEASE);
    __atomic_fetch_add(&queue->msg_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&queue->total_sent, 1, __ATOMIC_RELAXED);

    return true;
}

bool DistributedMessageManager::dequeue_message(uint32_t src_node, dist_message_t& msg) {
    if (!shm_header_ || src_node >= DIST_MAX_NODES) return false;

    uint32_t queue_idx = src_node * DIST_MAX_NODES + local_node_id_;
    dist_node_queue_t* queue = &shm_header_->queues[queue_idx];

    uint32_t head = __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&queue->tail, __ATOMIC_ACQUIRE);

    if (head == tail) {
        // Queue empty
        return false;
    }

    // Copy message from queue
    memcpy(&msg, &queue->messages[tail], sizeof(dist_message_t));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    // Update tail
    uint32_t next_tail = (tail + 1) % queue->capacity;
    __atomic_store_n(&queue->tail, next_tail, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&queue->msg_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&queue->total_received, 1, __ATOMIC_RELAXED);

    return true;
}

bool DistributedMessageManager::send_message(uint32_t dst_node, const dist_message_t& msg) {
    return enqueue_message(dst_node, msg);
}

bool DistributedMessageManager::send_message_wait_response(uint32_t dst_node,
                                                            const dist_message_t& req,
                                                            dist_message_t& resp,
                                                            int timeout_ms) {
    // Create pending request entry
    auto pending = std::make_shared<PendingRequest>();
    pending->msg_id = req.header.msg_id;
    pending->response = &resp;
    pending->completed = false;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[req.header.msg_id] = pending;
    }

    // Send request
    if (!send_message(dst_node, req)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(req.header.msg_id);
        return false;
    }

    // Wait for response
    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        if (!pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                   [&pending]() { return pending->completed; })) {
            // Timeout
            std::lock_guard<std::mutex> pending_lock(pending_mutex_);
            pending_requests_.erase(req.header.msg_id);
            SPDLOG_WARN("Request {} to node {} timed out", req.header.msg_id, dst_node);
            return false;
        }
    }

    // Clean up
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(req.header.msg_id);
    }

    return true;
}

bool DistributedMessageManager::broadcast_message(const dist_message_t& msg) {
    bool all_success = true;
    auto active_nodes = get_active_nodes();

    for (uint32_t node_id : active_nodes) {
        if (node_id != local_node_id_) {
            if (!send_message(node_id, msg)) {
                all_success = false;
            }
        }
    }

    return all_success;
}

void DistributedMessageManager::register_handler(dist_msg_type_t type, DistMessageHandler handler) {
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    handlers_[type] = std::move(handler);
}

void DistributedMessageManager::unregister_handler(dist_msg_type_t type) {
    std::unique_lock<std::shared_mutex> lock(handlers_mutex_);
    handlers_.erase(type);
}

void DistributedMessageManager::process_message(const dist_message_t& msg) {
    // Check if this is a response to a pending request
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(msg.header.msg_id);
        if (it != pending_requests_.end()) {
            auto pending = it->second;
            memcpy(pending->response, &msg, sizeof(dist_message_t));
            {
                std::lock_guard<std::mutex> pending_lock(pending->mutex);
                pending->completed = true;
            }
            pending->cv.notify_one();
            return;
        }
    }

    // Find and call handler
    DistMessageHandler handler;
    {
        std::shared_lock<std::shared_mutex> lock(handlers_mutex_);
        auto it = handlers_.find(static_cast<dist_msg_type_t>(msg.header.msg_type));
        if (it != handlers_.end()) {
            handler = it->second;
        }
    }

    if (handler) {
        dist_message_t response;
        memset(&response, 0, sizeof(response));
        response.header.msg_id = msg.header.msg_id;
        response.header.src_node_id = local_node_id_;
        response.header.dst_node_id = msg.header.src_node_id;
        response.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        handler(msg, response);

        // Send response if handler set a response type
        if (response.header.msg_type != DIST_MSG_NONE) {
            send_message(msg.header.src_node_id, response);
        }
    } else {
        SPDLOG_WARN("No handler for message type {}", msg.header.msg_type);
    }
}

void DistributedMessageManager::worker_thread() {
    while (running_) {
        int processed = poll_messages(100);
        if (processed == 0) {
            // No messages, sleep briefly
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

int DistributedMessageManager::poll_messages(int max_messages) {
    int processed = 0;

    // Poll messages from all nodes
    for (uint32_t src_node = 0; src_node < DIST_MAX_NODES && processed < max_messages; src_node++) {
        if (src_node == local_node_id_) continue;
        if (!is_node_active(src_node)) continue;

        dist_message_t msg;
        while (processed < max_messages && dequeue_message(src_node, msg)) {
            process_message(msg);
            processed++;
        }
    }

    return processed;
}

void DistributedMessageManager::send_heartbeat() {
    if (!shm_header_) return;

    // Update local node status
    dist_node_status_t* status = &shm_header_->nodes[local_node_id_];
    status->last_heartbeat = std::chrono::steady_clock::now().time_since_epoch().count();

    // Send heartbeat message to all active nodes
    dist_message_t hb_msg;
    memset(&hb_msg, 0, sizeof(hb_msg));
    hb_msg.header.msg_type = DIST_MSG_NODE_HEARTBEAT;
    hb_msg.header.msg_id = generate_msg_id();
    hb_msg.header.src_node_id = local_node_id_;
    hb_msg.header.timestamp = status->last_heartbeat;

    hb_msg.payload.node.node_id = local_node_id_;
    hb_msg.payload.node.node_state = status->state;
    hb_msg.payload.node.memory_base = status->memory_base;
    hb_msg.payload.node.memory_size = status->memory_size;

    broadcast_message(hb_msg);
}

void DistributedMessageManager::start_processing() {
    if (running_) return;

    running_ = true;

    // Start worker threads
    int num_workers = 2;
    for (int i = 0; i < num_workers; i++) {
        workers_.emplace_back(&DistributedMessageManager::worker_thread, this);
    }

    SPDLOG_INFO("Started {} message processing workers", num_workers);
}

void DistributedMessageManager::stop_processing() {
    running_ = false;

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

DistributedMessageManager::Stats DistributedMessageManager::get_stats() const {
    Stats stats = {0, 0, 0, 0};

    if (!shm_header_) return stats;

    // Sum stats from all queues
    for (int i = 0; i < DIST_MAX_NODES * DIST_MAX_NODES; i++) {
        stats.messages_sent += shm_header_->queues[i].total_sent;
        stats.messages_received += shm_header_->queues[i].total_received;
        stats.messages_dropped += shm_header_->queues[i].total_dropped;
    }

    return stats;
}

/* ============================================================================
 * DistributedDirectory removed - all coherency now via CoherencyEngine.
 * ============================================================================ */

/* (DistributedDirectory methods removed) */

/* (All DistributedDirectory method implementations removed) */

/* ============================================================================
 * DistributedMemoryServer Implementation
 * ============================================================================ */

DistributedMemoryServer::DistributedMemoryServer(uint32_t node_id, const std::string& shm_name,
                                                   int tcp_port, size_t capacity_mb,
                                                   CXLController* controller,
                                                   DistTransportMode transport_mode,
                                                   const std::string& rdma_addr,
                                                   uint16_t rdma_port)
    : node_id_(node_id), shm_name_(shm_name), tcp_port_(tcp_port),
      memory_capacity_mb_(capacity_mb), transport_mode_(transport_mode),
      rdma_addr_(rdma_addr), rdma_port_(rdma_port),
      controller_(controller),
      running_(false), state_(NODE_STATE_UNKNOWN),
      local_reads_(0), local_writes_(0), remote_reads_(0), remote_writes_(0),
      forwarded_requests_(0), coherency_messages_(0) {
}

DistributedMemoryServer::~DistributedMemoryServer() {
    stop();
}

bool DistributedMemoryServer::initialize() {
    state_ = NODE_STATE_INITIALIZING;

    // Initialize local memory manager
    std::string local_shm = shm_name_ + "_node" + std::to_string(node_id_);
    local_memory_ = std::make_unique<SharedMemoryManager>(memory_capacity_mb_, local_shm);
    if (!local_memory_->initialize()) {
        SPDLOG_ERROR("Failed to initialize local shared memory");
        return false;
    }

    auto shm_info = local_memory_->get_shm_info();

    // Initialize message manager
    msg_manager_ = std::make_unique<DistributedMessageManager>(shm_name_, node_id_);
    bool is_first_node = (node_id_ == 0);
    if (!msg_manager_->initialize(is_first_node)) {
        SPDLOG_ERROR("Failed to initialize message manager");
        return false;
    }

    // Register this node
    DistNodeInfo info;
    info.node_id = node_id_;
    info.hostname = "node" + std::to_string(node_id_);
    info.state = NODE_STATE_READY;
    info.memory_base = shm_info.base_addr;
    info.memory_size = shm_info.size;
    info.last_heartbeat = std::chrono::steady_clock::now().time_since_epoch().count();

    if (!msg_manager_->register_node(info)) {
        SPDLOG_ERROR("Failed to register node");
        return false;
    }

    // Setup message handlers
    setup_message_handlers();

    // Configure LogP model on the controller for distributed latency
    auto active_nodes = msg_manager_->get_active_nodes();
    uint32_t num_nodes = std::max(static_cast<uint32_t>(active_nodes.size()), 2u);
    LogPConfig logp_cfg(
        150.0,   // L: 150ns network latency (typical CXL switch hop)
        20.0,    // o_s: 20ns sender overhead
        20.0,    // o_r: 20ns receiver overhead
        4.0,     // g: 4ns gap (250MHz message rate)
        num_nodes
    );
    controller_->configure_logp(logp_cfg);

    // Configure controller for distributed mode with HDM decoder + CoherencyEngine
    HDMDecoderMode hdm_mode = (transport_mode_ == DistTransportMode::RDMA)
        ? HDMDecoderMode::INTERLEAVED : HDMDecoderMode::RANGE_BASED;
    controller_->configure_distributed(node_id_, hdm_mode);

    // Add local range to HDM decoder
    controller_->hdm_decoder_->add_range(shm_info.base_addr, shm_info.size, node_id_, false);

    // Initialize RDMA transport if requested
    if (transport_mode_ == DistTransportMode::RDMA || transport_mode_ == DistTransportMode::HYBRID) {
        if (!initialize_rdma_transport()) {
            if (transport_mode_ == DistTransportMode::RDMA) {
                SPDLOG_ERROR("Failed to initialize RDMA transport");
                return false;
            }
            // HYBRID mode: fall back to SHM only
            SPDLOG_WARN("RDMA initialization failed, falling back to SHM-only mode");
            transport_mode_ = DistTransportMode::SHM;
        }
    }

    state_ = NODE_STATE_READY;
    SPDLOG_INFO("Distributed node {} initialized: memory 0x{:x}-0x{:x} ({} MB), transport={}",
                node_id_, shm_info.base_addr, shm_info.base_addr + shm_info.size,
                memory_capacity_mb_,
                transport_mode_ == DistTransportMode::RDMA ? "RDMA" :
                transport_mode_ == DistTransportMode::HYBRID ? "HYBRID" : "SHM");

    return true;
}

bool DistributedMemoryServer::start() {
    if (running_) return true;
    if (state_ != NODE_STATE_READY) {
        SPDLOG_ERROR("Cannot start: node not ready");
        return false;
    }

    running_ = true;

    // Start message processing
    msg_manager_->start_processing();

    // Start heartbeat thread
    heartbeat_thread_ = std::thread(&DistributedMemoryServer::heartbeat_loop, this);

    // Start request processor
    request_processor_thread_ = std::thread(&DistributedMemoryServer::process_requests_loop, this);

    SPDLOG_INFO("Distributed node {} started", node_id_);
    return true;
}

void DistributedMemoryServer::stop() {
    if (!running_) return;

    running_ = false;
    state_ = NODE_STATE_DRAINING;

    // Wait for threads
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    if (request_processor_thread_.joinable()) {
        request_processor_thread_.join();
    }
    for (auto& t : client_threads_) {
        if (t.joinable()) t.join();
    }
    client_threads_.clear();

    // Stop message processing
    if (msg_manager_) {
        msg_manager_->stop_processing();
        msg_manager_->deregister_node(node_id_);
    }

    state_ = NODE_STATE_OFFLINE;
    SPDLOG_INFO("Distributed node {} stopped", node_id_);
}

bool DistributedMemoryServer::join_cluster(const std::string& coordinator_shm) {
    // For joining an existing cluster
    // Re-initialize message manager with the coordinator's shared memory
    msg_manager_ = std::make_unique<DistributedMessageManager>(coordinator_shm, node_id_);
    if (!msg_manager_->initialize(false)) {
        SPDLOG_ERROR("Failed to join cluster");
        return false;
    }

    auto shm_info = local_memory_->get_shm_info();
    DistNodeInfo info;
    info.node_id = node_id_;
    info.hostname = "node" + std::to_string(node_id_);
    info.state = NODE_STATE_READY;
    info.memory_base = shm_info.base_addr;
    info.memory_size = shm_info.size;

    if (!msg_manager_->register_node(info)) {
        SPDLOG_ERROR("Failed to register with cluster");
        return false;
    }

    // Notify coordinator
    dist_message_t reg_msg;
    memset(&reg_msg, 0, sizeof(reg_msg));
    reg_msg.header.msg_type = DIST_MSG_NODE_REGISTER;
    reg_msg.header.msg_id = msg_manager_->generate_msg_id();
    reg_msg.header.src_node_id = node_id_;
    reg_msg.header.dst_node_id = 0; // Coordinator
    reg_msg.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    reg_msg.payload.node = {
        .node_id = info.node_id,
        .node_state = static_cast<uint32_t>(info.state),
        .memory_base = info.memory_base,
        .memory_size = info.memory_size,
        .num_cachelines = info.memory_size / DIST_CACHELINE_SIZE,
        .port = static_cast<uint32_t>(tcp_port_),
        .flags = 0
    };
    strncpy(reg_msg.payload.node.hostname, info.hostname.c_str(),
            sizeof(reg_msg.payload.node.hostname) - 1);

    msg_manager_->send_message(0, reg_msg);

    SPDLOG_INFO("Node {} joined cluster", node_id_);
    return true;
}

bool DistributedMemoryServer::leave_cluster() {
    // Notify other nodes
    dist_message_t dereg_msg;
    memset(&dereg_msg, 0, sizeof(dereg_msg));
    dereg_msg.header.msg_type = DIST_MSG_NODE_DEREGISTER;
    dereg_msg.header.msg_id = msg_manager_->generate_msg_id();
    dereg_msg.header.src_node_id = node_id_;
    dereg_msg.header.dst_node_id = 0xFFFF; // Broadcast
    dereg_msg.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    dereg_msg.payload.node.node_id = node_id_;

    msg_manager_->broadcast_message(dereg_msg);
    msg_manager_->deregister_node(node_id_);

    SPDLOG_INFO("Node {} left cluster", node_id_);
    return true;
}

void DistributedMemoryServer::setup_message_handlers() {
    // Read request handler
    msg_manager_->register_handler(DIST_MSG_READ_REQ,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_read_request(req, resp);
        });

    // Write request handler
    msg_manager_->register_handler(DIST_MSG_WRITE_REQ,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_write_request(req, resp);
        });

    // Atomic FAA handler
    msg_manager_->register_handler(DIST_MSG_ATOMIC_FAA_REQ,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_atomic_request(req, resp);
        });

    // Atomic CAS handler
    msg_manager_->register_handler(DIST_MSG_ATOMIC_CAS_REQ,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_atomic_request(req, resp);
        });

    // Coherency handlers
    msg_manager_->register_handler(DIST_MSG_INVALIDATE,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_coherency_request(req, resp);
        });

    msg_manager_->register_handler(DIST_MSG_DOWNGRADE,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_coherency_request(req, resp);
        });

    // Node management handlers
    msg_manager_->register_handler(DIST_MSG_NODE_REGISTER,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_node_message(req, resp);
        });

    msg_manager_->register_handler(DIST_MSG_NODE_HEARTBEAT,
        [this](const dist_message_t& req, dist_message_t& resp) {
            handle_node_message(req, resp);
        });

    // Directory query handler (now handled by CoherencyEngine, reply with ACK)
    msg_manager_->register_handler(DIST_MSG_DIR_QUERY,
        [this](const dist_message_t& req, dist_message_t& resp) {
            resp.header.msg_type = DIST_MSG_DIR_RESPONSE;
            resp.payload.coherency.cacheline_addr = req.payload.coherency.cacheline_addr;
            // CoherencyEngine processes coherency internally; just acknowledge
        });
}

void DistributedMemoryServer::handle_read_request(const dist_message_t& req, dist_message_t& resp) {
    resp.header.msg_type = DIST_MSG_READ_RESP;

    uint64_t addr = req.payload.mem.addr;
    uint64_t size = req.payload.mem.size;
    uint32_t client = req.payload.mem.client_id;

    // Ensure coherency
    ensure_coherency_for_read(addr, client);

    // Read from local memory
    if (local_memory_->read_cacheline(addr, resp.payload.mem.data, size)) {
        resp.payload.mem.status = 0;

        // Calculate latency
        std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
        access_elem.push_back(std::make_tuple(addr, size));
        resp.payload.mem.latency_ns = controller_->calculate_latency(access_elem, controller_->dramlatency);

        local_reads_++;
    } else {
        resp.payload.mem.status = 1;
        SPDLOG_ERROR("Failed to read local address 0x{:x}", addr);
    }
}

void DistributedMemoryServer::handle_write_request(const dist_message_t& req, dist_message_t& resp) {
    resp.header.msg_type = DIST_MSG_WRITE_RESP;

    uint64_t addr = req.payload.mem.addr;
    uint64_t size = req.payload.mem.size;
    uint32_t client = req.payload.mem.client_id;

    // Ensure coherency (invalidate other copies)
    ensure_coherency_for_write(addr, client);

    // Write to local memory
    if (local_memory_->write_cacheline(addr, req.payload.mem.data, size)) {
        resp.payload.mem.status = 0;

        // Calculate latency
        std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
        access_elem.push_back(std::make_tuple(addr, size));
        resp.payload.mem.latency_ns = controller_->calculate_latency(access_elem, controller_->dramlatency);

        local_writes_++;
    } else {
        resp.payload.mem.status = 1;
        SPDLOG_ERROR("Failed to write local address 0x{:x}", addr);
    }
}

void DistributedMemoryServer::handle_atomic_request(const dist_message_t& req, dist_message_t& resp) {
    uint64_t addr = req.payload.mem.addr;

    // Ensure exclusive access
    ensure_coherency_for_write(addr, req.header.src_node_id);

    // Get pointer to data
    uint8_t* data_ptr = local_memory_->get_cacheline_data(addr & ~(DIST_CACHELINE_SIZE - 1));
    if (!data_ptr) {
        if (req.header.msg_type == DIST_MSG_ATOMIC_FAA_REQ) {
            resp.header.msg_type = DIST_MSG_ATOMIC_FAA_RESP;
        } else {
            resp.header.msg_type = DIST_MSG_ATOMIC_CAS_RESP;
        }
        resp.payload.mem.status = 1;
        return;
    }

    uint64_t offset = addr % DIST_CACHELINE_SIZE;
    uint64_t* ptr = reinterpret_cast<uint64_t*>(data_ptr + offset);

    if (req.header.msg_type == DIST_MSG_ATOMIC_FAA_REQ) {
        resp.header.msg_type = DIST_MSG_ATOMIC_FAA_RESP;
        uint64_t old = __atomic_fetch_add(ptr, req.payload.mem.value, __ATOMIC_SEQ_CST);
        memcpy(&resp.payload.mem.value, &old, sizeof(old));
        resp.payload.mem.status = 0;
    } else { // CAS
        resp.header.msg_type = DIST_MSG_ATOMIC_CAS_RESP;
        uint64_t expected = req.payload.mem.expected;
        __atomic_compare_exchange_n(ptr, &expected, req.payload.mem.value,
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        memcpy(&resp.payload.mem.value, &expected, sizeof(expected));
        resp.payload.mem.status = 0;
    }

    // Calculate latency with atomic overhead
    std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
    access_elem.push_back(std::make_tuple(addr, sizeof(uint64_t)));
    resp.payload.mem.latency_ns = controller_->calculate_latency(access_elem, controller_->dramlatency) + 20;
}

void DistributedMemoryServer::handle_coherency_request(const dist_message_t& req, dist_message_t& resp) {
    coherency_messages_++;

    uint64_t cacheline_addr = req.payload.coherency.cacheline_addr;

    if (req.header.msg_type == DIST_MSG_INVALIDATE) {
        resp.header.msg_type = DIST_MSG_INVALIDATE_ACK;

        // Delegate to CoherencyEngine
        if (controller_->coherency_) {
            controller_->coherency_->handle_remote_invalidate(
                cacheline_addr, req.header.src_node_id);
        }
        resp.payload.coherency.cacheline_addr = cacheline_addr;

    } else if (req.header.msg_type == DIST_MSG_DOWNGRADE) {
        resp.header.msg_type = DIST_MSG_DOWNGRADE_ACK;

        // Delegate to CoherencyEngine
        if (controller_->coherency_) {
            controller_->coherency_->handle_remote_downgrade(
                cacheline_addr, req.header.src_node_id);
        }
        resp.payload.coherency.cacheline_addr = cacheline_addr;
    }
}

void DistributedMemoryServer::handle_node_message(const dist_message_t& req, dist_message_t& resp) {
    if (req.header.msg_type == DIST_MSG_NODE_REGISTER) {
        // New node joined
        DistNodeInfo info;
        info.node_id = req.payload.node.node_id;
        info.hostname = req.payload.node.hostname;
        info.state = static_cast<node_state_t>(req.payload.node.node_state);
        info.memory_base = req.payload.node.memory_base;
        info.memory_size = req.payload.node.memory_size;

        {
            std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
            nodes_[info.node_id] = info;
        }

        resp.header.msg_type = DIST_MSG_NODE_ACK;
        SPDLOG_INFO("Node {} registered: {} (memory: 0x{:x}-0x{:x})",
                    info.node_id, info.hostname, info.memory_base,
                    info.memory_base + info.memory_size);

    } else if (req.header.msg_type == DIST_MSG_NODE_HEARTBEAT) {
        // Update node status
        uint32_t node_id = req.payload.node.node_id;

        std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end()) {
            it->second.last_heartbeat = req.header.timestamp;
            it->second.state = static_cast<node_state_t>(req.payload.node.node_state);
        }
        // No response needed for heartbeat
        resp.header.msg_type = DIST_MSG_NONE;
    }
}

void DistributedMemoryServer::heartbeat_loop() {
    while (running_) {
        msg_manager_->send_heartbeat();

        // Check for dead nodes
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        {
            std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
            for (auto& [id, info] : nodes_) {
                if (id == node_id_) continue;
                // Mark node as offline if no heartbeat for 10 seconds
                if (now - info.last_heartbeat > 10000000000ULL) {
                    if (info.state != NODE_STATE_OFFLINE) {
                        SPDLOG_WARN("Node {} appears to be offline", id);
                        info.state = NODE_STATE_OFFLINE;
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void DistributedMemoryServer::process_requests_loop() {
    // This thread handles any additional request processing
    while (running_) {
        // Main processing is done by message manager workers
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

uint32_t DistributedMemoryServer::get_node_for_address(uint64_t addr) const {
    if (controller_->hdm_decoder_) {
        return controller_->hdm_decoder_->get_home_node(addr);
    }
    // Fallback: assume local
    return node_id_;
}

bool DistributedMemoryServer::is_local_address(uint64_t addr) const {
    auto shm_info = local_memory_->get_shm_info();
    return addr >= shm_info.base_addr && addr < shm_info.base_addr + shm_info.size;
}

int DistributedMemoryServer::read(uint64_t addr, void* data, size_t size, uint64_t* latency_ns) {
    uint32_t target_node = get_node_for_address(addr);

    if (target_node == node_id_ || is_local_address(addr)) {
        // Local read
        ensure_coherency_for_read(addr, node_id_);

        if (!local_memory_->read_cacheline(addr, static_cast<uint8_t*>(data), size)) {
            return -1;
        }

        std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
        access_elem.push_back(std::make_tuple(addr, size));
        *latency_ns = controller_->calculate_latency(access_elem, controller_->dramlatency);

        local_reads_++;
        return 0;
    }

    // Remote read - dispatch based on transport mode
    if ((transport_mode_ == DistTransportMode::RDMA || transport_mode_ == DistTransportMode::HYBRID) &&
        rdma_transport_ && rdma_transport_->is_connected(target_node)) {
        return forward_read_rdma(target_node, addr, data, size, latency_ns);
    }
    return forward_read(target_node, addr, data, size, latency_ns);
}

int DistributedMemoryServer::write(uint64_t addr, const void* data, size_t size, uint64_t* latency_ns) {
    uint32_t target_node = get_node_for_address(addr);

    if (target_node == node_id_ || is_local_address(addr)) {
        // Local write
        ensure_coherency_for_write(addr, node_id_);

        if (!local_memory_->write_cacheline(addr, static_cast<const uint8_t*>(data), size)) {
            return -1;
        }

        std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
        access_elem.push_back(std::make_tuple(addr, size));
        *latency_ns = controller_->calculate_latency(access_elem, controller_->dramlatency);

        local_writes_++;
        return 0;
    }

    // Remote write - dispatch based on transport mode
    if ((transport_mode_ == DistTransportMode::RDMA || transport_mode_ == DistTransportMode::HYBRID) &&
        rdma_transport_ && rdma_transport_->is_connected(target_node)) {
        return forward_write_rdma(target_node, addr, data, size, latency_ns);
    }
    return forward_write(target_node, addr, data, size, latency_ns);
}

int DistributedMemoryServer::forward_read(uint32_t target_node, uint64_t addr, void* data,
                                          size_t size, uint64_t* latency_ns) {
    forwarded_requests_++;
    remote_reads_++;

    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_READ_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.size = size;
    req.payload.mem.client_id = node_id_;

    if (!msg_manager_->send_message_wait_response(target_node, req, resp)) {
        SPDLOG_ERROR("Forward read to node {} failed", target_node);
        return -1;
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    memcpy(data, resp.payload.mem.data, size);
    // Use LogP model from controller for network latency
    double logp_lat = controller_->calculate_logp_latency(node_id_, target_node,
                                                           req.header.timestamp);
    *latency_ns = resp.payload.mem.latency_ns + static_cast<uint64_t>(logp_lat);

    return 0;
}

int DistributedMemoryServer::forward_write(uint32_t target_node, uint64_t addr, const void* data,
                                           size_t size, uint64_t* latency_ns) {
    forwarded_requests_++;
    remote_writes_++;

    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_WRITE_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.size = size;
    req.payload.mem.client_id = node_id_;
    memcpy(req.payload.mem.data, data, size);

    if (!msg_manager_->send_message_wait_response(target_node, req, resp)) {
        SPDLOG_ERROR("Forward write to node {} failed", target_node);
        return -1;
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    // Use LogP model from controller for network latency
    double logp_lat_w = controller_->calculate_logp_latency(node_id_, target_node,
                                                             req.header.timestamp);
    *latency_ns = resp.payload.mem.latency_ns + static_cast<uint64_t>(logp_lat_w);

    return 0;
}

int DistributedMemoryServer::atomic_faa(uint64_t addr, uint64_t value, uint64_t* old_value) {
    uint32_t target_node = get_node_for_address(addr);

    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_ATOMIC_FAA_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.value = value;

    if (target_node == node_id_) {
        // Local atomic
        handle_atomic_request(req, resp);
    } else {
        if (!msg_manager_->send_message_wait_response(target_node, req, resp)) {
            return -1;
        }
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    memcpy(old_value, &resp.payload.mem.value, sizeof(*old_value));
    return 0;
}

int DistributedMemoryServer::atomic_cas(uint64_t addr, uint64_t expected, uint64_t desired,
                                        uint64_t* old_value) {
    uint32_t target_node = get_node_for_address(addr);

    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_ATOMIC_CAS_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.expected = expected;
    req.payload.mem.value = desired;

    if (target_node == node_id_) {
        // Local atomic
        handle_atomic_request(req, resp);
    } else {
        if (!msg_manager_->send_message_wait_response(target_node, req, resp)) {
            return -1;
        }
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    memcpy(old_value, &resp.payload.mem.value, sizeof(*old_value));
    return 0;
}

void DistributedMemoryServer::fence() {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // Broadcast fence to all active nodes
    dist_message_t fence_msg;
    memset(&fence_msg, 0, sizeof(fence_msg));
    fence_msg.header.msg_type = DIST_MSG_FENCE_REQ;
    fence_msg.header.msg_id = msg_manager_->generate_msg_id();
    fence_msg.header.src_node_id = node_id_;
    fence_msg.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

    msg_manager_->broadcast_message(fence_msg);
}

bool DistributedMemoryServer::ensure_coherency_for_read(uint64_t addr, uint32_t requesting_node) {
    if (controller_->coherency_) {
        uint64_t ts = std::chrono::steady_clock::now().time_since_epoch().count();
        CoherencyRequest req{addr, requesting_node, 0, false, ts};
        auto resp = controller_->coherency_->process_read(req);
        return resp.success;
    }
    return true;
}

bool DistributedMemoryServer::ensure_coherency_for_write(uint64_t addr, uint32_t requesting_node) {
    if (controller_->coherency_) {
        uint64_t ts = std::chrono::steady_clock::now().time_since_epoch().count();
        CoherencyRequest req{addr, requesting_node, 0, true, ts};
        auto resp = controller_->coherency_->process_write(req);
        return resp.success;
    }
    return true;
}

bool DistributedMemoryServer::add_remote_node(const DistNodeInfo& info) {
    std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
    nodes_[info.node_id] = info;
    return true;
}

bool DistributedMemoryServer::remove_remote_node(uint32_t node_id) {
    std::unique_lock<std::shared_mutex> lock(nodes_mutex_);
    nodes_.erase(node_id);
    return msg_manager_->deregister_node(node_id);
}

std::vector<DistNodeInfo> DistributedMemoryServer::get_cluster_nodes() const {
    std::vector<DistNodeInfo> result;
    std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
    for (const auto& [id, info] : nodes_) {
        result.push_back(info);
    }
    return result;
}

DistributedMemoryServer::Stats DistributedMemoryServer::get_stats() const {
    return {
        local_reads_.load(),
        local_writes_.load(),
        remote_reads_.load(),
        remote_writes_.load(),
        forwarded_requests_.load(),
        coherency_messages_.load(),
        static_cast<uint64_t>(client_threads_.size())
    };
}

CoherencyEngine* DistributedMemoryServer::coherency() {
    return controller_->coherency_.get();
}

/* ============================================================================
 * RDMA-based forwarding for DistributedMemoryServer
 * ============================================================================ */

bool DistributedMemoryServer::initialize_rdma_transport() {
    if (!RDMATransport::is_rdma_available()) {
        SPDLOG_WARN("No RDMA device available on this system");
        return false;
    }

    rdma_transport_ = std::make_unique<DistributedRDMATransport>(
        node_id_, rdma_addr_, rdma_port_);

    if (!rdma_transport_->initialize()) {
        SPDLOG_ERROR("Failed to initialize RDMA transport on {}:{}", rdma_addr_, rdma_port_);
        rdma_transport_.reset();
        return false;
    }

    SPDLOG_INFO("RDMA transport initialized on {}:{}", rdma_addr_, rdma_port_);

    // Register RDMA transport and message manager with the CoherencyEngine
    if (controller_->coherency_) {
        controller_->coherency_->set_rdma_transport(rdma_transport_.get());
        controller_->coherency_->set_msg_manager(msg_manager_.get());
        controller_->coherency_->activate_head(node_id_, memory_capacity_mb_ * 1024 * 1024);
    }

    return true;
}

void DistributedMemoryServer::calibrate_all_rdma_nodes() {
    if (!rdma_transport_) return;

    auto connected = rdma_transport_->get_connected_nodes();
    for (uint32_t node_id : connected) {
        auto result = rdma_transport_->calibrate_node(node_id);
        if (result.valid) {
            SPDLOG_INFO("RDMA calibration to node {}: L={:.2f}us o_s={:.2f}us o_r={:.2f}us g={:.3f}us",
                        node_id, result.L, result.o_s, result.o_r, result.g);
        }
    }

    // Apply aggregate calibration to controller's LogP model
    auto aggregate = rdma_transport_->get_aggregate_calibration();
    if (aggregate.valid) {
        controller_->calibrate_logp_from_rdma(aggregate);
    }
}

bool DistributedMemoryServer::connect_rdma_node(uint32_t node_id, const std::string& addr, uint16_t port) {
    if (!rdma_transport_) {
        SPDLOG_ERROR("RDMA transport not initialized");
        return false;
    }

    if (!rdma_transport_->connect_to_node(node_id, addr, port)) {
        SPDLOG_ERROR("Failed to connect RDMA to node {} at {}:{}", node_id, addr, port);
        return false;
    }

    SPDLOG_INFO("RDMA connected to node {} at {}:{}", node_id, addr, port);

    // Create virtual endpoint for the remote node
    uint64_t peer_capacity = memory_capacity_mb_ * 1024ULL * 1024ULL; // Assume same capacity
    uint64_t peer_base_addr = node_id * peer_capacity;

    // Get peer info from node registry if available
    {
        std::shared_lock<std::shared_mutex> lock(nodes_mutex_);
        auto it = nodes_.find(node_id);
        if (it != nodes_.end()) {
            peer_base_addr = it->second.memory_base;
            peer_capacity = it->second.memory_size;
        }
    }

    FabricLinkConfig link_cfg{100.0, 25.0, 32};  // 100ns hop, 25GB/s, 32 credits
    auto* remote = controller_->add_remote_endpoint(node_id, peer_base_addr, peer_capacity, link_cfg);
    remote->rdma_transport_ = rdma_transport_.get();
    remote->msg_manager_ = msg_manager_.get();
    remote->coherency_engine_ = controller_->coherency_.get();

    // Register in HDM decoder
    controller_->hdm_decoder_->add_range(peer_base_addr, peer_capacity, node_id, true);

    // Register fabric link with coherency engine
    controller_->coherency_->register_fabric_link(node_id, remote->fabric_link_.get());

    return true;
}

bool DistributedMemoryServer::calibrate_rdma_logp(uint32_t target_node) {
    if (!rdma_transport_) {
        SPDLOG_ERROR("RDMA transport not initialized");
        return false;
    }

    if (target_node == UINT32_MAX) {
        // Calibrate all connected nodes
        calibrate_all_rdma_nodes();
        return true;
    }

    auto result = rdma_transport_->calibrate_node(target_node);
    if (result.valid) {
        controller_->calibrate_logp_from_rdma(result);
        return true;
    }
    return false;
}

int DistributedMemoryServer::forward_read_rdma(uint32_t target_node, uint64_t addr,
                                                void* data, size_t size, uint64_t* latency_ns) {
    forwarded_requests_++;
    remote_reads_++;

    auto start = std::chrono::high_resolution_clock::now();

    // Use RDMA two-sided messaging (wraps dist_message_t in RDMAMessage)
    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_READ_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.size = size;
    req.payload.mem.client_id = node_id_;

    if (!rdma_transport_->send_message_wait_response(target_node, req, resp)) {
        SPDLOG_ERROR("RDMA forward read to node {} failed", target_node);
        return -1;
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    memcpy(data, resp.payload.mem.data, size);

    auto end = std::chrono::high_resolution_clock::now();
    double measured_ns = std::chrono::duration<double, std::nano>(end - start).count();

    // Use calibrated LogP latency, or measured if no calibration
    auto calib = rdma_transport_->get_calibration(target_node);
    double network_latency;
    if (calib.valid) {
        network_latency = controller_->calculate_logp_latency(node_id_, target_node,
                                                               req.header.timestamp);
    } else {
        network_latency = measured_ns;
    }

    // Add coherency overhead via CoherencyEngine
    double coherency_overhead = 0.0;
    if (controller_->coherency_) {
        CoherencyRequest coh_req{addr, node_id_, 0, false, req.header.timestamp};
        auto coh_resp = controller_->coherency_->process_read(coh_req);
        coherency_overhead = coh_resp.latency_ns;
    }

    *latency_ns = resp.payload.mem.latency_ns +
                  static_cast<uint64_t>(network_latency) +
                  static_cast<uint64_t>(coherency_overhead);

    return 0;
}

int DistributedMemoryServer::forward_write_rdma(uint32_t target_node, uint64_t addr,
                                                 const void* data, size_t size, uint64_t* latency_ns) {
    forwarded_requests_++;
    remote_writes_++;

    auto start = std::chrono::high_resolution_clock::now();

    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_WRITE_REQ;
    req.header.msg_id = msg_manager_->generate_msg_id();
    req.header.src_node_id = node_id_;
    req.header.dst_node_id = target_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.mem.addr = addr;
    req.payload.mem.size = size;
    req.payload.mem.client_id = node_id_;
    memcpy(req.payload.mem.data, data, std::min(size, sizeof(req.payload.mem.data)));

    if (!rdma_transport_->send_message_wait_response(target_node, req, resp)) {
        SPDLOG_ERROR("RDMA forward write to node {} failed", target_node);
        return -1;
    }

    if (resp.payload.mem.status != 0) {
        return -1;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double measured_ns = std::chrono::duration<double, std::nano>(end - start).count();

    auto calib = rdma_transport_->get_calibration(target_node);
    double network_latency;
    if (calib.valid) {
        network_latency = controller_->calculate_logp_latency(node_id_, target_node,
                                                               req.header.timestamp);
    } else {
        network_latency = measured_ns;
    }

    // Add coherency overhead via CoherencyEngine (includes invalidation cost)
    double coherency_overhead = 0.0;
    if (controller_->coherency_) {
        CoherencyRequest coh_req{addr, node_id_, 0, true, req.header.timestamp};
        auto coh_resp = controller_->coherency_->process_write(coh_req);
        coherency_overhead = coh_resp.latency_ns;
    }

    *latency_ns = resp.payload.mem.latency_ns +
                  static_cast<uint64_t>(network_latency) +
                  static_cast<uint64_t>(coherency_overhead);

    return 0;
}

/* ============================================================================
 * DistributedRDMATransport Implementation
 * ============================================================================ */

DistributedRDMATransport::DistributedRDMATransport(uint32_t node_id,
                                                     const std::string& bind_addr, uint16_t port)
    : local_node_id_(node_id), bind_addr_(bind_addr), port_(port), running_(false) {
}

DistributedRDMATransport::~DistributedRDMATransport() {
    shutdown();
}

bool DistributedRDMATransport::initialize() {
    // Check RDMA availability
    if (!RDMATransport::is_rdma_available()) {
        SPDLOG_ERROR("No RDMA devices available");
        return false;
    }

    // Create RDMA server for incoming connections
    server_ = std::make_unique<RDMAServer>(bind_addr_, port_);
    if (server_->start() != 0) {
        SPDLOG_ERROR("Failed to start RDMA server on {}:{}", bind_addr_, port_);
        server_.reset();
        return false;
    }

    running_ = true;

    // Start accept thread for incoming connections
    accept_thread_ = std::thread(&DistributedRDMATransport::accept_loop, this);

    SPDLOG_INFO("DistributedRDMATransport initialized: node={} bind={}:{}",
                local_node_id_, bind_addr_, port_);
    return true;
}

void DistributedRDMATransport::shutdown() {
    running_ = false;

    // Disconnect all nodes
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [node_id, conn] : connections_) {
            if (conn.client) {
                conn.client->disconnect();
            }
            conn.connected = false;
        }
        connections_.clear();
    }

    // Stop server
    if (server_) {
        server_->stop();
    }

    // Join accept thread
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    server_.reset();
    SPDLOG_INFO("DistributedRDMATransport shutdown complete");
}

void DistributedRDMATransport::accept_loop() {
    while (running_) {
        if (server_->accept_connection() == 0) {
            SPDLOG_INFO("Accepted incoming RDMA connection");
            // Handle client in a separate thread
            std::thread handler([this]() {
                server_->handle_client();
            });
            handler.detach();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool DistributedRDMATransport::connect_to_node(uint32_t node_id, const std::string& addr, uint16_t port) {
    std::lock_guard<std::mutex> lock(connections_mutex_);

    // Check if already connected
    auto it = connections_.find(node_id);
    if (it != connections_.end() && it->second.connected) {
        SPDLOG_WARN("Already connected to node {}", node_id);
        return true;
    }

    // Create new client connection
    auto& conn = connections_[node_id];
    conn.client = std::make_unique<RDMAClient>(addr, port);

    if (conn.client->connect() != 0) {
        SPDLOG_ERROR("Failed to connect RDMA to node {} at {}:{}", node_id, addr, port);
        conn.client.reset();
        connections_.erase(node_id);
        return false;
    }

    conn.connected = true;
    SPDLOG_INFO("RDMA connected to node {} at {}:{}", node_id, addr, port);

    // Exchange MR info for potential one-sided operations
    exchange_mr_info(node_id);

    return true;
}

void DistributedRDMATransport::disconnect_node(uint32_t node_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(node_id);
    if (it != connections_.end()) {
        if (it->second.client) {
            it->second.client->disconnect();
        }
        connections_.erase(it);
        SPDLOG_INFO("Disconnected RDMA from node {}", node_id);
    }
}

bool DistributedRDMATransport::is_connected(uint32_t node_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(connections_mutex_));
    auto it = connections_.find(node_id);
    return it != connections_.end() && it->second.connected;
}

std::vector<uint32_t> DistributedRDMATransport::get_connected_nodes() const {
    std::vector<uint32_t> nodes;
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(connections_mutex_));
    for (const auto& [id, conn] : connections_) {
        if (conn.connected) {
            nodes.push_back(id);
        }
    }
    return nodes;
}

bool DistributedRDMATransport::send_message(uint32_t dst_node, const dist_message_t& msg) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(dst_node);
    if (it == connections_.end() || !it->second.connected || !it->second.client) {
        SPDLOG_ERROR("Cannot send RDMA message to node {}: not connected", dst_node);
        return false;
    }

    // Wrap dist_message_t into an RDMAMessage
    RDMAMessage rdma_msg;
    memset(&rdma_msg, 0, sizeof(rdma_msg));
    rdma_msg.request.op_type = RDMA_OP_WRITE;
    rdma_msg.request.addr = msg.payload.mem.addr;
    rdma_msg.request.size = sizeof(dist_message_t);
    rdma_msg.request.timestamp = msg.header.timestamp;
    rdma_msg.request.host_id = static_cast<uint8_t>(local_node_id_);

    // Serialize the dist_message into the data field (truncated to fit)
    size_t copy_size = std::min(sizeof(rdma_msg.request.data), sizeof(dist_message_t));
    memcpy(rdma_msg.request.data, &msg, copy_size);

    if (it->second.client->send_message(rdma_msg) != 0) {
        SPDLOG_ERROR("Failed to send RDMA message to node {}", dst_node);
        return false;
    }

    return true;
}

bool DistributedRDMATransport::send_message_wait_response(uint32_t dst_node,
                                                            const dist_message_t& req,
                                                            dist_message_t& resp,
                                                            int timeout_ms) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(dst_node);
    if (it == connections_.end() || !it->second.connected || !it->second.client) {
        return false;
    }

    // Build RDMA request from dist_message
    RDMARequest rdma_req;
    memset(&rdma_req, 0, sizeof(rdma_req));
    rdma_req.op_type = (req.header.msg_type == DIST_MSG_READ_REQ) ? RDMA_OP_READ : RDMA_OP_WRITE;
    rdma_req.addr = req.payload.mem.addr;
    rdma_req.size = req.payload.mem.size;
    rdma_req.timestamp = req.header.timestamp;
    rdma_req.host_id = static_cast<uint8_t>(local_node_id_);

    // Copy data for write requests
    if (rdma_req.op_type == RDMA_OP_WRITE) {
        memcpy(rdma_req.data, req.payload.mem.data,
               std::min(sizeof(rdma_req.data), sizeof(req.payload.mem.data)));
    }

    RDMAResponse rdma_resp;
    if (it->second.client->send_request(rdma_req, rdma_resp) != 0) {
        SPDLOG_ERROR("RDMA send_request to node {} failed", dst_node);
        return false;
    }

    // Convert RDMA response back to dist_message_t
    memset(&resp, 0, sizeof(resp));
    resp.header.msg_type = (req.header.msg_type == DIST_MSG_READ_REQ) ?
                           DIST_MSG_READ_RESP : DIST_MSG_WRITE_RESP;
    resp.header.msg_id = req.header.msg_id;
    resp.header.src_node_id = dst_node;
    resp.header.dst_node_id = local_node_id_;
    resp.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    resp.payload.mem.status = rdma_resp.status;
    resp.payload.mem.latency_ns = rdma_resp.latency_ns;
    resp.payload.mem.cache_state = rdma_resp.cache_state;
    memcpy(resp.payload.mem.data, rdma_resp.data,
           std::min(sizeof(resp.payload.mem.data), sizeof(rdma_resp.data)));

    return true;
}

bool DistributedRDMATransport::rdma_read(uint32_t dst_node, uint64_t remote_offset,
                                          void* local_buf, size_t size) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(dst_node);
    if (it == connections_.end() || !it->second.connected || !it->second.client) {
        return false;
    }

    // For one-sided RDMA read, we use send_request with OP_READ
    RDMARequest req;
    memset(&req, 0, sizeof(req));
    req.op_type = RDMA_OP_READ;
    req.addr = remote_offset;
    req.size = size;
    req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.host_id = static_cast<uint8_t>(local_node_id_);

    RDMAResponse resp;
    if (it->second.client->send_request(req, resp) != 0) {
        return false;
    }

    if (resp.status != 0) {
        return false;
    }

    memcpy(local_buf, resp.data, std::min(size, sizeof(resp.data)));
    return true;
}

bool DistributedRDMATransport::rdma_write(uint32_t dst_node, uint64_t remote_offset,
                                           const void* local_buf, size_t size) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(dst_node);
    if (it == connections_.end() || !it->second.connected || !it->second.client) {
        return false;
    }

    RDMARequest req;
    memset(&req, 0, sizeof(req));
    req.op_type = RDMA_OP_WRITE;
    req.addr = remote_offset;
    req.size = size;
    req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.host_id = static_cast<uint8_t>(local_node_id_);
    memcpy(req.data, local_buf, std::min(size, sizeof(req.data)));

    RDMAResponse resp;
    if (it->second.client->send_request(req, resp) != 0) {
        return false;
    }

    return resp.status == 0;
}

bool DistributedRDMATransport::exchange_mr_info(uint32_t node_id) {
    // MR info exchange is implicit in the RDMA connection setup
    // The remote side's buffer info is available after connect
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(node_id);
    if (it == connections_.end() || !it->second.client) {
        return false;
    }

    // For now, mark as having basic info from connection
    it->second.remote_buffer_size = RDMA_BUFFER_SIZE * sizeof(RDMAMessage);
    SPDLOG_DEBUG("MR info exchanged with node {}: buffer_size={}",
                 node_id, it->second.remote_buffer_size);
    return true;
}

RDMACalibrationResult DistributedRDMATransport::calibrate_node(uint32_t dst_node, uint32_t num_samples) {
    RDMACalibrationResult result;

    if (!is_connected(dst_node)) {
        SPDLOG_ERROR("Cannot calibrate: not connected to node {}", dst_node);
        return result;
    }

    std::vector<double> rtts;
    std::vector<double> send_times;
    std::vector<double> gaps;
    rtts.reserve(num_samples);
    send_times.reserve(num_samples);
    gaps.reserve(num_samples);

    // Warmup: send a few messages to prime the path
    for (uint32_t i = 0; i < std::min(num_samples / 10, 100u); i++) {
        RDMARequest req;
        memset(&req, 0, sizeof(req));
        req.op_type = RDMA_OP_READ;
        req.addr = 0;
        req.size = 0;
        req.timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        req.host_id = static_cast<uint8_t>(local_node_id_);

        RDMAResponse resp;
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(dst_node);
        if (it != connections_.end() && it->second.client) {
            it->second.client->send_request(req, resp);
        }
    }

    // Ping-pong measurement loop
    auto prev_send_time = std::chrono::high_resolution_clock::now();

    for (uint32_t i = 0; i < num_samples; i++) {
        RDMARequest req;
        memset(&req, 0, sizeof(req));
        req.op_type = RDMA_OP_READ;
        req.addr = 0;
        req.size = 8; // Small payload for calibration
        req.host_id = static_cast<uint8_t>(local_node_id_);

        auto send_start = std::chrono::high_resolution_clock::now();
        req.timestamp = send_start.time_since_epoch().count();

        RDMAResponse resp;
        bool success = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(dst_node);
            if (it != connections_.end() && it->second.client) {
                success = (it->second.client->send_request(req, resp) == 0);
            }
        }

        auto recv_end = std::chrono::high_resolution_clock::now();

        if (success) {
            double rtt_us = std::chrono::duration<double, std::micro>(recv_end - send_start).count();
            double send_overhead_us = std::chrono::duration<double, std::micro>(
                send_start - prev_send_time).count();

            rtts.push_back(rtt_us);

            if (i > 0) {
                gaps.push_back(send_overhead_us);
            }
        }

        prev_send_time = send_start;
    }

    if (rtts.empty()) {
        SPDLOG_ERROR("Calibration to node {} failed: no successful measurements", dst_node);
        return result;
    }

    // Sort and take median (robust against outliers)
    std::sort(rtts.begin(), rtts.end());
    std::sort(gaps.begin(), gaps.end());

    double median_rtt = rtts[rtts.size() / 2];
    double median_gap = gaps.empty() ? 0.1 : gaps[gaps.size() / 2];

    // Calculate p10 RTT for minimum overhead estimation
    double p10_rtt = rtts[rtts.size() / 10];

    // Extract LogP parameters:
    // o_s + o_r ~= p10_rtt (minimum RTT represents pure overhead)
    // L = (median_rtt - p10_rtt) / 2 (latency is half the excess RTT)
    // g = median gap between consecutive sends
    double overhead_total = p10_rtt;
    result.o_s = overhead_total * 0.5;  // Split evenly between send/recv
    result.o_r = overhead_total * 0.5;
    result.L = std::max(0.0, (median_rtt - overhead_total) / 2.0);
    result.g = median_gap;
    result.samples = rtts.size();
    result.valid = true;

    // Store calibration
    {
        std::lock_guard<std::mutex> lock(calibration_mutex_);
        calibration_results_[dst_node] = result;
    }

    SPDLOG_INFO("RDMA calibration to node {} ({} samples): "
                "L={:.3f}us o_s={:.3f}us o_r={:.3f}us g={:.3f}us median_rtt={:.3f}us",
                dst_node, result.samples, result.L, result.o_s, result.o_r,
                result.g, median_rtt);

    return result;
}

RDMACalibrationResult DistributedRDMATransport::get_calibration(uint32_t node_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(calibration_mutex_));
    auto it = calibration_results_.find(node_id);
    if (it != calibration_results_.end()) {
        return it->second;
    }
    return RDMACalibrationResult();
}

RDMACalibrationResult DistributedRDMATransport::get_aggregate_calibration() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(calibration_mutex_));

    if (calibration_results_.empty()) {
        return RDMACalibrationResult();
    }

    // Aggregate: take the average of all node calibrations
    RDMACalibrationResult aggregate;
    aggregate.valid = true;
    aggregate.samples = 0;

    for (const auto& [node_id, calib] : calibration_results_) {
        if (!calib.valid) continue;
        aggregate.L += calib.L;
        aggregate.o_s += calib.o_s;
        aggregate.o_r += calib.o_r;
        aggregate.g += calib.g;
        aggregate.samples += calib.samples;
    }

    size_t count = calibration_results_.size();
    if (count > 0) {
        aggregate.L /= count;
        aggregate.o_s /= count;
        aggregate.o_r /= count;
        aggregate.g /= count;
    }

    return aggregate;
}

/* ============================================================================
 * DistributedMHSLDManager removed - replaced by CoherencyEngine.
 * ============================================================================ */

#if 0  // DistributedMHSLDManager removed
DistributedMHSLDManager_removed::DistributedMHSLDManager(CXLController* ctrl,
                                                   DistributedRDMATransport* rdma,
                                                   DistributedMessageManager* msg_mgr,
                                                   uint32_t node_id, uint32_t head_id)
    : controller_(ctrl), rdma_transport_(rdma), msg_manager_(msg_mgr),
      local_node_id_(node_id), local_head_id_(head_id),
      remote_invalidations_(0), remote_fetches_(0),
      remote_updates_(0), cache_hits_(0) {
}

double DistributedMHSLDManager::distributed_read(uint64_t addr, uint64_t timestamp) {
    double total_latency = 0.0;

    // First, try local MH-SLD read (handles local coherency)
    if (controller_->mhsld_device) {
        total_latency += controller_->mhsld_read(local_head_id_, addr, timestamp);
    }

    // Check remote directory cache for cross-node coherency
    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex_);
        auto it = remote_dir_cache_.find(addr & ~(DIST_CACHELINE_SIZE - 1));
        if (it != remote_dir_cache_.end()) {
            cache_hits_++;
            // If the entry shows another node owns this exclusively,
            // we need to issue a downgrade via RDMA
            if (it->second.state == DIST_CACHE_MODIFIED &&
                it->second.owner_node != local_node_id_) {
                total_latency += rdma_fetch_directory(it->second.home_node, addr);
            }
            return total_latency;
        }
    }

    // Cache miss - determine home node and fetch directory info
    if (msg_manager_) {
        auto active_nodes = msg_manager_->get_active_nodes();
        if (active_nodes.size() > 1) {
            // Simple home node calculation
            uint64_t cacheline_idx = (addr & ~(DIST_CACHELINE_SIZE - 1)) / DIST_CACHELINE_SIZE;
            uint32_t home_node = active_nodes[cacheline_idx % active_nodes.size()];

            if (home_node != local_node_id_) {
                total_latency += rdma_fetch_directory(home_node, addr);
            }
        }
    }

    return total_latency;
}

double DistributedMHSLDManager::distributed_write(uint64_t addr, uint64_t timestamp) {
    double total_latency = 0.0;

    // Local MH-SLD write (handles local coherency)
    if (controller_->mhsld_device) {
        total_latency += controller_->mhsld_write(local_head_id_, addr, timestamp);
    }

    // For writes, we need to invalidate remote copies
    if (rdma_transport_) {
        auto connected = rdma_transport_->get_connected_nodes();
        for (uint32_t node_id : connected) {
            if (node_id != local_node_id_) {
                total_latency += rdma_invalidate_remote(node_id, addr, timestamp);
            }
        }
    }

    // Update remote directory to show this node as owner
    if (msg_manager_) {
        auto active_nodes = msg_manager_->get_active_nodes();
        if (active_nodes.size() > 1) {
            uint64_t cacheline_idx = (addr & ~(DIST_CACHELINE_SIZE - 1)) / DIST_CACHELINE_SIZE;
            uint32_t home_node = active_nodes[cacheline_idx % active_nodes.size()];

            if (home_node != local_node_id_) {
                total_latency += rdma_update_directory(home_node, addr,
                                                       static_cast<uint8_t>(DIST_CACHE_MODIFIED),
                                                       local_node_id_);
            }
        }
    }

    return total_latency;
}

double DistributedMHSLDManager::distributed_atomic(uint64_t addr, uint64_t timestamp) {
    // Atomics require exclusive access, same as write but with extra overhead
    double latency = distributed_write(addr, timestamp);
    // Add atomic serialization overhead
    latency += 20.0; // 20ns for atomic RMW overhead
    return latency;
}

double DistributedMHSLDManager::rdma_invalidate_remote(uint32_t target_node, uint64_t addr,
                                                         uint64_t timestamp) {
    if (!rdma_transport_ || !rdma_transport_->is_connected(target_node)) {
        // Fall back to message-based invalidation
        if (msg_manager_) {
            dist_message_t inv_msg;
            memset(&inv_msg, 0, sizeof(inv_msg));
            inv_msg.header.msg_type = DIST_MSG_INVALIDATE;
            inv_msg.header.msg_id = msg_manager_->generate_msg_id();
            inv_msg.header.src_node_id = local_node_id_;
            inv_msg.header.dst_node_id = target_node;
            inv_msg.header.timestamp = timestamp;
            inv_msg.payload.coherency.cacheline_addr = addr & ~(DIST_CACHELINE_SIZE - 1);
            inv_msg.payload.coherency.requesting_node = local_node_id_;

            msg_manager_->send_message(target_node, inv_msg);
        }
        remote_invalidations_++;
        return 50.0; // Estimated SHM invalidation latency in ns
    }

    // Use RDMA to send invalidation
    dist_message_t inv_msg;
    memset(&inv_msg, 0, sizeof(inv_msg));
    inv_msg.header.msg_type = DIST_MSG_INVALIDATE;
    inv_msg.header.msg_id = 0; // No response expected for fire-and-forget
    inv_msg.header.src_node_id = local_node_id_;
    inv_msg.header.dst_node_id = target_node;
    inv_msg.header.timestamp = timestamp;
    inv_msg.payload.coherency.cacheline_addr = addr & ~(DIST_CACHELINE_SIZE - 1);
    inv_msg.payload.coherency.requesting_node = local_node_id_;

    rdma_transport_->send_message(target_node, inv_msg);
    remote_invalidations_++;

    // Return calibrated latency for the invalidation
    auto calib = rdma_transport_->get_calibration(target_node);
    if (calib.valid) {
        return (calib.o_s + calib.L) * 1000.0; // Convert us to ns
    }
    return 10.0; // Default RDMA invalidation latency in ns
}

double DistributedMHSLDManager::rdma_fetch_directory(uint32_t home_node, uint64_t addr) {
    remote_fetches_++;

    uint64_t cacheline_addr = addr & ~(DIST_CACHELINE_SIZE - 1);

    if (!rdma_transport_ || !rdma_transport_->is_connected(home_node)) {
        // Fall back to SHM message-based directory lookup
        if (msg_manager_) {
            dist_message_t req, resp;
            memset(&req, 0, sizeof(req));
            req.header.msg_type = DIST_MSG_DIR_QUERY;
            req.header.msg_id = msg_manager_->generate_msg_id();
            req.header.src_node_id = local_node_id_;
            req.header.dst_node_id = home_node;
            req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            req.payload.coherency.cacheline_addr = cacheline_addr;

            if (msg_manager_->send_message_wait_response(home_node, req, resp)) {
                // Update cache
                std::unique_lock<std::shared_mutex> lock(cache_mutex_);
                auto& entry = remote_dir_cache_[cacheline_addr];
                entry.cacheline_addr = cacheline_addr;
                entry.state = static_cast<dist_cache_state_t>(resp.payload.coherency.current_state);
                entry.owner_node = resp.payload.coherency.owner_node;
                entry.home_node = home_node;
                entry.version = resp.payload.coherency.version;
            }
        }
        return 100.0; // SHM directory fetch latency in ns
    }

    // Use RDMA send/recv for directory query
    dist_message_t req, resp;
    memset(&req, 0, sizeof(req));
    req.header.msg_type = DIST_MSG_DIR_QUERY;
    req.header.msg_id = 0;
    req.header.src_node_id = local_node_id_;
    req.header.dst_node_id = home_node;
    req.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    req.payload.coherency.cacheline_addr = cacheline_addr;

    if (rdma_transport_->send_message_wait_response(home_node, req, resp)) {
        // Update local cache
        std::unique_lock<std::shared_mutex> lock(cache_mutex_);
        auto& entry = remote_dir_cache_[cacheline_addr];
        entry.cacheline_addr = cacheline_addr;
        entry.state = static_cast<dist_cache_state_t>(resp.payload.coherency.current_state);
        entry.owner_node = resp.payload.coherency.owner_node;
        entry.home_node = home_node;
        entry.version = resp.payload.coherency.version;
    }

    // Return calibrated latency
    auto calib = rdma_transport_->get_calibration(home_node);
    if (calib.valid) {
        return (calib.o_s + calib.L + calib.o_r) * 1000.0; // Full RTT in ns
    }
    return 20.0; // Default RDMA directory fetch latency in ns
}

double DistributedMHSLDManager::rdma_update_directory(uint32_t home_node, uint64_t addr,
                                                        uint8_t new_state, uint32_t new_owner) {
    remote_updates_++;

    uint64_t cacheline_addr = addr & ~(DIST_CACHELINE_SIZE - 1);

    // Send directory update
    dist_message_t update_msg;
    memset(&update_msg, 0, sizeof(update_msg));
    update_msg.header.msg_type = DIST_MSG_DIR_UPDATE;
    update_msg.header.src_node_id = local_node_id_;
    update_msg.header.dst_node_id = home_node;
    update_msg.header.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    update_msg.payload.coherency.cacheline_addr = cacheline_addr;
    update_msg.payload.coherency.current_state = new_state;
    update_msg.payload.coherency.owner_node = new_owner;
    update_msg.payload.coherency.requesting_node = local_node_id_;

    if (rdma_transport_ && rdma_transport_->is_connected(home_node)) {
        rdma_transport_->send_message(home_node, update_msg);
        auto calib = rdma_transport_->get_calibration(home_node);
        if (calib.valid) {
            return calib.o_s * 1000.0; // Just send overhead for fire-and-forget in ns
        }
        return 5.0; // Default RDMA update latency
    }

    // Fall back to SHM
    if (msg_manager_) {
        update_msg.header.msg_id = msg_manager_->generate_msg_id();
        msg_manager_->send_message(home_node, update_msg);
    }
    return 50.0; // SHM update latency in ns
}

void DistributedMHSLDManager::invalidate_cache_entry(uint64_t addr) {
    uint64_t cacheline_addr = addr & ~(DIST_CACHELINE_SIZE - 1);
    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    remote_dir_cache_.erase(cacheline_addr);
}

DistributedMHSLDManager::Stats DistributedMHSLDManager::get_stats() const {
    return {
        remote_invalidations_.load(),
        remote_fetches_.load(),
        remote_updates_.load(),
        cache_hits_.load()
    };
}
#endif  // DistributedMHSLDManager removed
