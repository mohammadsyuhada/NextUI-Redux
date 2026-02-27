#!/bin/sh

cd "$(dirname "$0")"

# little Cortex-A55 (cpu0-3) - app runs here, cap max freq
# Available: 408000 672000 792000 936000 1032000 1128000 1224000 1320000 1416000
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
echo 1032000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null

# big Cortex-A55 (cpu4-7) - not used, set to low fixed freq
# Available: 408000 672000 840000 1008000 1200000 1344000 1488000 1584000 1680000 1800000 1992000 2088000 2160000
echo userspace > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null
echo 408000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_setspeed 2>/dev/null

./sync.elf &> "$LOGS_PATH/sync.txt"

# Restore defaults on exit
echo 1416000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
echo ondemand > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null
