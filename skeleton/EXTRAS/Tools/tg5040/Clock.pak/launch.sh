#!/bin/sh

cd $(dirname "$0")

# Set CPU frequency for clock app (power saving: 600 MHz)
echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed 2>/dev/null

./clock.elf &> "$LOGS_PATH/clock.txt"

# Restore default CPU governor on exit
echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
