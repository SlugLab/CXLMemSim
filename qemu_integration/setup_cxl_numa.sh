#!/bin/bash

# CXL NUMA Configuration Script
# This script automatically configures CXL memory as NUMA node 1 at boot

set -e

LOG_FILE="/var/log/cxl_numa_setup.log"
CXL_REGION_SIZE="256M"
MAX_RETRIES=10
RETRY_DELAY=2

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

wait_for_cxl_device() {
    local retries=0
    while [ $retries -lt $MAX_RETRIES ]; do
        if cxl list -M 2>/dev/null | grep -q "mem0"; then
            log "CXL device mem0 detected"
            return 0
        fi
        log "Waiting for CXL device... (attempt $((retries+1))/$MAX_RETRIES)"
        sleep $RETRY_DELAY
        retries=$((retries+1))
    done
    log "ERROR: CXL device not found after $MAX_RETRIES attempts"
    return 1
}

setup_cxl_region() {
    log "Creating CXL region..."
    
    # Check if region already exists
    if cxl list -R 2>/dev/null | grep -q "region0"; then
        log "Region0 already exists, skipping creation"
        return 0
    fi
    
    # Create CXL region
    if cxl create-region -m -d decoder0.0 -w 1 mem0 -s "$CXL_REGION_SIZE" 2>&1 | tee -a "$LOG_FILE"; then
        log "CXL region created successfully"
        return 0
    else
        log "ERROR: Failed to create CXL region"
        return 1
    fi
}

setup_dax_namespace() {
    log "Setting up DAX namespace..."
    
    # Check if namespace already exists
    if ndctl list -N 2>/dev/null | grep -q "namespace"; then
        log "Namespace already exists"
        return 0
    fi
    
    # Wait a bit for region to be fully initialized
    sleep 2
    
    # Create DAX namespace
    if ndctl create-namespace -m dax -r region0 2>&1 | tee -a "$LOG_FILE"; then
        log "DAX namespace created successfully"
        return 0
    else
        # Try to reconfigure if it fails
        log "Initial namespace creation failed, trying to reconfigure..."
        ndctl destroy-namespace all -f 2>/dev/null || true
        sleep 1
        if ndctl create-namespace -m dax -r region0 2>&1 | tee -a "$LOG_FILE"; then
            log "DAX namespace created after reconfiguration"
            return 0
        else
            log "WARNING: Could not create DAX namespace"
            return 1
        fi
    fi
}

configure_numa_node() {
    log "Configuring NUMA node..."
    
    # Find the DAX device
    local dax_device=$(ls /sys/bus/dax/devices/ 2>/dev/null | head -n1)
    
    if [ -z "$dax_device" ]; then
        log "WARNING: No DAX device found"
        return 1
    fi
    
    # Online the memory as NUMA node 1
    if [ -f "/sys/bus/dax/devices/$dax_device/target_node" ]; then
        local target_node=$(cat "/sys/bus/dax/devices/$dax_device/target_node")
        log "Target NUMA node: $target_node"
        
        # Try to online the memory
        if daxctl reconfigure-device --mode=system-ram "$dax_device" 2>&1 | tee -a "$LOG_FILE"; then
            log "Memory onlined as system RAM"
        else
            log "WARNING: Could not online memory as system RAM"
        fi
    fi
    
    # Verify NUMA configuration
    numactl --hardware 2>&1 | tee -a "$LOG_FILE"
    
    return 0
}

main() {
    log "Starting CXL NUMA configuration..."
    
    # Load required kernel modules
    modprobe cxl_core 2>/dev/null || true
    modprobe cxl_pci 2>/dev/null || true
    modprobe cxl_acpi 2>/dev/null || true
    modprobe cxl_port 2>/dev/null || true
    modprobe cxl_mem 2>/dev/null || true
    modprobe dax 2>/dev/null || true
    modprobe device_dax 2>/dev/null || true
    modprobe kmem 2>/dev/null || true
    
    # Wait for CXL device to appear
    if ! wait_for_cxl_device; then
        log "Aborting: CXL device not available"
        exit 1
    fi
    
    # Setup CXL region
    if ! setup_cxl_region; then
        log "Warning: CXL region setup failed, continuing anyway"
    fi
    
    # Setup DAX namespace
    if ! setup_dax_namespace; then
        log "Warning: DAX namespace setup failed, continuing anyway"
    fi
    
    # Configure NUMA node
    #if ! configure_numa_node; then
    #    log "Warning: NUMA node configuration incomplete"
    #fi
    
    log "CXL NUMA configuration completed"
    
    # Display final configuration
    log "Final CXL configuration:"
    cxl list 2>&1 | tee -a "$LOG_FILE"
    log "Final NUMA configuration:"
    numactl --hardware 2>&1 | tee -a "$LOG_FILE"
}

# Run main function
main
#dhcpcd
ip link set enp0s2 up
ip addr add 192.168.100.10/24 dev enp0s2
ip route add default via 192.168.100.1