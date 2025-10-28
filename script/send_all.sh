#!/bin/bash
for i in 0 1 2 3 4 5 6 7; do
	scp -r $@ root@192.168.100.1$i:/root
done
