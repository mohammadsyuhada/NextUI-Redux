#!/bin/sh
# Mute speaker amp to suppress pop during SDL audio init
amixer cset numid=30 0 >/dev/null 2>&1
amixer cset numid=29 0 >/dev/null 2>&1

PAK_DIR="$(dirname "$0")"
PAK_NAME="$(basename "$PAK_DIR")"
PAK_NAME="${PAK_NAME%.*}"

rm -f "$LOGS_PATH/$PAK_NAME.txt"
exec >>"$LOGS_PATH/$PAK_NAME.txt"
exec 2>&1
[ -f "$USERDATA_PATH/PORTS-portmaster/debug" ] && set -x

echo "$0" "$*"
cd "$PAK_DIR" || exit 1
mkdir -p "$USERDATA_PATH/PORTS-portmaster"
mkdir -p "$SHARED_USERDATA_PATH/PORTS-portmaster"

EMU_DIR="$SDCARD_PATH/Emus/shared/PortMaster"
BB="$SHARED_SYSTEM_PATH/bin/busybox"

export PATH="$EMU_DIR:$SHARED_SYSTEM_PATH/bin:$PATH"

# tg5050 ships newer lib versions than what some bundled binaries expect
# (can't symlink on exFAT, so copy the actual files)
for lib_pair in "libffi.so.7 libffi.so.8" "libncurses.so.5 libncurses.so.6" "libncursesw.so.5 libncursesw.so.6"; do
    old="${lib_pair% *}" new="${lib_pair#* }"
    [ ! -e "$SHARED_SYSTEM_PATH/lib/$old" ] && [ -e "/usr/lib/$new" ] && \
        cp "/usr/lib/$new" "$SHARED_SYSTEM_PATH/lib/$old"
done

export LD_LIBRARY_PATH="$SHARED_SYSTEM_PATH/lib:/usr/trimui/lib:$LD_LIBRARY_PATH"
export SSL_CERT_FILE="$SHARED_SYSTEM_PATH/etc/ssl/certs/ca-certificates.crt"
export SDL_GAMECONTROLLERCONFIG_FILE="$EMU_DIR/gamecontrollerdb.txt"
export PYSDL2_DLL_PATH="/usr/trimui/lib"
export HOME="$SHARED_USERDATA_PATH/PORTS-portmaster"
export XDG_DATA_HOME="$HOME/.local/share"
mkdir -p "$XDG_DATA_HOME"

# Bind mount so port scripts find PortMaster via XDG_DATA_HOME detection
# Port scripts check: elif [ -d "$XDG_DATA_HOME/PortMaster/" ]
# (symlinks don't work on FAT32/exFAT SD cards)
if ! mountpoint -q "$XDG_DATA_HOME/PortMaster" 2>/dev/null; then
    mkdir -p "$XDG_DATA_HOME/PortMaster"
    mount -o bind "$EMU_DIR" "$XDG_DATA_HOME/PortMaster"
fi

[ -z "$1" ] && exit 1
ROM_PATH="$1"
ROM_DIR="$(dirname "$ROM_PATH")"
ROM_NAME="$(basename "$ROM_PATH")"
TEMP_DATA_DIR="$SDCARD_PATH/.ports_temp"
PORTS_DIR="$ROM_DIR/.ports"

export controlfolder="$EMU_DIR"
export PM_SCRIPTNAME="$ROM_NAME"

export HM_TOOLS_DIR="$SDCARD_PATH/Emus/shared"
export HM_PORTS_DIR="$TEMP_DATA_DIR/ports"
export HM_SCRIPTS_DIR="$TEMP_DATA_DIR/ports"

# Create busybox wrapper scripts for commands that ports need
# The system busybox (v1.27.2) is too old and missing many applets
create_busybox_wrappers() {
    bin_dir="$SHARED_SYSTEM_PATH/bin"
    [ -f "$bin_dir/busybox_wrappers.done" ] && return 0
    echo "Creating busybox wrappers in $bin_dir"

    created=""
    for cmd in $("$BB" --list); do
        case "$cmd" in sh) continue ;; esac
        # Only create wrapper if no existing binary (don't override python3, bash, etc.)
        if [ ! -e "$bin_dir/$cmd" ]; then
            printf '#!/bin/sh\nexec %s %s "$@"\n' "$BB" "$cmd" > "$bin_dir/$cmd"
            created="$created $bin_dir/$cmd"
        fi
    done

    # Batch chmod instead of per-file
    [ -n "$created" ] && chmod +x $created

    touch "$bin_dir/busybox_wrappers.done"
}

cleanup() {
    # Always attempt unmount before cleanup
    # (paths in /proc/mounts may differ from env vars due to symlinks)
    umount "$XDG_DATA_HOME/PortMaster" 2>/dev/null || true
    umount "$TEMP_DATA_DIR/ports" 2>/dev/null || true
    rm -rf "$TEMP_DATA_DIR" 2>/dev/null || true

    # Restore CPU governor (LITTLE cpu0 + BIG cpu4)
    cpu_state="$USERDATA_PATH/PORTS-portmaster"
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

patch_control_txt() {
    # Skip if already patched for NextUI
    grep -q "# Patched for NextUI v2" "$EMU_DIR/control.txt" 2>/dev/null && return 0
    cat > "$EMU_DIR/control.txt" << CONTROL_EOF
#!/bin/sh
#
# SPDX-License-Identifier: MIT
#
# Patched for NextUI v2

CUR_TTY=/dev/tty0

export controlfolder="$EMU_DIR"
export directory="mnt/SDCARD/.ports_temp"

PM_SCRIPTNAME="\$(basename "\${PM_SCRIPTNAME:-\$0}")"
PM_PORTNAME="\${PM_SCRIPTNAME%%.sh}"

if [ -z "\$PM_PORTNAME" ]; then
  PM_PORTNAME="Port"
fi

export ESUDO=""
export ESUDOKILL="-1"
export SDL_GAMECONTROLLERCONFIG_FILE="\$controlfolder/gamecontrollerdb.txt"

get_controls() {
  sleep 0.5
}

. \$controlfolder/device_info.txt
. \$controlfolder/funcs.txt

export GPTOKEYB2="\$ESUDO env LD_PRELOAD=\$controlfolder/libinterpose.aarch64.so \$controlfolder/gptokeyb2 \$ESUDOKILL"
export GPTOKEYB="\$ESUDO \$controlfolder/gptokeyb \$ESUDOKILL"
CONTROL_EOF
}

set_controller_layout() {
    # TRIMUI Player1 GUID (Bus=0003 Vendor=045e Product=028e Version=0114)
    local TRIMUI_GUID="030000005e0400008e02000014010000"
    local dest="$EMU_DIR/gamecontrollerdb.txt"

    case "$1" in
        nintendo)
            local src="$PAK_DIR/files/gamecontrollerdb_nintendo.txt"
            if [ -f "$src" ]; then
                # Only copy if different
                cmp -s "$src" "$dest" || cp -f "$src" "$dest"
            fi
            # Set env var (highest priority) to swap A/B and X/Y
            export SDL_GAMECONTROLLERCONFIG="${TRIMUI_GUID},TRIMUI Player1,a:b1,b:b0,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b3,y:b2,platform:Linux,"
            ;;
        xbox)
            local src="$PAK_DIR/files/gamecontrollerdb_xbox.txt"
            if [ -f "$src" ]; then
                cmp -s "$src" "$dest" || cp -f "$src" "$dest"
            fi
            # Clear any Nintendo override — default Xbox mapping is a:b0,b:b1
            unset SDL_GAMECONTROLLERCONFIG
            ;;
    esac
}

fix_port_scripts() {
    # Replace hardcoded paths and fix shebangs in a single sed pass per file
    "$BB" find "$1" -maxdepth 1 -type f \( -name "*.sh" -o -name "*.src" -o -name "*.txt" \) | while IFS= read -r file; do
        if grep -qE "/roms/ports/PortMaster|^#!/bin/bash" "$file" 2>/dev/null; then
            echo "Fixing: $file"
            sed -i -e "s|/roms/ports/PortMaster|$EMU_DIR|g" \
                   -e '1s|^#!/bin/bash|#!/usr/bin/env bash|' "$file"
        fi
    done
}

main() {
    echo "1" >/tmp/stay_awake
    trap "cleanup" EXIT INT TERM HUP QUIT

    # Create busybox wrappers (mount, umount, find, etc.) on first run
    create_busybox_wrappers

    # Save current CPU state and set performance mode (LITTLE cpu0 + BIG cpu4)
    cpu_state="$USERDATA_PATH/PORTS-portmaster"

    # LITTLE cores (Cortex-A55 cpu0): 408000-1416000
    cpu0_dir="/sys/devices/system/cpu/cpu0/cpufreq"
    cat "$cpu0_dir/scaling_governor" >"$cpu_state/cpu0_governor.txt"
    cat "$cpu0_dir/scaling_min_freq" >"$cpu_state/cpu0_min_freq.txt"
    cat "$cpu0_dir/scaling_max_freq" >"$cpu_state/cpu0_max_freq.txt"
    echo performance >"$cpu0_dir/scaling_governor"
    echo 1416000 >"$cpu0_dir/scaling_min_freq"
    echo 1416000 >"$cpu0_dir/scaling_max_freq"

    # BIG cores (Cortex-A55 cpu4): 408000-2160000
    cpu4_dir="/sys/devices/system/cpu/cpu4/cpufreq"
    cat "$cpu4_dir/scaling_governor" >"$cpu_state/cpu4_governor.txt"
    cat "$cpu4_dir/scaling_min_freq" >"$cpu_state/cpu4_min_freq.txt"
    cat "$cpu4_dir/scaling_max_freq" >"$cpu_state/cpu4_max_freq.txt"
    echo performance >"$cpu4_dir/scaling_governor"
    echo 1800000 >"$cpu4_dir/scaling_min_freq"
    echo 2160000 >"$cpu4_dir/scaling_max_freq"

    mkdir -p "$PORTS_DIR"

    # Mount bind .ports to temp dir so port scripts can find game data
    # Port scripts do: cd /$directory/ports/<game>
    # This maps /mnt/SDCARD/.ports_temp/ports -> $ROM_DIR/.ports
    if ! mountpoint -q "$TEMP_DATA_DIR/ports" 2>/dev/null; then
        mkdir -p "$TEMP_DATA_DIR/ports"
        if ! mount -o bind "$PORTS_DIR" "$TEMP_DATA_DIR/ports"; then
            echo "Failed to mount $PORTS_DIR to $TEMP_DATA_DIR/ports"
            exit 1
        fi
    fi

    # Fix hardcoded paths in port script if needed
    if grep -q "/roms/ports/PortMaster" "$ROM_PATH" 2>/dev/null; then
        fix_port_scripts "$ROM_DIR"
    fi

    # Ensure control.txt has correct NextUI paths
    # (PortMaster's init may have overwritten it with default TrimUI paths)
    patch_control_txt

    # Apply user's chosen button layout (default: nintendo)
    if [ -f "$SHARED_USERDATA_PATH/PORTS-portmaster/xbox_layout" ]; then
        set_controller_layout xbox
    else
        set_controller_layout nintendo
    fi

    echo "Starting port: $ROM_PATH"
    cd "$ROM_DIR"

    # Unmute speaker after game audio has initialized
    (sleep 5; amixer cset numid=30 1 >/dev/null 2>&1; amixer cset numid=29 1 >/dev/null 2>&1; syncsettings.elf) &
    SYNC_PID=$!

    bash "$ROM_PATH"

    kill $SYNC_PID 2>/dev/null || true
    amixer cset numid=30 1 >/dev/null 2>&1 || true
    amixer cset numid=29 1 >/dev/null 2>&1 || true
}

main "$@"
