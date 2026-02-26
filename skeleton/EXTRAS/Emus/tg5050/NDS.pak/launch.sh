#!/bin/sh
set -eo pipefail
set -x

rm -f "$LOGS_PATH/NDS.txt"
exec >>"$LOGS_PATH/NDS.txt"
exec 2>&1

echo "$0" "$@"

EMU_DIR="$SDCARD_PATH/Emus/shared/Drastic"
PAK_DIR="$(dirname "$0")"

# Hook code needs libjson-c.so.4 but device ships libjson-c.so.5
if [ ! -f "$EMU_DIR/libs/libjson-c.so.4" ] && [ -f /usr/lib/libjson-c.so.5 ]; then
    mkdir -p "$EMU_DIR/libs"
    cp /usr/lib/libjson-c.so.5 "$EMU_DIR/libs/libjson-c.so.4"
fi

export PATH="$EMU_DIR:$PATH"
# NDS.pak libs first (DRM-patched SDL2, libadvdrastic), then shared libs
export LD_LIBRARY_PATH="$PAK_DIR/libs:$EMU_DIR/libs:$LD_LIBRARY_PATH"

cleanup() {
    rm -f /tmp/stay_awake
    umount "$EMU_DIR/config" 2>/dev/null || true
    umount "$EMU_DIR/backup" 2>/dev/null || true
    umount "$EMU_DIR/cheats" 2>/dev/null || true
    umount "$EMU_DIR/savestates" 2>/dev/null || true

    # Restore CPU governor (LITTLE cpu0 + BIG cpu4)
    cpu_state="$USERDATA_PATH/NDS-drastic"
    if [ -f "$cpu_state/cpu0_governor.txt" ]; then
        cat "$cpu_state/cpu0_governor.txt" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
        cat "$cpu_state/cpu0_min_freq.txt" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null
        cat "$cpu_state/cpu0_max_freq.txt" >/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null
        rm -f "$cpu_state/cpu0_governor.txt" "$cpu_state/cpu0_min_freq.txt" "$cpu_state/cpu0_max_freq.txt"
    fi
    if [ -f "$cpu_state/cpu4_governor.txt" ]; then
        cat "$cpu_state/cpu4_governor.txt" >/sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
        cat "$cpu_state/cpu4_min_freq.txt" >/sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq 2>/dev/null
        cat "$cpu_state/cpu4_max_freq.txt" >/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq 2>/dev/null
        rm -f "$cpu_state/cpu4_governor.txt" "$cpu_state/cpu4_min_freq.txt" "$cpu_state/cpu4_max_freq.txt"
    fi
}

main() {
    echo "1" >/tmp/stay_awake
    trap "cleanup" EXIT INT TERM HUP QUIT

    # Save current CPU state and set performance mode (LITTLE cpu0 + BIG cpu4)
    cpu_state="$USERDATA_PATH/NDS-drastic"
    mkdir -p "$cpu_state"

    # LITTLE cores (Cortex-A55 cpu0): max 1416000
    cpu0_dir="/sys/devices/system/cpu/cpu0/cpufreq"
    cat "$cpu0_dir/scaling_governor" >"$cpu_state/cpu0_governor.txt"
    cat "$cpu0_dir/scaling_min_freq" >"$cpu_state/cpu0_min_freq.txt"
    cat "$cpu0_dir/scaling_max_freq" >"$cpu_state/cpu0_max_freq.txt"
    echo performance >"$cpu0_dir/scaling_governor"
    echo 1416000 >"$cpu0_dir/scaling_min_freq"
    echo 1416000 >"$cpu0_dir/scaling_max_freq"

    # BIG cores (Cortex-A55 cpu4): max 2160000
    cpu4_dir="/sys/devices/system/cpu/cpu4/cpufreq"
    cat "$cpu4_dir/scaling_governor" >"$cpu_state/cpu4_governor.txt"
    cat "$cpu4_dir/scaling_min_freq" >"$cpu_state/cpu4_min_freq.txt"
    cat "$cpu4_dir/scaling_max_freq" >"$cpu_state/cpu4_max_freq.txt"
    echo performance >"$cpu4_dir/scaling_governor"
    echo 1800000 >"$cpu4_dir/scaling_min_freq"
    echo 2160000 >"$cpu4_dir/scaling_max_freq"

    # Setup external directories
    mkdir -p "$SDCARD_PATH/Saves/NDS"
    mkdir -p "$SDCARD_PATH/Cheats/NDS"
    mkdir -p "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config"
    mkdir -p "$EMU_DIR/backup"
    mkdir -p "$EMU_DIR/cheats"
    mkdir -p "$EMU_DIR/config"
    mkdir -p "$EMU_DIR/savestates"

    # Apply device-specific config (only on first run, don't overwrite user changes)
    if [ ! -f "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config/.tg5050_initialized" ]; then
        cp "$PAK_DIR/config/drastic.cfg" "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config/" 2>/dev/null || true
        cp "$PAK_DIR/config/drastic.cf2" "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config/" 2>/dev/null || true
        if [ -f "$PAK_DIR/settings.json" ]; then
            cp "$PAK_DIR/settings.json" "$EMU_DIR/resources/" 2>/dev/null || true
        fi
        touch "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config/.tg5050_initialized"
    fi

    # Move any leftover cheats to centralized location
    if [ -d "$EMU_DIR/cheats" ] && ls -A "$EMU_DIR/cheats" 2>/dev/null | grep -q .; then
        cd "$EMU_DIR/cheats"
        mv * "$SDCARD_PATH/Cheats/NDS/" || true
    fi

    # Bind-mount external locations into drastic directory
    mount -o bind "$SHARED_USERDATA_PATH/NDS-advanced-drastic/config" "$EMU_DIR/config"
    mount -o bind "$SDCARD_PATH/Saves/NDS" "$EMU_DIR/backup"
    mount -o bind "$SDCARD_PATH/Cheats/NDS" "$EMU_DIR/cheats"
    mount -o bind "$SHARED_USERDATA_PATH/NDS-advanced-drastic" "$EMU_DIR/savestates"

    # Launch drastic — use dummy video driver (DRM rendering handled by hook patch)
    cd "$EMU_DIR"
    export HOME="$EMU_DIR"
    export SDL_VIDEODRIVER=dummy
    export SDL_AUDIODRIVER=alsa
    export SDL_AUDIO_BUFFER_SIZE=2048

    # Mute speaker before launch to prevent audio pop, then unmute after init
    amixer cset numid=27 0 >/dev/null 2>&1 || true
    amixer cset numid=28 0 >/dev/null 2>&1 || true
    (sleep 2; amixer cset numid=27 1 >/dev/null 2>&1; amixer cset numid=28 1 >/dev/null 2>&1; syncsettings.elf) &
    SYNC_PID=$!

    "$EMU_DIR/drastic" "$*"

    kill $SYNC_PID 2>/dev/null || true
    amixer cset numid=27 1 >/dev/null 2>&1 || true
    amixer cset numid=28 1 >/dev/null 2>&1 || true
}

main "$@"
