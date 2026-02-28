#!/bin/sh
EMU_TAG=$(basename "$(dirname "$0")" .pak)
PAK_DIR="$(dirname "$0")"
EMU_DIR="$SDCARD_PATH/Emus/shared/mupen64plus"
ROM="$1"

mkdir -p "$SAVES_PATH/$EMU_TAG"

# Single cluster: cpu0-3 (Cortex-A53, max 2000 MHz)
echo 1 >/sys/devices/system/cpu/cpu1/online 2>/dev/null
echo 1 >/sys/devices/system/cpu/cpu2/online 2>/dev/null
echo 1 >/sys/devices/system/cpu/cpu3/online 2>/dev/null
echo performance >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo 2000000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo 1608000 >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq

# Memory management: swap + VM tuning for hi-res texture loading
SWAPFILE="/mnt/UDISK/n64_swap"
if [ ! -f "$SWAPFILE" ]; then
    dd if=/dev/zero of="$SWAPFILE" bs=1M count=512 2>/dev/null
    mkswap "$SWAPFILE" 2>/dev/null
fi
swapon "$SWAPFILE" 2>/dev/null
echo 200 >/proc/sys/vm/vfs_cache_pressure 2>/dev/null
sync
echo 3 >/proc/sys/vm/drop_caches 2>/dev/null

# User data directory (config, saves, cache)
USERDATA_DIR="$SHARED_USERDATA_PATH/N64-mupen64plus"
mkdir -p "$USERDATA_DIR/save"

# First run: copy device-specific defaults
if [ ! -f "$USERDATA_DIR/.tg5040_initialized" ]; then
    cp "$PAK_DIR/default.cfg" "$USERDATA_DIR/mupen64plus.cfg"
    touch "$USERDATA_DIR/.tg5040_initialized"
fi

export HOME="$USERDATA_DIR"
export LD_LIBRARY_PATH="$PAK_DIR:$EMU_DIR:/usr/trimui/lib:$LD_LIBRARY_PATH"

# Launch on cpu0-1, then pin threads after startup
# GLideN64's built-in OSD shows shader/texture loading progress on screen
taskset -c 0,1 "$PAK_DIR/mupen64plus" --fullscreen --resolution 1024x768 \
    --configdir "$USERDATA_DIR" \
    --datadir "$EMU_DIR" \
    --plugindir "$PAK_DIR" \
    --gfx "$EMU_DIR/mupen64plus-video-GLideN64.so" \
    --audio mupen64plus-audio-sdl.so \
    --input mupen64plus-input-sdl.so \
    --rsp mupen64plus-rsp-hle.so \
    "$ROM" &> "$LOGS_PATH/$EMU_TAG.txt" &
EMU_PID=$!
sleep 3

# Thread pinning:
#   main thread (cpu emu + RSP) → cpu0 (dedicated)
#   video thread (GLideN64)     → cpu1 (dedicated)
#   audio/mali/helpers          → cpu2-3
taskset -p -c 0 "$EMU_PID" 2>/dev/null

# Move audio/mali/helpers to cpu2-3
for TID_PATH in /proc/$EMU_PID/task/*; do
    TID=$(basename "$TID_PATH")
    [ "$TID" = "$EMU_PID" ] && continue
    TNAME=$(cat "$TID_PATH/comm" 2>/dev/null)
    case "$TNAME" in
        SDLAudioP2|SDLHotplug*|SDLTimer|mali-*|m64pwq)
            taskset -p -c 2,3 "$TID" 2>/dev/null ;;
    esac
done

# Identify video thread by measuring utime delta over 2 seconds
for TID_PATH in /proc/$EMU_PID/task/*; do
    TID=$(basename "$TID_PATH")
    [ "$TID" = "$EMU_PID" ] && continue
    TNAME=$(cat "$TID_PATH/comm" 2>/dev/null)
    [ "$TNAME" = "mupen64plus" ] && eval "SNAP1_$TID=$(awk '{print $14}' "$TID_PATH/stat" 2>/dev/null)"
done
sleep 2
MAX_DELTA=0
VIDEO_TID=""
for TID_PATH in /proc/$EMU_PID/task/*; do
    TID=$(basename "$TID_PATH")
    [ "$TID" = "$EMU_PID" ] && continue
    TNAME=$(cat "$TID_PATH/comm" 2>/dev/null)
    if [ "$TNAME" = "mupen64plus" ]; then
        UTIME2=$(awk '{print $14}' "$TID_PATH/stat" 2>/dev/null)
        eval "UTIME1=\${SNAP1_$TID:-0}"
        DELTA=$((${UTIME2:-0} - ${UTIME1:-0}))
        if [ "$DELTA" -gt "$MAX_DELTA" ]; then
            MAX_DELTA=$DELTA
            VIDEO_TID=$TID
        fi
    fi
done
[ -n "$VIDEO_TID" ] && taskset -p -c 1 "$VIDEO_TID" 2>/dev/null

wait $EMU_PID

# Cleanup: disable swap, restore VM defaults
swapoff "$SWAPFILE" 2>/dev/null
echo 100 >/proc/sys/vm/vfs_cache_pressure 2>/dev/null
