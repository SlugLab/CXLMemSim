#!/bin/bash

# Setup script for RDMA support in CXLMemSim
# This script installs necessary dependencies and configures RDMA

set -e

echo "============================================"
echo "CXLMemSim RDMA Setup Script"
echo "============================================"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VER=$VERSION_ID
else
    echo "Cannot detect OS"
    exit 1
fi

echo "Detected OS: $OS $VER"

# Install RDMA packages based on OS
case $OS in
    ubuntu|debian)
        echo "Installing RDMA packages for Ubuntu/Debian..."
        apt-get update
        apt-get install -y \
            rdma-core \
            libibverbs-dev \
            librdmacm-dev \
            ibverbs-utils \
            perftest \
            infiniband-diags \
            opensm
        ;;

    fedora|rhel|centos)
        echo "Installing RDMA packages for Fedora/RHEL/CentOS..."
        yum install -y \
            rdma-core \
            libibverbs-devel \
            librdmacm-devel \
            perftest \
            infiniband-diags \
            opensm
        ;;

    arch)
        echo "Installing RDMA packages for Arch Linux..."
        pacman -S --noconfirm \
            rdma-core \
            perftest
        ;;

    *)
        echo "Unsupported OS: $OS"
        echo "Please install RDMA packages manually:"
        echo "  - rdma-core"
        echo "  - libibverbs-dev/devel"
        echo "  - librdmacm-dev/devel"
        exit 1
        ;;
esac

# Load RDMA kernel modules
echo "Loading RDMA kernel modules..."
modprobe ib_core
modprobe ib_umad
modprobe ib_uverbs
modprobe rdma_cm
modprobe rdma_ucm

# For RoCE (RDMA over Converged Ethernet)
modprobe mlx4_ib || true
modprobe mlx5_ib || true

# For iWARP
modprobe iw_cxgb4 || true

# For soft-RoCE (RXE) - software RDMA for testing
modprobe rdma_rxe || true

# Check RDMA devices
echo "Checking RDMA devices..."
if command -v ibv_devices &> /dev/null; then
    ibv_devices
else
    echo "ibv_devices command not found"
fi

# Setup soft-RoCE if no hardware RDMA devices found
if ! ibv_devices 2>/dev/null | grep -q "device"; then
    echo "No hardware RDMA devices found. Setting up soft-RoCE..."

    # Get primary network interface
    PRIMARY_IFACE=$(ip route | grep default | awk '{print $5}' | head -n1)

    if [ -z "$PRIMARY_IFACE" ]; then
        echo "Could not determine primary network interface"
        echo "Please set up soft-RoCE manually with: rdma link add rxe0 type rxe netdev <interface>"
    else
        echo "Using network interface: $PRIMARY_IFACE"

        # Create soft-RoCE device
        rdma link add rxe0 type rxe netdev $PRIMARY_IFACE 2>/dev/null || true

        # Verify soft-RoCE device
        if rdma link show | grep -q rxe0; then
            echo "Soft-RoCE device rxe0 created successfully"
        else
            echo "Failed to create soft-RoCE device"
        fi
    fi
fi

# Set up hugepages for better RDMA performance
echo "Setting up hugepages..."
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Configure RDMA limits
echo "Configuring system limits for RDMA..."
cat >> /etc/security/limits.conf <<EOF
* soft memlock unlimited
* hard memlock unlimited
EOF

# Create systemd service for RDMA setup (optional)
cat > /etc/systemd/system/cxlmemsim-rdma.service <<EOF
[Unit]
Description=CXLMemSim RDMA Setup
After=network.target

[Service]
Type=oneshot
ExecStart=/bin/bash -c 'modprobe rdma_rxe; rdma link add rxe0 type rxe netdev $(ip route | grep default | awk "{print $5}" | head -n1) || true'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable cxlmemsim-rdma.service

# Create configuration file
echo "Creating RDMA configuration file..."
mkdir -p /etc/cxlmemsim
cat > /etc/cxlmemsim/rdma.conf <<EOF
# CXLMemSim RDMA Configuration

# Transport mode: tcp, shm, or rdma
CXL_TRANSPORT_MODE=rdma

# RDMA server address (for client)
CXL_MEMSIM_RDMA_SERVER=127.0.0.1

# RDMA server port
CXL_MEMSIM_RDMA_PORT=5555

# Enable RDMA statistics
CXL_RDMA_STATS=1

# RDMA buffer size (in KB)
CXL_RDMA_BUFFER_SIZE=4096

# RDMA queue depth
CXL_RDMA_QUEUE_DEPTH=512
EOF

echo "============================================"
echo "RDMA setup complete!"
echo "============================================"
echo ""
echo "Configuration file: /etc/cxlmemsim/rdma.conf"
echo ""
echo "To test RDMA setup:"
echo "  1. Check devices: ibv_devices"
echo "  2. Check device info: ibv_devinfo"
echo "  3. Test bandwidth: ib_write_bw (server) and ib_write_bw <server_ip> (client)"
echo ""
echo "To use RDMA with CXLMemSim:"
echo "  1. Set environment variable: export CXL_TRANSPORT_MODE=rdma"
echo "  2. Start server: ./cxlmemsim_server_rdma <tcp_port> [rdma_port]"
echo "  3. Configure QEMU to use RDMA"
echo ""
echo "Note: You may need to reboot or re-login for limits to take effect"