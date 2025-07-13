#!/bin/bash

NUM_CLIENTS=${1:-4}

echo "Starting $NUM_CLIENTS concurrent clients..."

for i in $(seq 1 $NUM_CLIENTS); do
    ./test_cxl_mem > client_$i.log 2>&1 &
done

echo "Waiting for all clients to complete..."
wait

echo "All clients completed. Checking results:"
for i in $(seq 1 $NUM_CLIENTS); do
    echo "Client $i:"
    tail -n 5 client_$i.log
    echo ""
done