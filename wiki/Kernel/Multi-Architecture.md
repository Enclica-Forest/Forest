# Multi-Architecture Support in Forest OS

Forest OS is a hobby operating system kernel designed to run on multiple CPU architectures from a single, unified codebase. This page covers how the kernel organizes architecture-specific code, the cross-architecture abstraction layer, and the details of each supported architecture.

## Supported Architectures

Forest OS currently supports five architecture targets:

| Architecture | ARCH value | Bits | ISA | Page Table | Example Target |
|---|---|---|---|---|---|
| x86 (32-bit) | `32` | 32 | IA-32 (i686) | 2-level paging | QEMU i440FX |
| x86_64 (64-bit) | `64` | 64 | AMD64 | 4-level (PML4) | QEMU Q35/qemu64 |
| ARM32 | `arm` | 32 | ARMv7-A (Cortex-A15) | Short-descriptor (L1+L2) | QEMU virt, Raspberry Pi 2/3 |
| AArch64 | `aarch64` | 64 | ARMv8-A (Cortex-A53) | 4-level (L0→L3) | QEMU virt, Raspberry Pi 3/4 |
| RISC-V 64 | `riscv64` | 64 | RV64GC | Sv39 (3-level) | QEMU virt |

All architectures use 4 KB pages and are little-endian.

## Code Organization

The kernel source tree splits architecture-specific code into two main areas:

### 1. The Cross-Architecture Abstraction Layer (`src/arch/`)

This directory contains headers and C files that are **compiled for every architecture**. The build system selects the correct architecture-specific implementations at compile time using preprocessor conditionals.

Key files:

- **`arch.h`** — The single entry point for all architecture definitions. Detects the target architecture via compiler predefined macros (`__x86_64__`, `__aarch64__`, `__arm__`, `__riscv`, etc.) and defines `ARCH_X86_32`, `ARCH_X86_64`, `ARCH_ARM32`, `ARCH_ARM64`, `ARCH_RISCV64`. Provides abstract types (`arch_word_t`, `arch_paddr_t`, `arch_vaddr_t`), abstract CPU state (`arch_cpu_state_t` — a union of per-arch register frames), and inline operations (`arch_get_sp()`, `arch_halt()`, `arch_enable_irq()`, `arch_disable_irq()`, `arch_cpu_relax()`).

- **`arch_ops.c`** — Implements `arch_init()` (per-arch early init), `arch_get_name()`, `arch_get_page_size()` (always 4096), `arch_supports_feature()` (lazy feature detection with caching), and platform UART stubs for early boot console output.

- **`barrier.h`** — Unified memory barrier interface: `arch_mb()` (full barrier), `arch_rmb()` (read barrier), `arch_wmb()` (write barrier), `arch_compiler_barrier()`, and ARM-specific `arch_dsb()` / `arch_isb()`.

- **`platform.h`** — Platform/board detection and MMIO base addresses. Auto-detects platform from architecture. Defines MMIO bases for QEMU x86, QEMU ARM virt, Raspberry Pi 3, and Raspberry Pi 4.

### 2. Architecture-Specific Directories

Each architecture has its own directory with full implementations:

```
src/arm32/       31 files — boot, exceptions, context switch, MMU, GIC, UART, timer, etc.
src/aarch64/     34 files — boot, exceptions, context switch, MMU, GICv3, UART, FPU, UEFI, etc.
src/riscv64/     27 files — boot, trap handling, context switch, MMU, CLINT, PLIC, UEFI, etc.
src/x86_64/       2 files — signal.c, smp.c (most x86 code is in src/ directly)
```

Additionally, each architecture has a thin header in `src/arch/{arch}/`:

```
src/arch/x86_32/arch_x86_32.h     — GDT, IDT, TSS, EFLAGS, CR0-CR4, CPUID, port I/O
src/arch/x86_64/arch_x86_64.h     — 64-bit GDT/IDT/TSS, RFLAGS, MSRs, SYSCALL/SYSRET, 4-level paging
src/arch/arm32/arch_arm32.h        — CPSR, CP15 accessors, MMU short-descriptor, cache/TLB ops
src/arch/aarch64/arch_aarch64.h    — Exception levels, system registers, GICv3 ICC, 4-level paging
src/arch/riscv64/arch_riscv64.h    — CSRs, Sv39 page tables, TLB/cache maintenance
```

These headers are pulled in automatically by `arch.h` based on the detected architecture — you never include them directly.

## The Cross-Architecture Abstraction Layer

The abstraction layer provides a unified API that kernel subsystems use without caring which architecture they're running on. Here's how it works:

### Architecture Detection

`arch.h` uses compiler-predefined macros to set exactly one `ARCH_*` flag to 1 and all others to 0:

```c
#if defined(__x86_64__) || defined(_M_X64)
#   define ARCH_X86_64   1
#   define ARCH_BITS     64
#   define ARCH_NAME     "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#   define ARCH_ARM64    1
#   define ARCH_BITS     64
#   define ARCH_NAME     "aarch64"
// ... etc for each architecture
```

Convenience macros like `ARCH_IS_X86`, `ARCH_IS_ARM`, `ARCH_IS_64BIT`, and `ARCH_IS_32BIT` are derived from these.

### Abstract CPU State

Each architecture defines its own CPU state structure, but the kernel sees only `arch_cpu_state_t`:

```c
// x86_64: r15-r8, rdi-rbx, rdx-rcx, rax, interrupt frame
// ARM32:  r0-r12, sp, lr, pc, cpsr, spsr
// AArch64: x0-x29, x30 (lr), sp, pc, spsr_el1, elr_el1, esr_el1, far_el1
// RISC-V: ra, gp, tp, t0-t6, s0-s11, a0-a7, sstatus, sepc, scause, stval
```

### Inline Operations

Architecture-neutral inline functions compile to a single instruction per architecture:

| Operation | x86/x86_64 | ARM32 | AArch64 | RISC-V |
|---|---|---|---|---|
| `arch_get_sp()` | `mov %rsp, %0` | `mov %0, sp` | `mov %0, sp` | `mv %0, sp` |
| `arch_halt()` | `hlt` | `wfi` | `wfi` | `wfi` |
| `arch_enable_irq()` | `sti` | `cpsie i` | `msr daifclr, #2` | `csrsi sstatus, 0x2` |
| `arch_disable_irq()` | `cli` | `cpsid i` | `msr daifset, #2` | `csrci sstatus, 0x2` |
| `arch_cpu_relax()` | `pause` | `yield` | `yield` | `rep.nop` |

### Memory Barriers

```c
arch_mb();    // Full barrier: x86=mfence, ARM=dmb sy, RISC-V=fence rw,rw
arch_rmb();   // Read barrier: x86=lfence, ARM=dmb ishld, RISC-V=fence r,r
arch_wmb();   // Write barrier: x86=sfence, ARM=dmb ishst, RISC-V=fence w,w
```

### Feature Detection

`arch_supports_feature()` lazily detects CPU capabilities at runtime and caches the result:

- **x86/x86_64**: Uses CPUID leaves 1 and 7 to detect FPU, SSE2, AVX-512, TSX, AES-NI, VMX, SMP (APIC)
- **ARM32**: Reads MPIDR (SMP), ID_ISAR0 (FPU), MVFR1 (NEON), SCTLR bits
- **AArch64**: Reads MPIDR_EL1, ID_AA64PFR0_EL1 (SVE, EL2), ID_AA64ISAR0_EL1 (AES, CRC32)
- **RISC-V**: Feature detection is not yet implemented in `arch_ops.c` (the function exists but no `detect_features_riscv64()` is present — this is a TODO)

Feature flags include: `ARCH_FEAT_FPU`, `ARCH_FEAT_SIMD`, `ARCH_FEAT_MMU`, `ARCH_FEAT_SMP`, `ARCH_FEAT_ATOMIC64`, `ARCH_FEAT_NEON`, `ARCH_FEAT_SVE`, `ARCH_FEAT_AVX512`, `ARCH_FEAT_TSX`, `ARCH_FEAT_CRYPTO`, `ARCH_FEAT_CRC32`, `ARCH_FEAT_VIRT`.

## x86 (32-bit) Specifics

**Toolchain**: `i686-forestos-gcc` (Forest OS cross-compiler)
**Linker flags**: `-m elf_i386`
**NASM flags**: `-f elf386`

Key features:
- **Paging**: 2-level page directory/table (1024 PDEs × 1024 PTEs = 4 GB VA space)
- **Interrupts**: 8-byte IDT gate descriptors, PIC or APIC
- **Boot**: Multiboot-compliant, loaded at 1 MB by BIOS/GRUB
- **GDT/IDT/TSS**: Full IA-32 protected mode structures defined in `arch_x86_32.h`
- **Port I/O**: `inb`/`outb`/`inw`/`outw`/`inl`/`outl` inline functions
- **Syscalls**: Via `int 0x80` or `sysenter`/`sysexit`

Compiler flags: `-m32 -march=i386 -mtune=i386`, with `-mno-red-zone -mno-sse -mno-sse2 -mfpmath=387` for kernel code.

## x86_64 (64-bit) Specifics

**Toolchain**: `x86_64-forestos-gcc` (Forest OS cross-compiler, required)
**Linker flags**: `-m elf_x86_64`
**NASM flags**: `-f elf64`

Key features:
- **Long mode**: 64-bit paging with 4-level page tables (PML4 → PDP → PD → PT), 48-bit virtual address space
- **SYSCALL/SYSRET**: Fast syscall via MSR configuration (LSTAR, STAR, SFMAP)
- **MSR access**: `rdmsr`/`wrmsr` for EFER, FS_BASE, GS_BASE, SYSCALL MSRs
- **GS base / SWAPGS**: Per-CPU kernel data via GS.base swap on syscall entry
- **NX bit**: Execute Disable via EFER.NXE
- **SMEP/SMAP**: Supervisor Mode Execution/Access Prevention (detected via CPUID leaf 7)
- **5-level paging**: Optional LA57 support (gated by `ENABLE_5LEVEL_PAGING`)
- **Interrupts**: 16-byte IDT entries with IST (Interrupt Stack Table) support
- **TSS**: 64-bit TSS with rsp0 (ring-0 stack) and ist1-ist7

Compiler flags: `-m64 -march=x86-64 -mcmodel=kernel`, with `-mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 -mno-80387 -msoft-float`.

The `x86_64/` directory contains `signal.c` and `smp.c`. Most x86-specific code lives in the main `src/` directory (boot, interrupt stubs, GDT/IDT/PIC/APIC drivers).

## ARM32 Specifics

**Toolchain**: `arm-none-eabi-gcc` or `arm-linux-gnueabi-gcc`
**Linker flags**: `-m armelf`
**Compiler flags**: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp -marm`

Key features:
- **Boot** (`boot.S`): Starts in SVC mode, masks interrupts, installs VBAR, sets up per-mode stacks (SVC: 8KB, IRQ: 4KB, FIQ: 1KB, ABT: 2KB, UND: 1KB), zeroes BSS, copies .data, initializes PL011 UART, calls `kernel_main(dtb_addr)`.
- **Exception vectors**: 8-entry table with `LDR PC, [PC, #offset]` trampolines. Each handler saves full context via `SAVE_CONTEXT` macro (pushes r0-r12, lr, spsr).
- **MMU**: VMSAv7 short-descriptor format. L1 table (4096 entries × 4 bytes = 16 KB) with 1 MB section descriptors or L2 page table pointers. L2 tables (256 entries × 4 bytes = 1 KB) with 4 KB small page descriptors. Domain-based access control.
- **Context switch** (`context_switch.S`): Saves/restores callee-saved registers (r4-r11, lr) and VFP/NEON state (s16-s31, fpscr). User-mode entry via `ldmfd sp!, {pc}^` which atomically restores PC and CPSR.
- **Interrupt controller**: GIC (Generic Interrupt Controller) — see `gic.c`/`gic.h`
- **UART**: PL011 MMIO UART at `0x09000000` (QEMU virt)
- **Timer**: ARM Generic Timer or BCM system timer
- **VFP/NEON**: Lazy save/restore in context switch and exception handlers
- **Syscalls**: SWI (Supervisor Call) with Linux ARM EABI convention — syscall number in r7, args in r0-r6

Platform support: QEMU virt (cortex-a15), Raspberry Pi 2/3 (BCM2837).

## AArch64 (ARM 64-bit) Specifics

**Toolchain**: `aarch64-linux-gnu-gcc` or `aarch64-none-elf-gcc`
**Linker flags**: `-m aarch64elf`
**Compiler flags**: `-march=armv8-a`

Key features:
- **Exception levels**: EL0 (user), EL1 (kernel), EL2 (hypervisor), EL3 (secure monitor). Boot code handles EL3→EL2→EL1 transitions.
- **Boot** (`boot.S`): Parks secondary CPUs, determines current EL, configures HCR_EL2 (EL1 is AArch64), sets up SCTLR_EL1, enables FP/SIMD via CPACR_EL1, installs vector table at VBAR_EL1, zeroes BSS, sets up 16-byte-aligned kernel stack, calls `kernel_main(dtb_addr)`.
- **Exception vectors** (`exceptions.S`): 16-entry table (2 KB aligned, each entry 128 bytes). Four groups: Current EL with SP0, Current EL with SPx, Lower EL (AArch64), Lower EL (AArch32). Syscalls detected via ESR_EL1.EC = 0x15 (SVC from AArch64).
- **MMU**: 4-level page table (L0→L1→L2→L3), 4 KB granule, 48-bit VA. TTBR0 for user space (0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF), TTBR1 for kernel (0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF). MAIR_EL1 for memory attributes (Normal WB, Device nGnRnE, Normal NC).
- **Context switch** (`context_switch.S`): Saves/restores x19-x30, sp_el0, and page table registers (sctlr_el1, ttbr0_el1, tcr_el1, mair_el1). TLB flush only when ttbr0 changes. NEON/FP state saved/restored via `fpu_save()`/`fpu_restore()`. User-mode entry via `eret`.
- **Interrupt controller**: GICv3 with system register CPU interface (ICC_SRE_EL1, ICC_IAR1_EL1, ICC_EOIR1_EL1). SGIs via ICC_SGI1R_EL1.
- **UART**: PL011 MMIO UART
- **UEFI boot**: `uefi_boot.c`/`uefi_boot.S` for UEFI boot path
- **Syscalls**: Linux AArch64 convention — x8=syscall number, x0-x7=args, x0=return value

Platform support: QEMU virt (cortex-a53), Raspberry Pi 3/4.

## RISC-V 64 Specifics

**Toolchain**: `riscv64-unknown-elf-gcc` or `riscv64-linux-gnu-gcc`
**Linker flags**: `-m elf64lriscv`
**Compiler flags**: `-march=rv64gc -mabi=lp64d -mcmodel=medany`

Key features:
- **Privilege modes**: M-mode (machine), S-mode (supervisor), U-mode (user). Kernel runs in S-mode.
- **Boot** (`boot.S`): OpenSBI firmware loads kernel at `0x80200000` in M-mode. Boot code parks secondary harts, configures mstatus (MPP=supervisor, MPIE=1), sets mepc to S-mode entry, executes `ecall` to invoke OpenSBI runtime which transitions to S-mode. S-mode code zeroes BSS, sets up stack, calls `kernel_main(dtb_addr)`.
- **Trap handling** (`trap.S`): Uses Direct mode (stvec = base address). Single `trap_vector` entry point decodes scause: bit 63 = interrupt, code 8 = ecall from U-mode (syscall), others = exception. Full register save/restore via `save_regs`/`restore_regs` macros (272-byte frame: all GPRs + sstatus, sepc, scause, stval).
- **MMU**: Sv39 (3-level page table, 39-bit VA, 4 KB granule). Virtual addresses sign-extended from bit 38. Kernel at `0xFFFFFF80_80000000`, user space up to `0x0000003F_FFFFFFFF` (256 GB). SATP register for mode/ASID/PPN. TLB flush via `sfence.vma`.
- **Context switch** (`context_switch.S`): Saves/restores s0-s11, ra, sp (112-byte frame). User-mode entry via `sret` with sstatus.SPP=0 (return to U-mode), sepc = user entry point.
- **Interrupt controller**: PLIC (Platform-Level Interrupt Controller) — see `plic.c`/`plic.h`
- **Timer**: CLINT (Core-Local Interruptor) — see `clint.c`/`clint.h`, provides machine timer and software interrupts
- **UART**: Memory-mapped UART
- **UEFI boot**: `uefi_boot.c`/`uefi_boot.S` for UEFI boot path
- **Syscalls**: Linux RISC-V convention — a7=syscall number, a0-a5=args, a0=return value

Platform support: QEMU virt with OpenSBI firmware.

## Architecture-Specific Assembly Code

Each architecture has hand-written assembly for the low-level kernel paths that cannot be written in C:

### Boot Sequences

| Architecture | Entry point | First C function | Key steps |
|---|---|---|---|
| x86 (32-bit) | `_start` (boot.asm) | `kernel_main` | Multiboot header, GDT, IDT, paging, BSS clear |
| x86_64 | `_start` (boot64.asm) | `kernel_main` | Long mode transition, 4-level paging, BSS clear |
| ARM32 | `_start` → `reset_handler` (boot.S) | `kernel_main(dtb)` | SVC mode, VBAR, per-mode stacks, PL011 init, BSS clear, .data copy |
| AArch64 | `_start` (boot.S) | `kernel_main(dtb)` | EL3→EL2→EL1, HCR_EL2, CPACR_EL1, VBAR_EL1, BSS clear |
| RISC-V | `_start` → `smode_entry` (boot.S) | `kernel_main(dtb)` | M-mode config, ecall to OpenSBI, S-mode, BSS clear |

### Context Switching

All architectures save callee-saved registers to the kernel stack and switch stack pointers. The implementations differ in what they save:

- **ARM32**: r4-r11, lr (via `stmfd`/`ldmfd`), plus VFP s16-s31 and fpscr
- **AArch64**: x19-x30, sp_el0, plus page table registers (sctlr_el1, ttbr0_el1, tcr_el1, mair_el1), plus NEON/FP via `fpu_save()`/`fpu_restore()`
- **RISC-V**: s0-s11, ra, sp (via `sd`/`ld`)
- **x86/x86_64**: Managed via TSS ring-0 stack switching and ISR stubs (not a separate context_switch.S)

### Exception/Interrupt Handling

- **ARM32**: 8-entry vector table, each entry is `LDR PC, [PC, #offset]`. Handlers save context, call C dispatchers, restore and return via `ldmfd sp!, {pc}^`.
- **AArch64**: 16-entry vector table (2 KB aligned, 128 bytes each). Syscalls detected by ESR_EL1.EC field. Returns via `eret`.
- **RISC-V**: Single trap entry decodes scause. Syscalls (ecall from U-mode, code 8) dispatch to `riscv64_syscall_handle()`. Returns via `sret`.
- **x86/x86_64**: Interrupt stubs in `.asm`/`.s` files push error codes and interrupt numbers, jump to common ISR handler.

### User-Mode Entry

All architectures provide `task_start_usermode_asm()` and `enter_usermode_asm()`:

- **ARM32**: Sets USR-mode SP (banked, must switch modes), builds SVC return frame, executes `ldmfd sp!, {pc}^`
- **AArch64**: Sets sp_el0, loads entry into elr_el1, configures spsr_el1 for EL0t, executes `eret`
- **RISC-V**: Sets sepc to user entry, configures sstatus (SPP=0, SPIE=1), saves user sp in sscratch, executes `sret`

## Build Targets

The Makefile provides convenient per-architecture build targets:

```bash
make build32              # x86 32-bit (BIOS)
make build64              # x86 64-bit (BIOS)
make buildarm             # ARM32
make buildaarch64         # AArch64 (BIOS)
make buildaarch64-uefi    # AArch64 (UEFI)
make buildriscv64         # RISC-V 64 (BIOS)
make buildriscv64-uefi    # RISC-V 64 (UEFI)
make buildall             # All combinations
```

Each target invokes `make` with the appropriate `ARCH=` and `BOOT_MODE=` overrides. The build system generates architecture-specific output directories:

```
build/32bit-bios-debug/
build/64bit-uefi-release/
build/arm-bios-debug/
build/aarch64-bios-debug/
build/riscv64-bios-debug/
```

QEMU run targets are also available:

```bash
make run32       # QEMU q35, 32-bit
make run64       # QEMU q35, 64-bit
make runarm      # QEMU virt, cortex-a15
make runaarch64  # QEMU virt, cortex-a53
```

## Cross-Compilation Setup

### Toolchain Detection

The build system (`build/toolchain.mk`) auto-detects available cross-compilers:

- **x86/x86_64**: Requires the Forest OS cross-toolchain (`i686-forestos-gcc` / `x86_64-forestos-gcc`) built from `forestos-toolchain/`. A fallback host toolchain is available for 64-bit when the cross-compiler is unavailable.
- **ARM32**: Auto-detects `arm-none-eabi-gcc`, `arm-linux-gnueabi-gcc`, or `arm-linux-gnueabihf-gcc`
- **AArch64**: Auto-detects `aarch64-linux-gnu-gcc` or `aarch64-none-elf-gcc`
- **RISC-V**: Auto-detects `riscv64-unknown-elf-gcc`, `riscv64-linux-gnu-gcc`, or `riscv64-elf-gcc`

### Architecture Flags

| ARCH | ARCH_FLAGS | ARCH_LDFLAGS |
|---|---|---|
| 32 | `-m32 -march=i386 -mtune=i386` | `-m elf_i386` |
| 64 | `-m64 -march=x86-64 -mcmodel=kernel` | `-m elf_x86_64` |
| arm | `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp -marm` | `-m armelf` |
| aarch64 | `-march=armv8-a` | `-m aarch64elf` |
| riscv64 | `-march=rv64gc -mabi=lp64d -mcmodel=medany` | `-m elf64lriscv` |

### Kernel Source Selection

`build/kernel-sources.mk` selects architecture-specific source files based on `ARCH`:

```makefile
ifeq ($(ARCH),arm)
    ARCH_EXTRA_OBJECTS := $(ARM32_OBJECTS)
else ifeq ($(ARCH),aarch64)
    ARCH_EXTRA_OBJECTS := $(AARCH64_OBJECTS)
else ifeq ($(ARCH),riscv64)
    ARCH_EXTRA_OBJECTS := $(RISCV64_OBJECTS)
else ifeq ($(ARCH),64)
    ARCH_EXTRA_OBJECTS := $(X86_64_OBJECTS)
endif
```

The `src/arch/*.c` shared objects are always linked regardless of architecture.

### Linker Scripts

Each architecture has its own linker script:

- x86 32-bit: `src/link.ld`
- x86 64-bit: `src/link64.ld`
- ARM32: `src/arm32/link.ld`
- AArch64: `src/aarch64/link.ld`
- RISC-V 64: `src/riscv64/link.ld` (BIOS) or `src/riscv64/link_uefi.ld` (UEFI)

## Current Status

| Architecture | Boot | MMU | Interrupts | Context Switch | Syscalls | SMP | UEFI | Status |
|---|---|---|---|---|---|---|---|---|
| x86 (32-bit) | Complete | Complete | Complete | Complete | Complete | Partial | Yes | Working |
| x86_64 | Complete | Complete | Complete | Complete | Complete | Partial | Yes | Working |
| ARM32 | Complete | Complete | Complete (GIC) | Complete | Complete | In progress | No | Working |
| AArch64 | Complete | Complete | Complete (GICv3) | Complete | Complete | In progress | Yes | Working |
| RISC-V 64 | Complete | Complete | Complete (PLIC) | Complete | Complete | In progress | Yes | In progress |

### Known TODOs

- **RISC-V**: `arch_supports_feature()` has no `detect_features_riscv64()` implementation yet — feature detection always returns 0.
- **SMP**: All architectures have `smp.c` files but multi-core bring-up is in progress across the board.
- **x86 32-bit**: Most x86-specific code lives in `src/` directly rather than `src/x86_32/`, making it less cleanly separated than the ARM/RISC-V ports.
- **Signal handling**: x86_64, ARM32, AArch64, and RISC-V all have `signal.c` — this is one of the more complete cross-arch subsystems.
- **Sound drivers**: Each architecture has its own `sound_{arch}.c` for architecture-specific audio hardware support.

---

*This page covers the Fern kernel's multi-architecture support. For the build system configuration, see [Build System](Build-System.md). For memory management details, see [Memory Management](Memory-Management.md).*
