#!/bin/bash
# =============================================================================
# FOREST OS 64-BIT TOOLCHAIN BUILDER
# =============================================================================
# Build script to create x86_64-forestos cross-compiler
# =============================================================================

set -euo pipefail

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FORESTOS_DIR="$(dirname "$SCRIPT_DIR")"
TOOLCHAIN_DIR="$FORESTOS_DIR/forestos-toolchain"
BUILD_DIR="$TOOLCHAIN_DIR/build"
INSTALL_DIR="$TOOLCHAIN_DIR/install"
SYSROOT_DIR="$TOOLCHAIN_DIR/sysroot"
SRC_DIR="$TOOLCHAIN_DIR/src"

# Target configuration
TARGET="x86_64-forestos"
PREFIX="$INSTALL_DIR"
SYSROOT="$SYSROOT_DIR"

# Build flags
MAKE_JOBS=$(nproc)
PARALLEL_FLAGS="-j$MAKE_JOBS"

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check dependencies
check_dependencies() {
    print_info "Checking build dependencies..."
    
    local deps=("gcc" "make" "flex" "bison" "gawk" "texinfo" "gmp-dev" "mpfr-dev" "mpc-dev" "zlib1g-dev" "libisl-dev")
    
    for dep in "${deps[@]}"; do
        if ! dpkg -l | grep -q "ii  $dep "; then
            print_error "Missing dependency: $dep"
            print_info "Install with: sudo apt install build-essential libgmp-dev libmpfr-dev libmpc-dev texinfo flex bison gawk zlib1g-dev libisl-dev"
            exit 1
        fi
    done
    
    print_success "Dependencies satisfied"
}

# Create directories
setup_directories() {
    print_info "Setting up build directories..."
    
    mkdir -p "$BUILD_DIR/x86_64-binutils"
    mkdir -p "$BUILD_DIR/x86_64-gcc"
    mkdir -p "$INSTALL_DIR/bin"
    mkdir -p "$SYSROOT_DIR/usr/lib"
    mkdir -p "$SYSROOT_DIR/usr/include"
    
    print_success "Directories created"
}

# Build binutils for 64-bit
build_binutils() {
    print_info "Building binutils for $TARGET..."
    
    cd "$BUILD_DIR/x86_64-binutils"
    
    if [ ! -f "$SRC_DIR/binutils-2.39/configure" ]; then
        print_error "binutils source not found at $SRC_DIR/binutils-2.39"
        return 1
    fi
    
    "$SRC_DIR/binutils-2.39/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --disable-werror \
        --with-syslibpath="$SYSROOT_DIR/usr/lib" \
        --disable-multilib \
        --enable-64-bit-bfd \
        --enable-gold=default
    
    make $PARALLEL_FLAGS
    make install
    
    print_success "binutils for $TARGET built successfully"
}

# Build gcc for 64-bit  
build_gcc() {
    print_info "Building gcc for $TARGET..."
    
    cd "$BUILD_DIR/x86_64-gcc"
    
    if [ ! -f "$SRC_DIR/gcc-12.2.0/configure" ]; then
        print_error "gcc source not found at $SRC_DIR/gcc-12.2.0"
        return 1
    fi
    
    # Configure with same options as 32-bit but for x86_64
    "$SRC_DIR/gcc-12.2.0/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" \
        --disable-nls \
        --enable-languages=c,c++ \
        --without-headers \
        --disable-libssp \
        --disable-libgomp \
        --disable-libquadmath \
        --disable-threads \
        --disable-libatomic \
        --disable-libstdcxx-pch \
        --disable-libstdcxx \
        --with-newlib \
        --disable-multilib \
        --with-arch=x86-64 \
        --with-tune=generic
    
    # Build gcc
    make $PARALLEL_FLAGS all-gcc
    make install-gcc
    
    print_success "gcc for $TARGET built successfully"
}

# Create basic sysroot headers
setup_sysroot() {
    print_info "Setting up basic sysroot..."
    
    # Copy kernel headers to sysroot
    if [ -d "$FORESTOS_DIR/src/include" ]; then
        cp -r "$FORESTOS_DIR/src/include/"* "$SYSROOT_DIR/usr/include/"
        print_success "Kernel headers copied to sysroot"
    fi
    
    # Create basic library structure
    mkdir -p "$SYSROOT_DIR/usr/lib"
    mkdir -p "$SYSROOT_DIR/lib"
    mkdir -p "$SYSROOT_DIR/usr/bin"
    
    # Create libc stub
    cat > "$SYSROOT_DIR/usr/lib/libc.a" << 'EOF'
!archivelib
EOF
    
    print_success "Basic sysroot created"
}

# Verify installation
verify_toolchain() {
    print_info "Verifying toolchain installation..."
    
    local gcc_path="$PREFIX/bin/$TARGET-gcc"
    local ld_path="$PREFIX/bin/$TARGET-ld"
    
    if [ ! -f "$gcc_path" ]; then
        print_error "GCC not found at $gcc_path"
        return 1
    fi
    
    if [ ! -f "$ld_path" ]; then
        print_error "LD not found at $ld_path"
        return 1
    fi
    
    # Test basic functionality
    echo 'int main(){return 0;}' | "$gcc_path" -x c - -o /tmp/test_elf
    if [ $? -eq 0 ] && [ -f "/tmp/test_elf" ]; then
        rm -f /tmp/test_elf
        print_success "Toolchain verification passed"
    else
        print_error "Toolchain verification failed"
        return 1
    fi
}

# Main build process
main() {
    print_info "Starting 64-bit Forest OS toolchain build..."
    print_info "Target: $TARGET"
    print_info "Prefix: $PREFIX"
    print_info "Sysroot: $SYSROOT"
    
    check_dependencies
    setup_directories
    setup_sysroot
    
    # Only build binutils and gcc (not full libstdc++ etc. for speed)
    build_binutils
    build_gcc
    
    verify_toolchain
    
    print_success "64-bit Forest OS toolchain build completed!"
    print_info "Toolchain binaries are available at: $PREFIX/bin/"
    print_info "Usage: $TARGET-gcc -m64 your_source.c"
    
    # Update Makefile to recognize the new toolchain
    if grep -q "FORESTOS_TOOLCHAIN_HAS_64BIT" "$FORESTOS_DIR/Makefile"; then
        print_info "Makefile already configured for 64-bit support"
    else
        print_warning "Consider adding FORESTOS_TOOLCHAIN_HAS_64BIT flag to Makefile"
    fi
}

# Run main function
main "$@"