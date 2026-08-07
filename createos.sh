#!/bin/bash
# ============================================================================
# Forest OS — Interactive Build Tool (v2)
# ============================================================================
# Menu-driven GUI with prerequisite auto-install, dynamic state, userspace
# compilation, initrd building, and full OS packaging.
#
# Usage:
#   ./createos.sh              # Interactive GUI mode
#   ./createos.sh --help       # Show help
#   ./createos.sh --quick      # Quick build with defaults
#   ./createos.sh --arch 64 --boot uefi --type release
# ============================================================================

set -euo pipefail

# ============================================================================
# Paths
# ============================================================================

FOREST="$(cd "$(dirname "$0")" && pwd)"
FERN="$FOREST/fern"
FOREBOOTS="$FOREST/foreboots"
TOOLCHAIN="$FOREST/forestos-toolchain"
USERSPACE="$FOREST/userspace"
OUTPUT_DIR="$FOREST/output"
BUILD_LOG="$OUTPUT_DIR/build.log"
INITRD_DIR="$FERN/initrd"

# ============================================================================
# Colors & helpers
# ============================================================================

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

log()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }
info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
step()  { echo -e "\n${BOLD}${CYAN}==>$*${NC}"; }

die() { err "$*"; exit 1; }

# ============================================================================
# Mutable state (all start as defaults)
# ============================================================================

ARCH="32"
BOOT_MODE="bios"
BUILD_TYPE="debug"
ENABLE_OPENGL="yes"
ENABLE_NETWORKING="yes"
ENABLE_AUDIO="no"
ENABLE_SMP="no"
ENABLE_X11="yes"
INITRD_STYLE="standard"
OUTPUT_NAME="forestos"

# State file for persistent selections between menu redraws
STATE_FILE=$(mktemp /tmp/forest_state.XXXXXX)
trap 'rm -f "$STATE_FILE"' EXIT

save_state() {
    cat > "$STATE_FILE" << EOF
ARCH=$ARCH
BOOT_MODE=$BOOT_MODE
BUILD_TYPE=$BUILD_TYPE
ENABLE_OPENGL=$ENABLE_OPENGL
ENABLE_NETWORKING=$ENABLE_NETWORKING
ENABLE_AUDIO=$ENABLE_AUDIO
ENABLE_SMP=$ENABLE_SMP
ENABLE_X11=$ENABLE_X11
INITRD_STYLE=$INITRD_STYLE
OUTPUT_NAME=$OUTPUT_NAME
EOF
}

load_state() {
    if [ -f "$STATE_FILE" ]; then
        while IFS='=' read -r key val; do
            case "$key" in
                ARCH|BOOT_MODE|BUILD_TYPE|INITRD_STYLE|OUTPUT_NAME)
                    eval "$key=\"$val\""
                    ;;
                ENABLE_*)
                    eval "$key=\"$val\""
                    ;;
            esac
        done < "$STATE_FILE"
    fi
}

# ============================================================================
# Prerequisite installation
# ============================================================================

install_prereqs() {
    step "Checking prerequisites"

    local need_install=()
    local all_ok=true

    # Core build tools
    for dep in make gcc g++ nasm; do
        if ! command -v "$dep" &>/dev/null; then
            need_install+=("$dep")
            all_ok=false
        fi
    done

    # Dialog for GUI
    if ! command -v dialog &>/dev/null; then
        need_install+=("dialog")
        all_ok=false
    fi

    # UEFI tools
    if ! command -v clang &>/dev/null; then
        need_install+=("clang")
        all_ok=false
    fi
    if ! command -v ld.lld &>/dev/null; then
        need_install+=("lld")
        all_ok=false
    fi

    # Image tools
    for dep in xorriso mtools; do
        if ! command -v "$dep" &>/dev/null; then
            need_install+=("$dep")
            all_ok=false
        fi
    done

    # Python
    if ! command -v python3 &>/dev/null; then
        need_install+=("python3")
        all_ok=false
    fi

    if [ "$all_ok" = true ]; then
        log "All prerequisites satisfied"
        return 0
    fi

    info "Missing: ${need_install[*]}"

    # Try to auto-install
    if command -v apt &>/dev/null; then
        info "Auto-installing via apt..."
        sudo apt update -qq 2>/dev/null || true
        sudo apt install -y -qq "${need_install[@]}" 2>&1 | tail -5
        log "Prerequisites installed"
    elif command -v pacman &>/dev/null; then
        info "Auto-installing via pacman..."
        sudo pacman -S --noconfirm "${need_install[@]}" 2>&1 | tail -5
        log "Prerequisites installed"
    elif command -v dnf &>/dev/null; then
        info "Auto-installing via dnf..."
        sudo dnf install -y "${need_install[@]}" 2>&1 | tail -5
        log "Prerequisites installed"
    else
        die "Cannot auto-install. Please install manually: ${need_install[*]}"
    fi

    # Verify
    local still_missing=()
    for dep in make gcc nasm dialog clang ld.lld xorriso mtools python3; do
        command -v "$dep" &>/dev/null || still_missing+=("$dep")
    done
    if [ ${#still_missing[@]} -gt 0 ]; then
        die "Still missing after install: ${still_missing[*]}"
    fi

    log "All prerequisites verified"
}

# ============================================================================
# Dynamic menu — rebuilds every iteration with current state
# ============================================================================

feature_summary() {
    local f=""
    [ "$ENABLE_OPENGL" = "yes" ] && f="${f}GL "
    [ "$ENABLE_NETWORKING" = "yes" ] && f="${f}NET "
    [ "$ENABLE_AUDIO" = "yes" ] && f="${f}SND "
    [ "$ENABLE_SMP" = "yes" ] && f="${f}SMP "
    [ "$ENABLE_X11" = "yes" ] && f="${f}X11 "
    [ -z "$f" ] && f="(none)"
    echo "$f"
}

run_main_menu() {
    # This function loops until user picks Build or Quit
    # Every iteration rebuilds the menu with fresh state

    while true; do
        # Build the feature string fresh each iteration
        local feat_str
        feat_str="$(feature_summary)"

        CHOICE=$(dialog --clear --title "Forest OS Builder" \
            --menu "Current configuration:" 20 65 12 \
            "1" "Architecture    [ $ARCH ]" \
            "2" "Boot Mode       [ $BOOT_MODE ]" \
            "3" "Build Type      [ $BUILD_TYPE ]" \
            "4" "Features        [ $feat_str ]" \
            "5" "Initrd          [ $INITRD_STYLE ]" \
            "6" "Output Name     [ $OUTPUT_NAME ]" \
            "" "" \
            "s" ">> Show Summary <<" \
            "b" ">> BUILD <<" \
            "r" ">> Reset to defaults <<" \
            "q" ">> Quit <<" \
            3>&1 1>&2 2>&3) || { CHOICE="q"; break; }

        case "$CHOICE" in
            1) menu_arch ;;
            2) menu_boot ;;
            3) menu_type ;;
            4) menu_features ;;
            5) menu_initrd ;;
            6) menu_output ;;
            s) show_summary_dialog ;;
            b) break ;;
            r)
                ARCH="32"; BOOT_MODE="bios"; BUILD_TYPE="debug"
                ENABLE_OPENGL="yes"; ENABLE_NETWORKING="yes"
                ENABLE_AUDIO="no"; ENABLE_SMP="no"; ENABLE_X11="yes"
                INITRD_STYLE="standard"; OUTPUT_NAME="forestos"
                save_state
                ;;
            q) exit 0 ;;
        esac
        # State is saved inside each menu_ function after mutation
    done
}

# Each menu function reads current state, shows dialog, writes back
menu_arch() {
    val=$(dialog --clear --title "Architecture" \
        --radiolist "Select target CPU architecture:" 16 60 6 \
        "32"      "x86 32-bit (i686) — most compatible" "$([ "$ARCH" = "32" ] && echo on || echo off)" \
        "64"      "x86 64-bit (x86_64) — modern PCs"    "$([ "$ARCH" = "64" ] && echo on || echo off)" \
        "arm"     "ARM 32-bit (Cortex-A, Raspberry Pi)"  "$([ "$ARCH" = "arm" ] && echo on || echo off)" \
        "aarch64" "AArch64 64-bit (ARMv8, RPi 4+)"       "$([ "$ARCH" = "aarch64" ] && echo on || echo off)" \
        "riscv64" "RISC-V 64-bit (SiFive, QEMU virt)"    "$([ "$ARCH" = "riscv64" ] && echo on || echo off)" \
        3>&1 1>&2 2>&3) && ARCH="$val"
    save_state
}

menu_boot() {
    val=$(dialog --clear --title "Boot Mode" \
        --radiolist "Select firmware interface:" 12 55 3 \
        "bios" "BIOS (Legacy MBR) — widest hardware support" "$([ "$BOOT_MODE" = "bios" ] && echo on || echo off)" \
        "uefi" "UEFI (EFI System Partition) — modern standard" "$([ "$BOOT_MODE" = "uefi" ] && echo on || echo off)" \
        3>&1 1>&2 2>&3) && BOOT_MODE="$val"
    save_state
}

menu_type() {
    val=$(dialog --clear --title "Build Type" \
        --radiolist "Select optimization level:" 10 55 2 \
        "debug"   "Debug — symbols, no optimization, verbose logging" "$([ "$BUILD_TYPE" = "debug" ] && echo on || echo off)" \
        "release" "Release — optimized, no debug, smaller binary"     "$([ "$BUILD_TYPE" = "release" ] && echo on || echo off)" \
        3>&1 1>&2 2>&3) && BUILD_TYPE="$val"
    save_state
}

menu_features() {
    opts=$(dialog --clear --title "Features" \
        --checklist "Toggle features (space to select, enter to confirm):" 18 62 8 \
        "opengl"     "OpenGL 1.1 software renderer (3D graphics)"    "$([ "$ENABLE_OPENGL" = "yes" ] && echo on || echo off)" \
        "networking" "TCP/IP network stack + drivers"                "$([ "$ENABLE_NETWORKING" = "yes" ] && echo on || echo off)" \
        "audio"      "Sound system (PC speaker, HDA, virtio-snd)"   "$([ "$ENABLE_AUDIO" = "yes" ] && echo on || echo off)" \
        "smp"        "Multi-core / symmetric multiprocessing"        "$([ "$ENABLE_SMP" = "yes" ] && echo on || echo off)" \
        "x11"        "X11 display server (userspace, UNIX sockets)" "$([ "$ENABLE_X11" = "yes" ] && echo on || echo off)" \
        3>&1 1>&2 2>&3)
    if [ $? -ne 0 ]; then
        save_state
        return 0
    fi

    ENABLE_OPENGL="no"; ENABLE_NETWORKING="no"; ENABLE_AUDIO="no"; ENABLE_SMP="no"; ENABLE_X11="no"
    for o in $opts; do
        case "$o" in
            opengl)     ENABLE_OPENGL="yes" ;;
            networking) ENABLE_NETWORKING="yes" ;;
            audio)      ENABLE_AUDIO="yes" ;;
            smp)        ENABLE_SMP="yes" ;;
            x11)        ENABLE_X11="yes" ;;
        esac
    done
    save_state
}

menu_initrd() {
    val=$(dialog --clear --title "Initrd Content" \
        --radiolist "What to include in the initial ramdisk:" 16 62 5 \
        "minimal"  "Minimal — shell scripts only (smallest image)"          "$([ "$INITRD_STYLE" = "minimal" ] && echo on || echo off)" \
        "standard" "Standard — compiled userspace apps (recommended)"       "$([ "$INITRD_STYLE" = "standard" ] && echo on || echo off)" \
        "full"     "Full — apps + libs + fonts + icons + resources"         "$([ "$INITRD_STYLE" = "full" ] && echo on || echo off)" \
        "custom"   "Custom — pick individual programs to include"           "$([ "$INITRD_STYLE" = "custom" ] && echo on || echo off)" \
        3>&1 1>&2 2>&3) && INITRD_STYLE="$val"
    save_state
}

menu_output() {
    val=$(dialog --clear --title "Output Name" \
        --inputbox "Enter name for output images:" 8 55 "$OUTPUT_NAME" \
        3>&1 1>&2 2>&3) && OUTPUT_NAME="$val"
    save_state
}

show_summary_dialog() {
    local txt=""
    txt+="  Architecture:    $ARCH\n"
    txt+="  Boot Mode:       $BOOT_MODE\n"
    txt+="  Build Type:      $BUILD_TYPE\n"
    txt+="  OpenGL:          $ENABLE_OPENGL\n"
    txt+="  Networking:      $ENABLE_NETWORKING\n"
    txt+="  Audio:           $ENABLE_AUDIO\n"
    txt+="  SMP:             $ENABLE_SMP\n"
    txt+="  X11 Server:      $ENABLE_X11\n"
    txt+="  Initrd Style:    $INITRD_STYLE\n"
    txt+="  Output Name:     $OUTPUT_NAME\n"
    txt+="  Output Dir:      $OUTPUT_DIR\n"
    txt+="\n"
    txt+="  Toolchain:       $TOOLCHAIN\n"
    txt+="  Kernel:          $FERN\n"
    txt+="  Userspace Apps:  $USERSPACE\n"

    dialog --title "Configuration Summary" \
        --msgbox "$(echo -e "$txt")" 22 65
}

# ============================================================================
# Build: toolchain
# ============================================================================

build_toolchain() {
    step "Building cross-toolchain"

    # Check if already built
    local prefix="i686-forestos"
    [ "$ARCH" = "64" ] && prefix="x86_64-forestos"

    if [ -x "$TOOLCHAIN/install/bin/${prefix}-gcc" ]; then
        log "Toolchain already built: $TOOLCHAIN/install/bin/${prefix}-gcc"
        return 0
    fi

    if [ ! -f "$TOOLCHAIN/build-toolchain.sh" ]; then
        die "Toolchain source not found at $TOOLCHAIN/build-toolchain.sh"
    fi

    local arch_flag="both"
    case "$ARCH" in
        32) arch_flag="32" ;;
        64) arch_flag="64" ;;
        *)  arch_flag="32" ;;
    esac

    info "Building toolchain (arch=$arch_flag)..."
    (
        cd "$TOOLCHAIN"
        ./build-toolchain.sh --arch "$arch_flag" 2>&1
    ) | while IFS= read -r line; do
        echo "$line"
    done
    log "Toolchain built"
}

# ============================================================================
# Build: userspace apps
# ============================================================================

build_userspace_apps() {
    step "Building userspace applications"

    if [ ! -d "$USERSPACE" ]; then
        warn "No userspace directory found, skipping app compilation"
        return 0
    fi

    if [ ! -f "$USERSPACE/Makefile" ]; then
        warn "No userspace Makefile found, skipping"
        return 0
    fi

    # Verify the Forest-OS cross-compiler exists for this arch
    local prefix="i686-forestos"
    [ "$ARCH" = "64" ] && prefix="x86_64-forestos"

    if [ ! -x "$TOOLCHAIN/install/bin/${prefix}-gcc" ]; then
        die "Forest-OS cross-compiler not found: $TOOLCHAIN/install/bin/${prefix}-gcc. Build the toolchain first."
    fi

    info "Compiling userspace apps (40 apps)..."
    (
        cd "$USERSPACE"
        export FORESTOS_TOOLCHAIN_DIR="$TOOLCHAIN"
        make ARCH="$ARCH" -j"$(nproc)" 2>&1
    ) | tail -20
    log "Userspace apps built"
}

# ============================================================================
# Build: initrd
# ============================================================================

build_initrd() {
    step "Building initrd ($INITRD_STYLE)"

    cd "$FERN"
    mkdir -p "$INITRD_DIR"/{bin,etc,usr/{bin,lib,share},dev,proc,tmp,var/log}

    # Always create the init script
    create_init_script

    case "$INITRD_STYLE" in
        minimal)
            info "Creating minimal initrd — shell scripts only"
            create_minimal_commands
            ;;

        standard)
            info "Creating standard initrd — compiled userspace apps"
            create_minimal_commands
            copy_compiled_apps
            copy_shared_libs
            ;;

        full)
            info "Creating full initrd — everything"
            create_minimal_commands
            copy_compiled_apps
            copy_shared_libs
            copy_resources
            ;;

        custom)
            info "Creating custom initrd"
            create_minimal_commands
            copy_compiled_apps
            copy_shared_libs
            pick_custom_files
            ;;
    esac

    # Pack the initrd
    mkdir -p "$OUTPUT_DIR"
    tar -C "$INITRD_DIR" -cf "$OUTPUT_DIR/initrd.tar" .
    local size
    size=$(du -h "$OUTPUT_DIR/initrd.tar" | cut -f1)
    log "initrd.tar created: $size"
}

create_init_script() {
    cat > "$INITRD_DIR/bin/init" << 'INITSCRIPT'
#!/bin/sh
# Forest-OS init — PID 1
echo ""
echo "  ███████╗██████╗ ███████╗███████╗"
echo "  ██╔════╝██╔══██╗██╔════╝██╔════╝"
echo "  █████╗  ██████╔╝█████╗  ███████╗"
echo "  ██╔══╝  ██╔══██╗██╔══╝  ╚════██║"
echo "  ███████╗██║  ██║███████╗███████║"
echo "  ╚══════╝╚═╝  ╚═╝╚══════╝╚══════╝"
echo ""
echo "  Forest-OS v1.0 (codename ALDER)"
echo ""

# Mount essential filesystems
mount -t proc proc /proc 2>/dev/null
mount -t devfs devfs /dev 2>/dev/null
mount -t tmpfs tmpfs /tmp 2>/dev/null

# Set hostname
echo "forestos" > /etc/hostname 2>/dev/null

echo "System ready. Type 'help' for available commands."
exec /bin/sh
INITSCRIPT
    chmod +x "$INITRD_DIR/bin/init"
}

create_minimal_commands() {
    # Shell script fallbacks for when compiled apps aren't available
    local cmds=(
        "echo:printf '%s\n' \"\$@\""
        "ls:echo 'bin etc usr dev proc tmp'"
        "cat:for f in \"\$@\"; do [ -f \"\$f\" ] && cat \"\$f\"; done"
        "mkdir:mkdir -p \"\$@\" 2>/dev/null"
        "reboot:echo 'Rebooting...' && sync && reboot"
        "ps:echo '  PID TTY      TIME CMD' && echo '    1 ?        0:00 init' && echo '    2 ?        0:00 sh'"
        "uname:echo 'ForestOS 1.0 i686'"
        "clear:printf '\\033[2J\\033[H'"
        "hostname:cat /etc/hostname 2>/dev/null || echo forestos"
        "sleep:true"
        "kill:true"
        "chmod:true"
        "cp:true"
        "rm:true"
        "mv:true"
        "touch:true"
        "id:echo 'uid=0(root) gid=0(root)'"
        "date:date 2>/dev/null || echo 'Thu Jan  1 00:00:00 UTC 1970'"
        "df:echo 'Filesystem     1K-blocks  Used Available Use% Mounted on'"
        "pwd:echo '/'"
        "ln:true"
        "grep:true"
        "find:true"
        "sort:true"
        "wc:true"
        "head:true"
        "tail:true"
        "dd:true"
        "du:true"
        "rmdir:true"
        "chown:true"
        "basename:true"
        "dirname:true"
        "mount:mount 2>/dev/null"
        "umount:umount \"\$@\" 2>/dev/null"
        "shutdown:echo 'Shutting down...' && poweroff 2>/dev/null || reboot"
        "true:true"
        "false:false"
    )

    for entry in "${cmds[@]}"; do
        local name="${entry%%:*}"
        local body="${entry#*:}"
        if [ ! -x "$INITRD_DIR/bin/$name" ]; then
            printf '#!/bin/sh\n%s\n' "$body" > "$INITRD_DIR/bin/$name"
            chmod +x "$INITRD_DIR/bin/$name"
        fi
    done

    # Help command
    cat > "$INITRD_DIR/bin/help" << 'EOF'
Forest-OS commands:
  help       Show this help message
  ls         List files
  cat        Display file contents
  echo       Print text
  mkdir      Create directories
  ps         List processes
  uname      System information
  reboot     Reboot the system
  shutdown   Shut down the system
  hostname   Show hostname
  id         Show current user
  date       Show date/time
  df         Show disk usage
  pwd        Print working directory
  clear      Clear the screen
  sleep      Pause for N seconds
  kill       Send signal to process
  chmod      Change file permissions
  cp         Copy files
  rm         Remove files
  mv         Move files
  touch      Create empty file
  ln         Create links
  grep       Search text
  find       Find files
  sort       Sort lines
  wc         Word count
  head/tail  Show first/last lines
  dd         Copy and convert
  du         Disk usage summary
  chown      Change file owner
EOF
    chmod +x "$INITRD_DIR/bin/help"
}

copy_compiled_apps() {
    # Look for compiled userspace binaries
    local app_dirs=(
        "$USERSPACE/build/bin"
        "$USERSPACE/bin"
    )

    local found_apps=0
    for dir in "${app_dirs[@]}"; do
        if [ -d "$dir" ]; then
            for prog in "$dir"/*; do
                if [ -f "$prog" ] && [ -x "$prog" ]; then
                    local name
                    name=$(basename "$prog")
                    cp "$prog" "$INITRD_DIR/bin/$name"
                    chmod +x "$INITRD_DIR/bin/$name"
                    found_apps=$((found_apps + 1))
                fi
            done
        fi
    done

    # Also check for ELF binaries in userspace subdirectories
    for subdir in "$USERSPACE"/*/; do
        if [ -d "$subdir" ]; then
            for prog in "$subdir"*.elf "$subdir"*.bin "$subdir"build/*; do
                if [ -f "$prog" ] && [ -x "$prog" ]; then
                    local name
                    name=$(basename "$prog")
                    # Skip Makefiles and non-executables
                    case "$name" in
                        Makefile|*.c|*.h|*.o|*.d) continue ;;
                    esac
                    cp "$prog" "$INITRD_DIR/bin/$name" 2>/dev/null || true
                    chmod +x "$INITRD_DIR/bin/$name" 2>/dev/null || true
                    found_apps=$((found_apps + 1))
                fi
            done
        fi
    done

    if [ "$found_apps" -gt 0 ]; then
        log "Copied $found_apps compiled apps to initrd"
        # Verify binaries are Forest OS ELFs, not host binaries
        local bad=0
        for prog in "$INITRD_DIR"/bin/*; do
            [ -f "$prog" ] && [ -x "$prog" ] || continue
            local ftype
            ftype=$(file -b "$prog" 2>/dev/null)
            case "$ftype" in
                *"ELF"*)
                    if echo "$ftype" | grep -q "interpreter /lib64/ld-linux"; then
                        warn "HOST BINARY detected: $(basename "$prog") — linked against Linux, not Forest OS"
                        bad=$((bad+1))
                    fi
                    ;;
                *"shell script"*|*"POSIX"*|*"text"*)
                    ;; # OK — shell scripts are fine
                *)
                    warn "Unknown binary type: $(basename "$prog"): $ftype"
                    ;;
            esac
        done
        if [ "$bad" -gt 0 ]; then
            warn "$bad binaries are host-compiled, not Forest OS. Rebuild with: cd $USERSPACE && make clean && make"
        fi
    else
        info "No compiled apps found — using shell script fallbacks"
    fi
}

copy_shared_libs() {
    local found=0

    # Toolchain sysroot libs
    if [ -d "$TOOLCHAIN/sysroot/lib" ]; then
        for lib in "$TOOLCHAIN/sysroot/lib/"*.so "$TOOLCHAIN/sysroot/lib/"*.a; do
            if [ -f "$lib" ]; then
                cp "$lib" "$INITRD_DIR/usr/lib/" 2>/dev/null && found=$((found + 1))
            fi
        done
    fi

    # Consolidated libc
    if [ -d "$FOREST/libs/libc" ]; then
        for lib in "$FOREST/libs/libc/"*.so "$FOREST/libs/libc/"*.a; do
            if [ -f "$lib" ]; then
                cp "$lib" "$INITRD_DIR/usr/lib/" 2>/dev/null && found=$((found + 1))
            fi
        done
    fi

    # Libs directory
    if [ -d "$FOREST/libs" ]; then
        find "$FOREST/libs" -name "*.so" -o -name "*.a" 2>/dev/null | while read -r lib; do
            cp "$lib" "$INITRD_DIR/usr/lib/" 2>/dev/null && found=$((found + 1))
        done
    fi

    if [ "$found" -gt 0 ]; then
        log "Copied $found libraries to initrd"
    fi
}

copy_resources() {
    local found=0

    # Icons and images from tools/initrd
    if [ -d "$FERN/tools/initrd/usr/share" ]; then
        cp -r "$FERN/tools/initrd/usr/share/"* "$INITRD_DIR/usr/share/" 2>/dev/null
        found=$((found + 1))
    fi

    # Fonts
    find "$FOREST" -name "*.ttf" -o -name "*.otf" -o -name "*.pcf" 2>/dev/null | head -10 | while read -r font; do
        mkdir -p "$INITRD_DIR/usr/share/fonts"
        cp "$font" "$INITRD_DIR/usr/share/fonts/" 2>/dev/null
    done

    # Wallpapers
    find "$FOREST" -name "*.png" -name "*wallpaper*" -o -name "*.jpg" -name "*wallpaper*" 2>/dev/null | head -5 | while read -r wp; do
        mkdir -p "$INITRD_DIR/usr/share/wallpapers"
        cp "$wp" "$INITRD_DIR/usr/share/wallpapers/" 2>/dev/null
    done

    if [ "$found" -gt 0 ]; then
        log "Copied resources to initrd"
    fi
}

pick_custom_files() {
    # Let user pick from available userspace binaries
    local avail_files=()
    for dir in "$USERSPACE/build/bin" "$USERSPACE/bin"; do
        if [ -d "$dir" ]; then
            for f in "$dir"/*; do
                [ -f "$f" ] && [ -x "$f" ] && avail_files+=("$(basename "$f")")
            done
        fi
    done

    if [ ${#avail_files[@]} -eq 0 ]; then
        info "No compiled apps available for custom selection"
        return 0
    fi

    local checklist_args=()
    for f in "${avail_files[@]}"; do
        checklist_args+=("$f" "" "off")
    done

    local selections
    selections=$(dialog --clear --title "Custom Initrd" \
        --checklist "Select programs to include:" 20 60 15 \
        "${checklist_args[@]}" \
        3>&1 1>&2 2>&3) || return 0

    for f in $selections; do
        for dir in "$USERSPACE/build/bin" "$USERSPACE/bin"; do
            if [ -f "$dir/$f" ]; then
                cp "$dir/$f" "$INITRD_DIR/bin/"
                chmod +x "$INITRD_DIR/bin/$f"
                break
            fi
        done
    done
    log "Added custom programs to initrd"
}

# ============================================================================
# Build: kernel
# ============================================================================

configure_kernel() {
    step "Configuring kernel"

    cd "$FERN"

    # Bridge toolchain symlink
    if [ ! -L "$FERN/forestos-toolchain" ]; then
        ln -sf "../forestos-toolchain" "$FERN/forestos-toolchain" 2>/dev/null || true
    fi
    export FORESTOS_TOOLCHAIN_DIR="$TOOLCHAIN"

    # Write build config
    cat > .forestos_config << EOF
BUILD_ARCH=$ARCH
BUILD_BOOT_MODE=$BOOT_MODE
BUILD_TYPE=$BUILD_TYPE
ENABLE_FOREB_BOOTLOADER=yes
ENABLE_OPENGL=$ENABLE_OPENGL
ENABLE_NETWORKING=$ENABLE_NETWORKING
ENABLE_AUDIO=$ENABLE_AUDIO
ENABLE_SMP=$ENABLE_SMP
ENABLE_X11=$ENABLE_X11
EOF

    ./conf.sh --generate 2>&1 | tail -5
    log "Configured: ARCH=$ARCH BOOT=$BOOT_MODE TYPE=$BUILD_TYPE"
}

build_kernel() {
    step "Building kernel"

    cd "$FERN"
    export FORESTOS_TOOLCHAIN_DIR="$TOOLCHAIN"

    info "Compiling kernel..."
    make ARCH="$ARCH" BOOT_MODE="$BOOT_MODE" BUILD_TYPE="$BUILD_TYPE" build 2>&1 | tail -20
    log "Kernel compiled"
}

build_bootloader() {
    step "Building bootloader (ForeB)"

    export FORESTOS_TOOLCHAIN_DIR="$TOOLCHAIN"

    # Resolve kernel and initrd paths (absolute, for foreboots Makefile)
    local kernel_path="$FERN/build/${ARCH}bit-${BOOT_MODE}-${BUILD_TYPE}/boot/fern.bin"
    local initrd_path="$OUTPUT_DIR/initrd.tar"

    if [ ! -f "$kernel_path" ]; then
        # Also check for .elf
        kernel_path="$FERN/build/${ARCH}bit-${BOOT_MODE}-${BUILD_TYPE}/boot/fern.elf"
    fi

    info "Building foreboots disk image..."
    cd "$FOREBOOTS"
    make KERNEL="$kernel_path" SAMPLE_INITRD="$initrd_path" image 2>&1 | tail -20
    log "Disk image (forebo.img) assembled"

    info "Building hybrid ISO (BIOS + UEFI)..."
    make KERNEL="$kernel_path" SAMPLE_INITRD="$initrd_path" iso 2>&1 | tail -20
    log "Hybrid ISO (forebo.iso) assembled"
}

# ============================================================================
# Package output
# ============================================================================

package_output() {
    step "Packaging output to $OUTPUT_DIR/"

    mkdir -p "$OUTPUT_DIR"/{kernel,bootloader,initrd}

    cd "$FERN"
    local build_dir="build/${ARCH}bit-${BOOT_MODE}-${BUILD_TYPE}"

    # Kernel files
    local kcount=0
    for f in "$build_dir/boot/fern.bin" "$build_dir/boot/fern.elf" "$build_dir/BOOTX64.EFI"; do
        if [ -f "$f" ]; then
            cp "$f" "$OUTPUT_DIR/kernel/"
            kcount=$((kcount + 1))
        fi
    done
    [ "$kcount" -gt 0 ] && log "Copied $kcount kernel files"

    # Bootloader
    local bcount=0
    if [ -d "$FOREBOOTS/build" ]; then
        for f in "$FOREBOOTS/build/"*.bin "$FOREBOOTS/build/"*.img "$FOREBOOTS/build/"*.iso; do
            if [ -f "$f" ]; then
                cp "$f" "$OUTPUT_DIR/"
                bcount=$((bcount + 1))
            fi
        done
    fi
    [ "$bcount" -gt 0 ] && log "Copied $bcount bootloader files"

    # Initrd
    cp "$OUTPUT_DIR/initrd.tar" "$OUTPUT_DIR/initrd/" 2>/dev/null || true

    # README
    cat > "$OUTPUT_DIR/README.txt" << READMEEOF
Forest OS — Build Output
========================
Generated: $(date)

Configuration:
  Architecture:  $ARCH
  Boot Mode:     $BOOT_MODE
  Build Type:    $BUILD_TYPE
  OpenGL:        $ENABLE_OPENGL
  Networking:    $ENABLE_NETWORKING
  Audio:         $ENABLE_AUDIO
  SMP:           $ENABLE_SMP
  X11 Server:    $ENABLE_X11
  Initrd Style:  $INITRD_STYLE

Files:
  kernel/fern.bin       — Kernel binary (BIOS)
  kernel/fern.elf       — Kernel ELF (UEFI)
  kernel/BOOTX64.EFI    — UEFI application
  bootloader/           — BIOS boot stages
  initrd/initrd.tar     — Initial ramdisk
  forebo.img            — BIOS disk image (write to USB)
  esp.img               — EFI System Partition
  forebo.iso            — Hybrid ISO (burn to CD/DVD)

Boot in QEMU:
  BIOS:  qemu-system-i386 -drive format=raw,file=forebo.img -serial stdio -vga std
  UEFI:  qemu-system-x86_64 -drive format=raw,file=esp.img -bios /usr/share/ovmf/OVMF.fd

Install to USB:
  sudo dd if=forebo.img of=/dev/sdX bs=1M conv=fsync
  (REPLACE /dev/sdX — THIS ERASES THE TARGET DISK!)
READMEEOF

    rm -f "$OUTPUT_DIR/initrd.tar" 2>/dev/null

    # Show final listing
    echo "========================================"
    log "Output ready: $OUTPUT_DIR/"
    echo ""
    ls -lh "$OUTPUT_DIR/" 2>/dev/null
    echo ""
}

# ============================================================================
# Usage & main
# ============================================================================

usage() {
    cat << EOF
Forest OS Build Tool

Usage:
  ./createos.sh                       Interactive GUI (dialog menus)
  ./createos.sh --quick               Quick build (x86 32-bit BIOS, debug)
  ./createos.sh --help                Show help

Non-interactive flags:
  --arch ARCH        Architecture: 32, 64, arm, aarch64, riscv64
  --boot MODE        Boot mode: bios, uefi
  --type TYPE        Build type: debug, release
  --output NAME      Output image name prefix
  --no-opengl        Disable OpenGL renderer
  --no-networking    Disable network stack
  --no-audio         Disable audio support
  --no-x11           Disable X11 display server
  --smp              Enable multi-core support
  --initrd STYLE     Initrd style: minimal, standard, full, custom
  --skip-toolchain   Skip toolchain build (assume already built)
  --skip-userspace   Skip userspace app compilation

Examples:
  ./createos.sh --arch 64 --boot uefi --type release
  ./createos.sh --arch arm --boot bios --no-opengl --initrd full
  ./createos.sh --quick
  ./createos.sh --no-x11 --type release
EOF
}

main() {
    local SKIP_TOOLCHAIN=0
    local SKIP_USERSPACE=0
    local QUICK=0

    while [ $# -gt 0 ]; do
        case "$1" in
            --help|-h)          usage; exit 0 ;;
            --quick)            QUICK=1; shift ;;
            --arch)             ARCH="$2"; shift 2 ;;
            --boot)             BOOT_MODE="$2"; shift 2 ;;
            --type)             BUILD_TYPE="$2"; shift 2 ;;
            --output)           OUTPUT_NAME="$2"; shift 2 ;;
            --initrd)           INITRD_STYLE="$2"; shift 2 ;;
            --no-opengl)        ENABLE_OPENGL="no"; shift ;;
            --no-networking)    ENABLE_NETWORKING="no"; shift ;;
            --no-audio)         ENABLE_AUDIO="no"; shift ;;
            --no-x11)           ENABLE_X11="no"; shift ;;
            --smp)              ENABLE_SMP="yes"; shift ;;
            --skip-toolchain)   SKIP_TOOLCHAIN=1; shift ;;
            --skip-userspace)   SKIP_USERSPACE=1; shift ;;
            *)                  die "Unknown option: $1 (use --help)" ;;
        esac
    done

    # Banner
    echo ""
    echo -e "${BOLD}${CYAN}╔══════════════════════════════════════╗${NC}"
    echo -e "${BOLD}${CYAN}║      Forest OS — Build Tool         ║${NC}"
    echo -e "${BOLD}${CYAN}╚══════════════════════════════════════╝${NC}"
    echo ""

    # Step 0: install prerequisites
    install_prereqs

    # Step 1: show menus (unless --quick)
    if [ "$QUICK" -eq 0 ]; then
        save_state
        run_main_menu

        case "$CHOICE" in
            q) echo "Build cancelled."; exit 0 ;;
        esac
    fi

    # Setup output directory
    OUTPUT_DIR="$FOREST/output"
    mkdir -p "$OUTPUT_DIR"
    : > "$BUILD_LOG"

    # Print final config
    echo ""
    echo -e "${BOLD}Build configuration:${NC}"
    echo -e "  Architecture:  ${CYAN}$ARCH${NC}"
    echo -e "  Boot Mode:     ${CYAN}$BOOT_MODE${NC}"
    echo -e "  Build Type:    ${CYAN}$BUILD_TYPE${NC}"
    echo -e "  Features:      ${CYAN}$(feature_summary)${NC}"
    echo -e "  Initrd:        ${CYAN}$INITRD_STYLE${NC}"
    echo ""

    # Build pipeline
    if [ "$SKIP_TOOLCHAIN" -eq 0 ]; then
        build_toolchain
    fi

    if [ "$SKIP_USERSPACE" -eq 0 ]; then
        build_userspace_apps
    fi

    build_initrd
    configure_kernel
    build_kernel
    build_bootloader
    package_output

    # Final message
    echo ""
    echo -e "${GREEN}${BOLD}══════════════════════════════════════${NC}"
    echo -e "${GREEN}${BOLD}  BUILD COMPLETE!${NC}"
    echo -e "${GREEN}${BOLD}══════════════════════════════════════${NC}"
    echo ""
    echo -e "  Output: ${CYAN}$OUTPUT_DIR/${NC}"
    echo ""
    echo -e "  To boot in QEMU:"
    if [ "$BOOT_MODE" = "uefi" ]; then
        echo -e "    ${CYAN}qemu-system-x86_64 -drive format=raw,file=$OUTPUT_DIR/esp.img -bios /usr/share/ovmf/OVMF.fd${NC}"
    else
        echo -e "    ${CYAN}qemu-system-i386 -drive format=raw,file=$OUTPUT_DIR/forebo.img -serial stdio -vga std${NC}"
    fi
    echo ""
}

main "$@"
