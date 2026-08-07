#!/usr/bin/env bash
# =============================================================================
# FOREST-OS CROSS-TOOLCHAIN BUILDER  (build-toolchain.sh)
# =============================================================================
# Builds the i686-forestos and/or x86_64-forestos cross-toolchains that the
# Fern kernel and Forest-OS userspace are compiled with.
#
# This is the single canonical, idempotent, resumable builder. It:
#   1. checks host build dependencies (portable probes, not distro-specific),
#   2. downloads + SHA-256-verifies the binutils/gcc tarballs into ./src,
#   3. patches binutils/gcc so the REAL *-forestos triple is recognised
#      (no more i686-elf + symlink hack),
#   4. populates ./sysroot from ./sysroot-skeleton + the Fern kernel headers,
#   5. builds binutils, then gcc (all-gcc + all-target-libgcc),
#   6. installs everything into ./install.
#
# Re-running is safe: completed configure/build/install phases are detected
# and skipped. Use --clean to force a rebuild.
#
# Pinned versions: binutils 2.43, gcc 13.2.0 (see checksums.txt).
#
# Usage:
#   ./build-toolchain.sh [OPTIONS]
#
# Options:
#   --arch 32|64|both   Target arch(s) to build          (default: both)
#   --jobs N            Parallel make jobs                (default: nproc)
#   --prefix DIR        Install prefix                    (default: ./install)
#   --fern-headers DIR  Fern kernel header dir            (default: ../fern/src/include)
#   --no-download       Never fetch tarballs; require them present in ./src
#   --skip-deps         Skip the host dependency check
#   --clean             Remove build/ dirs before building (forces reconfigure)
#   --help, -h          Show this help and exit
#
# Environment overrides:
#   FERN_HEADERS        Same as --fern-headers.
#   FORESTOS_TARGET_32  Override 32-bit triple (default i686-forestos).
#   FORESTOS_TARGET_64  Override 64-bit triple (default x86_64-forestos).
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths (anchored at THIS script's directory = the toolchain package root)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
TOOLCHAIN_DIR="$SCRIPT_DIR"
SRC_DIR="$TOOLCHAIN_DIR/src"
BUILD_DIR="$TOOLCHAIN_DIR/build"
INSTALL_DIR="$TOOLCHAIN_DIR/install"
SYSROOT_DIR="$TOOLCHAIN_DIR/sysroot"
SKELETON_DIR="$TOOLCHAIN_DIR/sysroot-skeleton"
CONSOLIDATED_LIBC="$TOOLCHAIN_DIR/../libs/libc"
PATCH_DIR="$TOOLCHAIN_DIR/patches"
CHECKSUMS="$TOOLCHAIN_DIR/checksums.txt"
LOG_DIR="$TOOLCHAIN_DIR/build/logs"

# ---------------------------------------------------------------------------
# Pinned versions & URLs
# ---------------------------------------------------------------------------
BINUTILS_VERSION="2.43"
GCC_VERSION="13.2.0"

BINUTILS_TARBALL="binutils-${BINUTILS_VERSION}.tar.xz"
GCC_TARBALL="gcc-${GCC_VERSION}.tar.xz"
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/${BINUTILS_TARBALL}"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/${GCC_TARBALL}"
BINUTILS_SRC="${SRC_DIR}/binutils-${BINUTILS_VERSION}"
GCC_SRC="${SRC_DIR}/gcc-${GCC_VERSION}"

# ---------------------------------------------------------------------------
# Defaults / options
# ---------------------------------------------------------------------------
BUILD_ARCH="both"
MAKE_JOBS="$(nproc 2>/dev/null || echo 4)"
FERN_HEADERS="${FERN_HEADERS:-${TOOLCHAIN_DIR}/../fern/src/include}"
NO_DOWNLOAD=false
SKIP_DEPS=false
DO_CLEAN=false

TARGET_32="${FORESTOS_TARGET_32:-i686-forestos}"
TARGET_64="${FORESTOS_TARGET_64:-x86_64-forestos}"

# ---------------------------------------------------------------------------
# Output helpers (colour only when stdout is a TTY)
# ---------------------------------------------------------------------------
if [[ -t 1 ]]; then
    C_RED=$'\033[0;31m'; C_GRN=$'\033[0;32m'; C_YEL=$'\033[1;33m'
    C_BLU=$'\033[0;34m'; C_CYN=$'\033[0;36m'; C_NC=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YEL=""; C_BLU=""; C_CYN=""; C_NC=""
fi
info()    { printf '%s[INFO]%s    %s\n'  "$C_BLU" "$C_NC" "$*"; }
success() { printf '%s[OK]%s      %s\n'  "$C_GRN" "$C_NC" "$*"; }
warn()    { printf '%s[WARN]%s    %s\n'  "$C_YEL" "$C_NC" "$*"; }
error()   { printf '%s[ERROR]%s   %s\n'  "$C_RED" "$C_NC" "$*" >&2; }
step()    { printf '%s[STEP]%s    %s\n'  "$C_CYN" "$C_NC" "$*"; }

die() { error "$*"; exit 1; }

usage() {
    # Print the banner comment block (lines between the two ==== rules).
    sed -n '/^# Usage:/,/^# ===/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)         BUILD_ARCH="${2:?--arch needs a value}"; shift 2 ;;
        --arch=*)       BUILD_ARCH="${1#*=}"; shift ;;
        --jobs)         MAKE_JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        --jobs=*)       MAKE_JOBS="${1#*=}"; shift ;;
        --prefix)       INSTALL_DIR="${2:?--prefix needs a value}"; shift 2 ;;
        --prefix=*)     INSTALL_DIR="${1#*=}"; shift ;;
        --fern-headers) FERN_HEADERS="${2:?--fern-headers needs a value}"; shift 2 ;;
        --fern-headers=*) FERN_HEADERS="${1#*=}"; shift ;;
        --no-download)  NO_DOWNLOAD=true; shift ;;
        --skip-deps)    SKIP_DEPS=true; shift ;;
        --clean)        DO_CLEAN=true; shift ;;
        --help|-h)      usage; exit 0 ;;
        *)              error "Unknown option: $1"; echo; usage; exit 1 ;;
    esac
done

case "$BUILD_ARCH" in
    32|64|both) ;;
    *) die "Invalid --arch '$BUILD_ARCH' (use 32, 64, or both)." ;;
esac
[[ "$MAKE_JOBS" =~ ^[0-9]+$ && "$MAKE_JOBS" -ge 1 ]] || die "Invalid --jobs '$MAKE_JOBS'."

# ---------------------------------------------------------------------------
# Host detection (for self-host on Forest-OS later)
# ---------------------------------------------------------------------------
HOST_KERNEL="$(uname -s 2>/dev/null || echo unknown)"
IS_FORESTOS_HOST=false
case "$HOST_KERNEL" in
    *[Ff]orest*|ForestOS|forestos) IS_FORESTOS_HOST=true ;;
esac
# Also treat a forestos-triple host cc as a self-host signal.
if command -v cc >/dev/null 2>&1 && cc -dumpmachine 2>/dev/null | grep -qi forestos; then
    IS_FORESTOS_HOST=true
fi

# ---------------------------------------------------------------------------
# Dependency check (portable: probe commands + libs, never require apt/dpkg)
# ---------------------------------------------------------------------------
check_dependencies() {
    step "Checking host build dependencies..."
    local missing_tools=() missing_libs=()

    local tools=(gcc g++ make makeinfo flex bison gawk tar xz)
    # A downloader is only required if we may fetch tarballs.
    if [[ "$NO_DOWNLOAD" == false ]]; then
        if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
            missing_tools+=("curl-or-wget")
        fi
    fi
    for t in "${tools[@]}"; do
        command -v "$t" >/dev/null 2>&1 || missing_tools+=("$t")
    done

    # Libraries GCC needs: gmp, mpfr, mpc, (isl optional). Probe headers via
    # pkg-config first, then a compile probe, then common header locations.
    _have_lib() {
        # $1 = header, $2... = pkg-config names to try
        local hdr="$1"; shift
        local pc
        for pc in "$@"; do
            command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$pc" 2>/dev/null && return 0
        done
        # compile probe (works even without pkg-config)
        if command -v gcc >/dev/null 2>&1; then
            printf '#include <%s>\nint main(void){return 0;}\n' "$hdr" \
                | gcc -x c -c -o /dev/null - >/dev/null 2>&1 && return 0
        fi
        local d
        for d in /usr/include /usr/local/include /usr/include/*-linux-gnu; do
            [[ -f "$d/$hdr" ]] && return 0
        done
        return 1
    }
    _have_lib gmp.h  gmp        || missing_libs+=("gmp (libgmp-dev)")
    _have_lib mpfr.h mpfr       || missing_libs+=("mpfr (libmpfr-dev)")
    _have_lib mpc.h  mpc        || missing_libs+=("mpc (libmpc-dev)")

    if [[ ${#missing_tools[@]} -gt 0 || ${#missing_libs[@]} -gt 0 ]]; then
        error "Missing host build dependencies:"
        [[ ${#missing_tools[@]} -gt 0 ]] && error "  tools: ${missing_tools[*]}"
        [[ ${#missing_libs[@]}  -gt 0 ]] && error "  libs : ${missing_libs[*]}"
        echo
        echo "Install hints (choose your distro):"
        echo "  Debian/Ubuntu: sudo apt install -y build-essential flex bison gawk \\"
        echo "                   texinfo curl xz-utils libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev"
        echo "  Fedora/RHEL:   sudo dnf install -y gcc gcc-c++ make flex bison gawk \\"
        echo "                   texinfo curl xz gmp-devel mpfr-devel libmpc-devel isl-devel zlib-devel"
        echo "  Arch:          sudo pacman -S --needed base-devel flex bison gawk texinfo curl xz gmp mpfr libmpc"
        [[ "$IS_FORESTOS_HOST" == true ]] && echo "  Forest-OS self-host: ensure the native gcc/binutils + gmp/mpfr/mpc are installed in the sysroot."
        die "Resolve the dependencies above, or re-run with --skip-deps if you know they are present."
    fi
    success "All host dependencies satisfied."
}

# ---------------------------------------------------------------------------
# Download + checksum verification
# ---------------------------------------------------------------------------
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        die "No sha256sum/shasum available to verify downloads."
    fi
}

expected_sha() {
    # $1 = tarball filename; look it up in checksums.txt
    [[ -f "$CHECKSUMS" ]] || die "checksums.txt missing at $CHECKSUMS"
    awk -v f="$1" '$2==f {print $1; exit}' "$CHECKSUMS"
}

verify_tarball() {
    local path="$1" name; name="$(basename "$path")"
    local want; want="$(expected_sha "$name")"
    [[ -n "$want" ]] || die "No pinned checksum for $name in checksums.txt."
    local got; got="$(sha256_of "$path")"
    if [[ "$got" != "$want" ]]; then
        error "Checksum MISMATCH for $name"
        error "  expected: $want"
        error "  got:      $got"
        die "Refusing to use a corrupt/untrusted tarball. Delete it and retry."
    fi
    success "Verified $name (sha256 ok)."
}

download_source() {
    local url="$1" tarball="$2" src_dir="$3" name="$4"

    # Already extracted (configure present)? Nothing to do.
    if [[ -f "${src_dir}/configure" ]]; then
        info "${name} source already extracted."
        return 0
    fi

    mkdir -p "$SRC_DIR"
    local tarball_path="${SRC_DIR}/${tarball}"

    if [[ ! -f "$tarball_path" ]]; then
        [[ "$NO_DOWNLOAD" == true ]] && die "${tarball} not present and --no-download was given."
        step "Downloading ${name}..."
        if command -v curl >/dev/null 2>&1; then
            curl -fL --retry 3 --progress-bar -o "${tarball_path}.part" "$url"
        elif command -v wget >/dev/null 2>&1; then
            wget -q --show-progress -O "${tarball_path}.part" "$url"
        else
            die "Neither curl nor wget available to download ${name}."
        fi
        mv -f "${tarball_path}.part" "$tarball_path"
    else
        info "${tarball} already present in src/."
    fi

    verify_tarball "$tarball_path"

    step "Extracting ${tarball}..."
    rm -rf "$src_dir"
    tar -xf "$tarball_path" -C "$SRC_DIR"
    [[ -f "${src_dir}/configure" ]] || die "Extraction of ${tarball} did not yield ${src_dir}/configure."
    success "${name} extracted."
}

# ---------------------------------------------------------------------------
# Forest-OS triple support: idempotent, version-tolerant source patching
# ---------------------------------------------------------------------------
patch_config_sub() {
    # $1 = path to a config.sub file. Adds `forestos*` to the OS list.
    local f="$1"
    [[ -f "$f" ]] || return 0
    if grep -q 'forestos\*' "$f"; then
        return 0   # already patched
    fi
    if grep -q 'fiwix\*' "$f"; then
        sed -i 's/\bfiwix\*/forestos* | fiwix*/' "$f"
    else
        die "Cannot patch $f: expected 'fiwix*' anchor not found (unexpected config.sub version)."
    fi
    grep -q 'forestos\*' "$f" || die "Failed to add forestos to $f."
}

patch_binutils_gas_configure_tgt() {
    local f="${BINUTILS_SRC}/gas/configure.tgt"
    [[ -f "$f" ]] || return 0
    if grep -q 'forestos' "$f"; then
        return 0   # already patched
    fi
    # Insert forestos cases before the i386-*-elf* case (line ~231).
    # forestos is an ELF target like *-elf, so we use the same fmt=elf.
    grep -q 'i386-\*-elf\*' "$f" || die "Cannot patch gas/configure.tgt: 'i386-*-elf*' anchor not found."
    local tmp; tmp="$(mktemp)"
    awk '
        /^  i386-\*-elf\*\)/ && !done {
            print "  i[3-7]86-*-forestos*)   fmt=elf;;";
            print "  x86_64-*-forestos*)     fmt=elf;;";
            done=1;
        }
        { print }
    ' "$f" > "$tmp"
    mv -f "$tmp" "$f"
    grep -q 'forestos' "$f" || die "Failed to add forestos to gas/configure.tgt."
}

patch_binutils_ld_configure_tgt() {
    local f="${BINUTILS_SRC}/ld/configure.tgt"
    [[ -f "$f" ]] || return 0
    if grep -q 'forestos' "$f"; then
        return 0   # already patched
    fi
    # Insert forestos cases before the i[3-7]86-*-elf* case (line ~405).
    # Use the same emulation as *-elf targets (elf_i386 / elf_x86_64).
    grep -q 'i\[3-7\]86-\*-elf\* | i\[3-7\]86-\*-rtems\* | i\[3-7\]86-\*-genode\*' "$f" \
        || die "Cannot patch ld/configure.tgt: i[3-7]86-*-elf* anchor not found."
    local tmp; tmp="$(mktemp)"
    awk '
        /^i\[3-7\]86-\*-elf\* \| i\[3-7\]86-\*-rtems\* \| i\[3-7\]86-\*-genode\*\)/ && !done {
            print "i[3-7]86-*-forestos*)";
            print "\t\t\ttarg_emul=elf_i386";
            print "\t\t\ttarg_extra_emuls=elf_iamcu";
            print "\t\t\t;;";
            print "x86_64-*-forestos*)";
            print "\t\t\ttarg_emul=elf_x86_64";
            print "\t\t\ttarg_extra_emuls=\"elf_i386 elf_iamcu elf32_x86_64\"";
            print "\t\t\ttarg_extra_libpath=\"elf_i386 elf_iamcu elf32_x86_64\"";
            print "\t\t\ttdir_elf_i386=`echo ${targ_alias} | sed -e '\''s/x86_64/i386/'\''`";
            print "\t\t\t;;";
            done=1;
        }
        { print }
    ' "$f" > "$tmp"
    mv -f "$tmp" "$f"
    grep -q 'forestos' "$f" || die "Failed to add forestos to ld/configure.tgt."
}

patch_binutils_bfd_config_bfd() {
    local f="${BINUTILS_SRC}/bfd/config.bfd"
    [[ -f "$f" ]] || return 0
    if grep -q 'forestos' "$f"; then
        return 0   # already patched
    fi
    # Insert forestos cases before the i[3-7]86-*-elf* case (line ~590).
    # Match the same BFD vectors as *-elf targets.
    grep -q 'i\[3-7\]86-\*-elf\* | i\[3-7\]86-\*-rtems\* | i\[3-7\]86-\*-genode\*' "$f" \
        || die "Cannot patch bfd/config.bfd: i[3-7]86-*-elf* anchor not found."
    local tmp; tmp="$(mktemp)"
    awk '
        /^  i\[3-7\]86-\*-elf\* \| i\[3-7\]86-\*-rtems\* \| i\[3-7\]86-\*-genode\*\)/ && !done {
            print "  i[3-7]86-*-forestos*)";
            print "    targ_defvec=i386_elf32_vec";
            print "    targ_selvecs=\"iamcu_elf32_vec i386_coff_vec\"";
            print "    ;;";
            print "#ifdef BFD64";
            print "  x86_64-*-forestos*)";
            print "    targ_defvec=x86_64_elf64_vec";
            print "    targ_selvecs=\"i386_elf32_vec iamcu_elf32_vec x86_64_elf32_vec\"";
            print "    want64=true";
            print "    ;;";
            print "#endif";
            done=1;
        }
        { print }
    ' "$f" > "$tmp"
    mv -f "$tmp" "$f"
    grep -q 'forestos' "$f" || die "Failed to add forestos to bfd/config.bfd."
}

patch_gcc_config_gcc() {
    local f="${GCC_SRC}/gcc/config.gcc"
    [[ -f "$f" ]] || die "Missing $f"
    if grep -q 'i\[34567\]86-\*-forestos\*' "$f"; then
        return 0   # already patched
    fi
    # Insert the forestos target cases immediately before the i686 elf case.
    local anchor='i\[34567\]86-\*-elf\*)'
    grep -q "$anchor" "$f" || die "Cannot patch config.gcc: '$anchor' anchor not found."
    # Build the insertion block in a temp file and splice it in with awk
    # (robust against sed multiline quoting issues).
    local tmp; tmp="$(mktemp)"
    awk '
        /^i\[34567\]86-\*-elf\*\)/ && !done {
            print "i[34567]86-*-forestos*)";
            print "\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h forestos.h\"";
            print "\t;;";
            print "x86_64-*-forestos*)";
            print "\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h forestos.h\"";
            print "\t;;";
            done=1;
        }
        { print }
    ' "$f" > "$tmp"
    mv -f "$tmp" "$f"
    grep -q 'i\[34567\]86-\*-forestos\*' "$f" || die "Failed to add forestos cases to config.gcc."
}

patch_gcc_libcody_cxx11() {
    local f="${GCC_SRC}/libcody/configure"
    [[ -f "$f" ]] || return 0
    # Patch libcody's configure to accept C++11 or later (>= instead of ==).
    # The original check requires __cplusplus == 201103 exactly, which fails
    # with GCC >= 14 host compilers that default to C++17 (201703).
    if grep -q '__cplusplus >= 201103' "$f"; then
        return 0   # already patched
    fi
    if grep -q '__cplusplus != 201103' "$f"; then
        sed -i 's/__cplusplus != 201103/__cplusplus >= 201103/g' "$f"
        sed -i 's/__cplusplus > 201103/__cplusplus < 201103/g' "$f"
        success "Patched libcody configure to accept C++11 or later."
    fi
}

patch_gcc_libgcc_config_host() {
    local f="${GCC_SRC}/libgcc/config.host"
    [[ -f "$f" ]] || return 0
    if grep -q 'forestos' "$f"; then
        return 0   # already patched
    fi
    # Add forestos cases after the x86_64-*-elf* case.
    local anchor='x86_64-\*-elf\* | x86_64-\*-rtems\*)'
    grep -q "$anchor" "$f" || die "Cannot patch libgcc/config.host: '$anchor' anchor not found."
    local tmp; tmp="$(mktemp)"
    awk '
        /^x86_64-\*-elf\* \| x86_64-\*-rtems\*\)/ && !done {
            print "i[34567]86-*-forestos*)";
            print "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"";
            print "\t;;";
            print "x86_64-*-forestos*)";
            print "\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"";
            print "\t;;";
            done=1;
        }
        { print }
    ' "$f" > "$tmp"
    mv -f "$tmp" "$f"
    grep -q 'forestos' "$f" || die "Failed to add forestos to libgcc/config.host."
}

install_forestos_gcc_header() {
    local dst="${GCC_SRC}/gcc/config/forestos.h"
    local srch="${PATCH_DIR}/gcc/config/forestos.h"
    [[ -f "$srch" ]] || die "Missing tracked patch asset: $srch"
    # Always refresh so edits to the tracked header take effect on rebuild.
    cp -f "$srch" "$dst"
}

apply_forestos_patches() {
    step "Applying Forest-OS triple patches (idempotent)..."
    patch_config_sub "${BINUTILS_SRC}/config.sub"
    patch_config_sub "${GCC_SRC}/config.sub"
    patch_binutils_gas_configure_tgt
    patch_binutils_ld_configure_tgt
    patch_binutils_bfd_config_bfd
    patch_gcc_config_gcc
    patch_gcc_libcody_cxx11
    patch_gcc_libgcc_config_host
    install_forestos_gcc_header
    success "binutils/gcc now recognise i686-forestos / x86_64-forestos."
}

# ---------------------------------------------------------------------------
# Sysroot population (headers only; libc/crt objects are build outputs)
# ---------------------------------------------------------------------------
setup_sysroot() {
    step "Populating sysroot at ${SYSROOT_DIR}..."
    mkdir -p "${SYSROOT_DIR}/usr/include" "${SYSROOT_DIR}/usr/lib" "${SYSROOT_DIR}/lib"

    # 1. Fern kernel headers (the bulk of <...> includes resolve via -Isrc/include
    #    in the kernel build, but install them for a complete sysroot).
    if [[ -d "$FERN_HEADERS" ]]; then
        cp -r "${FERN_HEADERS}/." "${SYSROOT_DIR}/usr/include/"
        success "Fern kernel headers installed from ${FERN_HEADERS}."
    else
        warn "Fern headers not found at ${FERN_HEADERS} (set --fern-headers / FERN_HEADERS)."
    fi

    # 2. Overlay the tracked libc island (stdio.h, forestos/syscalls.h,
    #    sys/types.h wrapper, libc/*) that has no other source in the repo and
    #    is REQUIRED for <stdio.h> -> <forestos/syscalls.h> to resolve.
    if [[ -d "${SKELETON_DIR}/usr/include" ]]; then
        cp -r "${SKELETON_DIR}/usr/include/." "${SYSROOT_DIR}/usr/include/"
        success "sysroot-skeleton libc island overlaid."
    else
        warn "sysroot-skeleton not found at ${SKELETON_DIR}."
    fi

    # 2b. Overlay consolidated libc headers (single source of truth for libc)
    if [[ -d "${CONSOLIDATED_LIBC}/include/libc" ]]; then
        mkdir -p "${SYSROOT_DIR}/usr/include/libc"
        cp -r "${CONSOLIDATED_LIBC}/include/libc/." "${SYSROOT_DIR}/usr/include/libc/"
        success "consolidated libc headers overlaid."
    else
        warn "consolidated libc not found at ${CONSOLIDATED_LIBC}/include/libc."
    fi

    # Sanity check the kernel-critical header island.
    local h
    for h in stdio.h forestos/syscalls.h sys/types.h; do
        [[ -f "${SYSROOT_DIR}/usr/include/${h}" ]] \
            || warn "sysroot missing <${h}> — the Fern kernel will fail to compile without it."
    done
}

# ---------------------------------------------------------------------------
# Build binutils for one target
# ---------------------------------------------------------------------------
build_binutils() {
    local target="$1"
    local bdir="${BUILD_DIR}/binutils-${target}"
    local marker="${bdir}/.forestos-installed"

    if [[ "$DO_CLEAN" == true ]]; then rm -rf "$bdir"; fi
    if [[ -f "$marker" ]]; then
        info "binutils for ${target} already installed (marker present) — skipping."
        return 0
    fi

    step "Building binutils ${BINUTILS_VERSION} for ${target}..."
    mkdir -p "$bdir" "$LOG_DIR"
    (
        cd "$bdir"
        if [[ ! -f Makefile ]]; then
            info "Configuring binutils (${target})..."
            "${BINUTILS_SRC}/configure" \
                --target="$target" \
                --prefix="$INSTALL_DIR" \
                --with-sysroot="$SYSROOT_DIR" \
                --disable-nls \
                --disable-werror \
                --disable-multilib \
                --enable-64-bit-bfd \
                --enable-gprofng=no \
                2>&1 | tee "${LOG_DIR}/binutils-${target}-configure.log"
        else
            info "binutils (${target}) already configured — skipping configure."
        fi
        info "Compiling binutils (${MAKE_JOBS} jobs)..."
        make -j"$MAKE_JOBS"      2>&1 | tee "${LOG_DIR}/binutils-${target}-build.log"
        make install             2>&1 | tee "${LOG_DIR}/binutils-${target}-install.log"
    )
    touch "$marker"
    success "binutils for ${target} installed."
}

# ---------------------------------------------------------------------------
# Build gcc (freestanding: all-gcc + all-target-libgcc) for one target
# ---------------------------------------------------------------------------
build_gcc() {
    local target="$1" arch_flags="$2"
    local bdir="${BUILD_DIR}/gcc-${target}"
    local marker="${bdir}/.forestos-installed"

    if [[ "$DO_CLEAN" == true ]]; then rm -rf "$bdir"; fi
    if [[ -f "$marker" ]]; then
        info "gcc for ${target} already installed (marker present) — skipping."
        return 0
    fi

    step "Building gcc ${GCC_VERSION} for ${target}..."
    mkdir -p "$bdir" "$LOG_DIR"
    (
        cd "$bdir"
        # Ensure the freshly built binutils are found first.
        export PATH="${INSTALL_DIR}/bin:${PATH}"
        # Force C++17 for the host compiler to avoid char8_t issues with
        # GCC >= 14 host compilers that default to C++20. The libcody
        # configure is patched to accept >= C++11 so this is safe.
        export CXXFLAGS="${CXXFLAGS:--std=gnu++17}"
        if [[ ! -f Makefile ]]; then
            info "Configuring gcc (${target})..."
            # shellcheck disable=SC2086  # arch_flags is intentionally word-split
            "${GCC_SRC}/configure" \
                --target="$target" \
                --prefix="$INSTALL_DIR" \
                --with-sysroot="$SYSROOT_DIR" \
                --disable-nls \
                --enable-languages=c,c++ \
                --without-headers \
                --with-newlib \
                --disable-hosted-libstdcxx \
                --disable-libstdcxx \
                --disable-libstdcxx-pch \
                --disable-libssp \
                --disable-libgomp \
                --disable-libquadmath \
                --disable-libatomic \
                --disable-threads \
                --disable-shared \
                --disable-shared-libgcc \
                --disable-multilib \
                ${arch_flags} \
                2>&1 | tee "${LOG_DIR}/gcc-${target}-configure.log"
        else
            info "gcc (${target}) already configured — skipping configure."
        fi
        info "Compiling gcc + libgcc (${MAKE_JOBS} jobs) — this takes a while..."
        make -j"$MAKE_JOBS" all-gcc           2>&1 | tee "${LOG_DIR}/gcc-${target}-all-gcc.log"
        make -j"$MAKE_JOBS" all-target-libgcc 2>&1 | tee "${LOG_DIR}/gcc-${target}-all-libgcc.log"
        make install-gcc                      2>&1 | tee "${LOG_DIR}/gcc-${target}-install-gcc.log"
        make install-target-libgcc            2>&1 | tee "${LOG_DIR}/gcc-${target}-install-libgcc.log"
    )
    touch "$marker"
    success "gcc for ${target} installed."

    # Fix broken include-fixed copies created by GCC's fixincludes.
    patch_include_fixed "$target"
}

# ---------------------------------------------------------------------------
# Fix GCC's include-fixed directory: GCC's fixincludes auto-processes sysroot
# headers and may create broken copies that reference headers not present in
# include-fixed/. We copy the missing headers so <stdio.h> -> <forestos/
# syscalls.h> resolves correctly.
# ---------------------------------------------------------------------------
patch_include_fixed() {
    local target="$1"
    # Locate the include-fixed directory for this target's GCC installation.
    # It lives under lib/gcc/<target-prefix>/<version>/include-fixed/.
    local gcc_lib_dir
    gcc_lib_dir="$(ls -d "${INSTALL_DIR}/lib/gcc/${target}/${GCC_VERSION}" 2>/dev/null | head -1)"
    [[ -n "$gcc_lib_dir" ]] || return 0

    local inc_fixed="${gcc_lib_dir}/include-fixed"
    if [[ ! -d "$inc_fixed" ]]; then
        info "No include-fixed directory for ${target} — skipping patch."
        return 0
    fi

    step "Patching include-fixed for ${target}..."

    # Copy forestos/syscalls.h (required by stdio.h)
    if [[ -f "${SYSROOT_DIR}/usr/include/forestos/syscalls.h" ]]; then
        mkdir -p "${inc_fixed}/forestos"
        if [[ ! -f "${inc_fixed}/forestos/syscalls.h" ]] || \
           ! diff -q "${SYSROOT_DIR}/usr/include/forestos/syscalls.h" \
                      "${inc_fixed}/forestos/syscalls.h" >/dev/null 2>&1; then
            cp -f "${SYSROOT_DIR}/usr/include/forestos/syscalls.h" \
                  "${inc_fixed}/forestos/syscalls.h"
            success "Copied forestos/syscalls.h into include-fixed."
        else
            info "forestos/syscalls.h already up to date in include-fixed."
        fi
    fi

    # Copy sys/types.h (required by forestos/syscalls.h)
    if [[ -f "${SYSROOT_DIR}/usr/include/sys/types.h" ]]; then
        mkdir -p "${inc_fixed}/sys"
        if [[ ! -f "${inc_fixed}/sys/types.h" ]] || \
           ! diff -q "${SYSROOT_DIR}/usr/include/sys/types.h" \
                      "${inc_fixed}/sys/types.h" >/dev/null 2>&1; then
            cp -f "${SYSROOT_DIR}/usr/include/sys/types.h" \
                  "${inc_fixed}/sys/types.h"
            success "Copied sys/types.h into include-fixed."
        else
            info "sys/types.h already up to date in include-fixed."
        fi
    fi
}

# ---------------------------------------------------------------------------
# Verify a built toolchain (freestanding compile, no libc/crt needed)
# ---------------------------------------------------------------------------
verify_toolchain() {
    local target="$1"
    local gcc_bin="${INSTALL_DIR}/bin/${target}-gcc"
    local ld_bin="${INSTALL_DIR}/bin/${target}-ld"

    step "Verifying ${target} toolchain..."
    [[ -x "$gcc_bin" ]] || die "Expected ${gcc_bin} — not found."
    [[ -x "$ld_bin"  ]] || die "Expected ${ld_bin} — not found."

    # Confirm the driver reports the forestos triple, not elf.
    local dm; dm="$("$gcc_bin" -dumpmachine 2>/dev/null || echo '?')"
    [[ "$dm" == "$target" ]] || warn "gcc -dumpmachine=${dm} (expected ${target})."

    # A freestanding object build must succeed with no libc/crt present.
    local tmpc tmpo
    tmpc="$(mktemp "${TMPDIR:-/tmp}/forestos_probe_XXXXXX.c")"
    tmpo="${tmpc%.c}.o"
    printf 'void _start(void){ for(;;){ __asm__ __volatile__("hlt"); } }\n' > "$tmpc"
    if "$gcc_bin" -ffreestanding -nostdlib -c -o "$tmpo" "$tmpc" >/dev/null 2>&1; then
        success "${target}: freestanding compile probe passed."
    else
        warn "${target}: freestanding compile probe failed (inspect logs in ${LOG_DIR})."
    fi
    rm -f "$tmpc" "$tmpo"

    # Verify <stdio.h> -> <forestos/syscalls.h> resolves (critical for kernel).
    tmpc="$(mktemp "${TMPDIR:-/tmp}/forestos_stdio_probe_XXXXXX.c")"
    tmpo="${tmpc%.c}.o"
    printf '#include <stdio.h>\nint main(void){return 0;}\n' > "$tmpc"
    if "$gcc_bin" -ffreestanding -nostdlib -c -o "$tmpo" "$tmpc" >/dev/null 2>&1; then
        success "${target}: stdio.h + forestos/syscalls.h include chain OK."
    else
        warn "${target}: stdio.h include chain broken — check include-fixed/."
    fi
    rm -f "$tmpc" "$tmpo"

    # libgcc must exist (the kernel resolves it via -print-libgcc-file-name).
    local libgcc; libgcc="$("$gcc_bin" -print-libgcc-file-name 2>/dev/null || true)"
    if [[ -n "$libgcc" && -f "$libgcc" ]]; then
        success "${target}: libgcc present ($(basename "$libgcc"))."
    else
        warn "${target}: libgcc not found — soft-float/builtin routines may be missing."
    fi

    info "  gcc: $("$gcc_bin" --version | head -1)"
    info "  ld : $("$ld_bin" --version | head -1)"
}

# ---------------------------------------------------------------------------
# Build one complete toolchain for a target
# ---------------------------------------------------------------------------
build_toolchain_for() {
    local target="$1" arch_flags="$2"
    echo
    echo "============================================================"
    echo "  Toolchain target: ${target}"
    echo "============================================================"
    build_binutils "$target"
    build_gcc      "$target" "$arch_flags"
    verify_toolchain "$target"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    echo
    printf '%s============================================================%s\n' "$C_CYN" "$C_NC"
    printf '%s  Forest-OS Cross-Toolchain Builder%s\n' "$C_CYN" "$C_NC"
    printf '%s============================================================%s\n' "$C_CYN" "$C_NC"
    echo
    info "Package dir  : ${TOOLCHAIN_DIR}"
    info "Install       : ${INSTALL_DIR}"
    info "Sysroot       : ${SYSROOT_DIR}"
    info "Fern headers  : ${FERN_HEADERS}"
    info "Target arch(s): ${BUILD_ARCH}"
    info "Jobs          : ${MAKE_JOBS}"
    info "binutils/gcc  : ${BINUTILS_VERSION} / ${GCC_VERSION}"
    [[ "$IS_FORESTOS_HOST" == true ]] && info "Host          : Forest-OS (self-host build)"
    echo

    [[ "$SKIP_DEPS" == true ]] || check_dependencies

    mkdir -p "${INSTALL_DIR}/bin"

    download_source "$BINUTILS_URL" "$BINUTILS_TARBALL" "$BINUTILS_SRC" "binutils-${BINUTILS_VERSION}"
    download_source "$GCC_URL"      "$GCC_TARBALL"      "$GCC_SRC"      "gcc-${GCC_VERSION}"

    apply_forestos_patches
    setup_sysroot

    case "$BUILD_ARCH" in
        32)   build_toolchain_for "$TARGET_32" "--with-arch=i686   --with-tune=generic" ;;
        64)   build_toolchain_for "$TARGET_64" "--with-arch=x86-64 --with-tune=generic" ;;
        both) build_toolchain_for "$TARGET_32" "--with-arch=i686   --with-tune=generic"
              build_toolchain_for "$TARGET_64" "--with-arch=x86-64 --with-tune=generic" ;;
    esac

    echo
    printf '%s============================================================%s\n' "$C_GRN" "$C_NC"
    printf '%s  Forest-OS toolchain build complete%s\n' "$C_GRN" "$C_NC"
    printf '%s============================================================%s\n' "$C_GRN" "$C_NC"
    echo
    success "Installed to: ${INSTALL_DIR}/bin/"
    info "Cross compilers:"
    local f
    for f in "${INSTALL_DIR}"/bin/*-forestos-gcc; do
        [[ -e "$f" ]] && echo "    $f"
    done
    echo
    info "Point the Fern build at this toolchain, e.g.:"
    echo "    export FORESTOS_TOOLCHAIN_DIR=${TOOLCHAIN_DIR}"
    echo "    make -C ../fern all"
    echo
}

main "$@"
