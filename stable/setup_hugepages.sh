#!/bin/bash

echo "[*] Cleaning up old artifacts..."
rm -f /dev/hugepages/map_A
rm -f /dev/hugepages/map_B

echo "[*] Resetting Hugepage Pool..."
# 1. Free all hugepages
echo 0 > /proc/sys/vm/nr_hugepages

# 2. Defragment RAM (Critical Step)
# This moves data around to create large empty physical blocks
echo "[*] Compacting Memory..."
sync
echo 3 > /proc/sys/vm/drop_caches
echo 1 > /proc/sys/vm/compact_memory

# Wait a moment for compaction
sleep 2

# 3. Re-allocate Hugepages
# We allocate 2048 pages (4GB) to ensure we have plenty of contiguous space
echo "[*] Allocating 2048 Hugepages..."
echo 2048 > /proc/sys/vm/nr_hugepages

# Verify
COUNT=$(cat /proc/sys/vm/nr_hugepages)
echo "[+] Allocated $COUNT hugepages."

if [ "$COUNT" -lt 2048 ]; then
    echo "[-] WARNING: Could not allocate all requested pages. Physical memory might be fragmented."
else
    echo "[+] Memory is fresh and compacted. Probability of contiguity is HIGH."
fi