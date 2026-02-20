#!/bin/sh

SDCARD_PATH="/mnt/SDCARD"
USERDATA_PATH="$SDCARD_PATH/.userdata"
SHARED_USERDATA_PATH="$USERDATA_PATH/shared"

cd $(dirname "$0")

# Set CPU frequency for settings menu (power saving: 672 MHz)
echo userspace > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null
echo 672000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_setspeed 2>/dev/null

./settings.elf > settings.log 2>&1

# Restore default CPU governor on exit
echo ondemand > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor 2>/dev/null
