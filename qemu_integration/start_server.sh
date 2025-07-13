#!/bin/bash

PORT=${1:-9999}
TOPOLOGY=${2:-topology_simple.txt}

echo "Starting CXLMemSim server on port $PORT with topology $TOPOLOGY"
./cxlmemsim_server $PORT $TOPOLOGY