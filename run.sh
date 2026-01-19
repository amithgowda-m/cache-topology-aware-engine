#!/bin/bash
set -e

echo "=========================================="
echo "CTAE Module Build and Load Script"
echo "=========================================="

echo "[1/7] Unloading existing modules..."
sudo rmmod ctae_policy 2>/dev/null || true
sleep 1
sudo rmmod ctae_monitor 2>/dev/null || true
sleep 1
sudo rmmod ctae_core 2>/dev/null || true
sleep 1

echo "[2/7] Cleaning..."
make clean > /dev/null
find . -type f \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' -o -name '.*.cmd' \) -delete 2>/dev/null || true

echo "[3/7] Building ctae_core..."
make core

echo "[4/7] Building ctae_monitor..."
make monitor

echo "[5/7] Building ctae_policy..."
make policy

echo "[6/7] Loading modules..."
# Insert in dependency order
sudo insmod core/ctae_core.ko
sudo insmod monitor/ctae_monitor.ko
sudo insmod policy/ctae_policy.ko

echo "[7/7] Loaded modules:"
lsmod | grep ctae

echo ""
echo "=========================================="
echo "CTAE modules loaded successfully!"
echo "=========================================="
echo ""
echo "Monitor with: sudo dmesg -w | grep CTAE"
echo "Run test with: ./stress_test"