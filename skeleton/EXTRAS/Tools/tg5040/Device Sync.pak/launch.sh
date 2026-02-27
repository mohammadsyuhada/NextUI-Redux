#!/bin/sh

cd "$(dirname "$0")"

# Cortex-A53 (cpu0-3) - cap max freq for power saving
# Available: 408000 600000 816000 1008000 1200000 1416000 1608000 1800000 2000000
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
echo 1008000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null

./sync.elf &> "$LOGS_PATH/sync.txt"

# Restore defaults on exit
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
