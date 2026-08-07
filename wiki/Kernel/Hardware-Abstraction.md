# Hardware Abstraction

Forest OS provides a modular hardware abstraction layer through the **Fern** kernel. Rather than a single monolithic HAL, the kernel exposes individual subsystem drivers — PCI, APIC, HPET, serial, and so on — each built as a standalone module gated by compile-time feature flags. This page documents how Forest discovers, configures, and communicates with physical hardware.

---

## Table of Contents

1. [PCI/PCIe Bus Enumeration](#1-pcipcie-bus-enumeration)
2. [ACPI Implementation](#2-acpi-implementation)
3. [APIC and IOAPIC Interrupt Controllers](#3-apic-and-ioapic-interrupt-controllers)
4. [HPET (High Precision Event Timer)](#4-hpet-high-precision-event-timer)
5. [TSC (Time Stamp Counter) Timer](#5-tsc-time-stamp-counter-timer)
6. [CMOS RTC (Real-Time Clock)](#6-cmos-rtc-real-time-clock)
7. [PS/2 Keyboard and Mouse Drivers](#7-ps2-keyboard-and-mouse-drivers)
8. [Serial Port (COM) Driver](#8-serial-port-com-driver)
9. [Parallel Port Driver](#9-parallel-port-driver)
10. [Storage Controllers](#10-storage-controllers)
11. [VirtIO Device Support](#11-virtio-device-support)
12. [I/O Port and MMIO Access Patterns](#12-io-port-and-mmio-access-patterns)
13. [DMA (Direct Memory Access) Management](#13-dma-direct-memory-access-management)
14. [PIC (8259A) Legacy Support](#14-pic-8259a-legacy-support)

---

## 1. PCI/PCIe Bus Enumeration

**Source files:** `pci.c`, `pcie.c`  
**Header:** `include/pci.h`, `include/pcie.h`  
**Build gate:** `ENABLE_PCI`, `ENABLE_PCIE`

Forest supports two PCI configuration space access methods and automatically selects the best one at boot:

- **Type 1 (CF8/CFC):** The traditional port I/O method using I/O ports `0xCF8` (address) and `0xCFC` (data). Works on every x86 system and is the safe fallback.
- **ECAM (Enhanced Configuration Access Mechanism):** The PCIe memory-mapped method discovered via the ACPI MCFG table. Maps a large MMIO region and directly indexes into each device's 4 KiB configuration space.

### Initialization Flow

1. `pci_init()` is called early in boot.
2. It queries `acpi_get_mcfg()` for the MCFG table. If found, it parses each segment entry (base address, bus range) and stores them in `g_segments[]`.
3. Before enabling ECAM, it performs a **safety test**: reads vendor ID from bus 0, device 0 via ECAM with a timeout. If the read hangs or returns `0xFFFFFFFF`, it falls back to Type1. This prevents hangs on VirtualBox and broken firmware.
4. Once the access mode is decided, `pci_enumerate()` walks all buses/devices/functions.

### Enumeration with Timeout Protection

To prevent infinite loops on buggy hardware, enumeration enforces a hard limit of **256 devices** (`PCI_ENUMERATION_LIMIT`). Each config read also has a timer-based timeout (`PCI_READ_TIMEOUT_TICKS`) — if a device doesn't respond within the window, it's treated as absent.

### PCIe Capability Walking

Forest traverses the PCIe standard capability list (offset `0x34`) and the extended capability list (offset `0x100+`). It identifies:

- **MSI** (`0x05`) and **MSI-X** (`0x11`) interrupt capabilities
- **Link speed** (Gen1 through Gen5) and **link width** (x1, x4, x16, etc.)
- **Device port type** (endpoint, root port, bridge, etc.)

### PCI Device Table Example

When `pcie_print_device_info()` is called, output looks like:

```
PCIe Device: 0000:00:01.0
  Vendor:Device: 8086:1237
  Class: 06:00:00
  Revision: 3
  PCIe Device: NO (Conventional PCI)
  BAR[0]: Memory - Type 0, Address: 0xFEBFE000
  BAR[4]: I/O - Address: 0xC000
```

For a real PCIe device:

```
PCIe Device: 0000:00:02.0
  Vendor:Device: 8086:100E
  Class: 02:00:00
  PCIe Device: YES
  PCIe Port Type: PCIe Endpoint
  PCIe Speed: 2.5 GT/s (Gen1)
  PCIe Width: x1
  BAR[0]: Memory - Type 0, Address: 0xFEBFC000
```

### API

```c
bool pci_init(void);
void pci_enumerate(pci_enum_callback_t callback, void* context);
bool pci_find_by_class(uint8 class_code, uint8 subclass, pci_device_t* out_device);
bool pci_find_by_vendor_device(uint16 vendor_id, uint16 device_id, pci_device_t* out_device);
uint8 pcie_find_capability_offset(uint16 seg, uint8 bus, uint8 dev, uint8 fn, uint8 cap_id);
```

---

## 2. ACPI Implementation

**Source files:** `acpi.c`, `acpi_enhanced.c`, `uacpi_port.c`  
**Headers:** `include/acpi.h`, `include/acpi_enhanced.h`  
**External library:** `libs/uacpi/`  
**Build gate:** `ENABLE_ACPI`

Forest ACPI support operates at two levels:

### Native Table Parser (`acpi.c`)

A lightweight parser that runs before heap allocation is available:

1. Scans the BIOS read-only area (`0xE0000`–`0xFFFFF`) for the RSDP signature (`"RSD PTR "`).
2. Validates the RSDP checksum and extracts the RSDT (ACPI 1.0) or XSDT (ACPI 2.0+) address.
3. Maps each SDT into kernel virtual memory via `acpi_map_table()` and validates its checksum.
4. Parses the **FADT** to enable ACPI mode (writes `acpi_enable` to the SMI command port, waits for PM1a status bit).
5. Parses the **MADT** to discover Local APIC addresses, I/O APICs, and interrupt source overrides.
6. Exposes `acpi_get_madt()`, `acpi_get_fadt()`, and `acpi_remap_irq()` for other subsystems.

### uACPI Integration (`uacpi_port.c`)

Forest integrates the third-party **uACPI** library (v3.2) for full ACPI namespace traversal, operation regions, and device evaluation. The `uacpi_port.c` file provides the kernel API callbacks uACPI requires:

- `uacpi_kernel_get_rsdp()` — finds the RSDP via the native parser
- `uacpi_kernel_map()` / `uacpi_kernel_unmap()` — identity-maps physical ACPI memory into kernel space
- `uacpi_kernel_alloc()` / `uacpi_kernel_free()` — delegates to the kernel heap
- `uacpi_kernel_log()` — routes to `debuglog`
- `uacpi_kernel_io_*()` — wraps `inportb`/`outportb` for operation region access
- `uacpi_kernel_mutex_create/lock/unlock/destroy()` — maps to kernel spinlocks
- `uacpi_kernel_pci_*()` — wraps `pci_config_read/write` for PCI config space access from AML

---

## 3. APIC and IOAPIC Interrupt Controllers

**Source files:** `apic.c`, `ioapic.c`  
**Headers:** `include/apic.h`, `include/io_apic.h`  
**Build gates:** `ENABLE_LOCAL_APIC`, `ENABLE_IOAPIC`

### Local APIC (`apic.c`)

The Local APIC handles per-CPU interrupt delivery, timer interrupts, and inter-processor interrupts (IPIs).

**Initialization sequence:**

1. Check CPUID for APIC support (`CPUID_FEAT_EDX_APIC`).
2. Read `MSR_APIC_BASE` to get the APIC base address and global enable flag.
3. Map the APIC register page into kernel virtual memory (cache-disabled mapping).
4. Verify the version register is readable (not `0xFFFFFFFF` or `0`).
5. Detect x2APIC support via CPUID and enable it if available (enables access to 2^32 APIC IDs via MSR rather than MMIO).
6. Configure the **spurious interrupt vector** (`0xFF`) with the APIC software enable bit.
7. Mask all LVT entries initially, then set up the error interrupt handler.
8. **Calibrate the APIC timer** against the PIT or TSC as a reference.
9. Accept all interrupt priorities (TPR = 0).
10. Disable the legacy 8259A PIC (`pic_8259a_disable()`).

**Timer calibration** uses a 10 ms window: set the LAPIC timer to max count with divisor 16, sleep for 10 ms using the PIT, then calculate the frequency from the difference. If the PIT isn't running yet (early boot), a TSC-based busy-wait fallback assumes ~1 GHz.

**IPI delivery** writes the destination APIC ID to `ICR_HIGH` and the vector + delivery mode to `ICR_LOW`, with a 1000-iteration timeout.

### I/O APIC (`ioapic.c`)

The I/O APIC routes external device interrupts to CPUs.

**Initialization sequence:**

1. Discover I/O APICs from the ACPI MADT (type 1 entries). Each entry provides the I/O APIC ID, MMIO base address, and global interrupt base.
2. Parse interrupt source overrides (type 2 entries) — these remap ISA IRQs to different global system interrupts (e.g., IRQ 0 → global IRQ 2).
3. Map each I/O APIC's register page into kernel memory.
4. Read the version register to determine the number of redirection table entries (typically 24 per I/O APIC).
5. Mask all redirection table entries.
6. Configure legacy ISA IRQ mappings: map IRQ 0–15 to vector numbers, apply polarity/trigger overrides from ACPI, and set the destination to the BSP initially.

**API for drivers:**

```c
int ioapic_enable_irq(uint8_t irq);
int ioapic_disable_irq(uint8_t irq);
int ioapic_set_affinity(uint8_t irq, uint32_t target_apic_id);
```

---

## 4. HPET (High Precision Event Timer)

**Source file:** `hpet.c`  
**Header:** `include/hpet.h`  
**Build gate:** `ENABLE_HPET`

The HPET provides a high-resolution (sub-microsecond) timer and up to 8 independent timer comparators.

### Discovery and Initialization

1. Map the standard HPET MMIO address (`0xFED00000`). A full implementation would parse the ACPI HPET table, but the standard address covers most systems.
2. Read the **General Capabilities and ID register** to extract:
   - Hardware revision
   - Number of timers (field at bits 8–12, plus 1)
   - Counter size (32-bit vs. 64-bit)
   - Vendor ID
   - Period in femtoseconds (from which frequency is derived: `1e15 / period_fs`)
3. Disable the HPET, reset the main counter to 0, then enable it.
4. For each timer, read its capabilities register to determine:
   - Whether it supports periodic mode
   - Whether it supports FSB interrupt delivery
   - IRQ routing capability

### Timer Operations

- **Periodic mode:** Allocate a timer, set `HPET_TN_TYPE_CNF` and `HPET_TN_VAL_SET_CNF`, write the period as tick count to the comparator register.
- **Oneshot mode:** Allocate a timer, clear `HPET_TN_TYPE_CNF`, compute target = current_counter + timeout_ticks.
- **Read counter:** Returns nanoseconds by converting `main_counter * period_femtoseconds / 1e6`.

The HPET integrates with the timer abstraction layer via a `timer_source` struct, allowing it to be used as the system's reference clock alongside the PIT and TSC.

---

## 5. TSC (Time Stamp Counter) Timer

**Source file:** `tsc_calibration.c`  
**Header:** `include/tsc_calibration.h`  
**Build gate:** `ENABLE_TSC`

The TSC counts CPU cycles since reset, providing the highest resolution timing on x86. However, it requires calibration because the frequency varies across CPUs.

### Calibration Methods

Forest supports multiple calibration strategies, falling back automatically:

1. **PIT-based calibration:** Count TSC ticks during a known PIT interval. Most accurate on systems with a constant-rate TSC.
2. **SMM-based calibration:** Use System Management Mode for a precise reference (where available).
3. **CPUID TSC frequency leaf:** Query `CPUID` leaf `0x15` or `0x80000007` for the TSC/crystal ratio.
4. **Fallback:** Assume 1 GHz if no reference is available.

### Features

- Detects **constant rate TSC** and **invariant TSC** via CPUID feature flags.
- Tracks counter drift across CPUs in SMP systems.
- Provides `rdtsc()` wrapper and `tsc_frequency_hz` global for other subsystems.
- Integrates with the APIC timer calibration as a timing reference when the PIT isn't ready.

---

## 6. CMOS RTC (Real-Time Clock)

**Source file:** `cmos_rtc.c`  
**Header:** `include/cmos_rtc.h`  
**Build gate:** `ENABLE_CMOS_RTC`

The MC146818-compatible RTC is accessed via I/O ports `0x70` (index) and `0x71` (data).

### Key Operations

- **Register read/write:** `cmos_inb()` / `cmos_outb()` set the index port (with NMI disable bit `0x80`), then read/write the data port. A small NOP delay settles the bus between port accesses.
- **BCD/Binary conversion:** Most RTCs store values in BCD; `cmos_bcd_to_binary()` and `cmos_binary_to_bcd()` handle the translation.
- **Update-in-progress (UIP) wait:** Before reading time registers, the driver polls the UIP bit in Status Register A with a 1,000,000-iteration timeout to ensure consistent values.
- **Time reading:** Reads seconds, minutes, hours, day-of-week, day-of-month, month, year, and century registers. Handles both 12-hour and 24-hour formats, and both BCD and binary modes.
- **Battery check:** Reads the VRT bit in Status Register D to verify CMOS battery health.

The RTC is used by the TTY status bar clock and provides the system's wall-clock time at boot.

---

## 7. PS/2 Keyboard and Mouse Drivers

**Source files:** `ps2_controller.c`, `ps2_keyboard.c`, `ps2_mouse.c`  
**Headers:** `include/ps2_controller.h`, `include/ps2_keyboard.h`, `include/ps2_mouse.h`  
**Build gate:** `ENABLE_PS2`

### PS/2 Controller (`ps2_controller.c`)

The 8042-compatible PS/2 controller is initialized through a well-defined sequence:

1. Disable keyboard and mouse devices during setup.
2. Flush the output buffer.
3. Read and modify the **configuration byte** — disables interrupts and translation during init, then re-enables them after device probing.
4. Run **controller self-test** (`0xAA`). If the response isn't `0x55`, log a warning but continue (many emulators return different values).
5. Test the **keyboard port** (`0xAB`).
6. Detect dual-channel (mouse) support by testing the auxiliary port (`0xA9`).
7. Enable keyboard and mouse interrupts.

### PS/2 Keyboard (`ps2_keyboard.c`)

The keyboard driver handles scan code decoding, modifier key state tracking, and layout translation:

- Processes scan codes from port `0x60` on IRQ 1.
- Supports **Set 1, Set 2, and Set 3** scan code sets.
- Translates scan codes to ASCII using configurable keyboard layouts loaded from `/usr/share/sysconf/keyboard.conf`.
- Manages modifier keys (Shift, Ctrl, Alt, GUI) and special keys (Caps Lock, Num Lock).
- Integrates with the **input event system** and **hotkey subsystem**.
- Polls via `ps2_keyboard_poll()` from the timer interrupt to prevent stuck buffers when IRQs are missed.

### PS/2 Mouse (`ps2_mouse.c`)

- Handles the 3-byte (standard) and 5-byte ( IntelliMouse) packet formats.
- Reports X/Y movement, button state, and scroll wheel data.
- Integrates with the **input event subsystem** for cursor movement.

---

## 8. Serial Port (COM) Driver

**Source file:** `serial_devices.c`  
**Header:** `include/serial_devices.h`  
**Build gate:** `ENABLE_SERIAL`

Forest provides drivers for four standard COM ports (COM1–COM4) exposed as character devices (`/dev/ttyS0` through `/dev/ttyS3`).

### Port Configuration

Each port is initialized with:

| Parameter | Value |
|-----------|-------|
| Baud rate | 38400 (divisor 3) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| FIFO | Enabled, 14-byte threshold |
| IRQs | Enabled, RTS/DSR set |

The initialization follows the standard sequence: disable interrupts, enable DLAB, set divisor, configure line control, enable FIFO, set modem control.

### Port Addresses

| Port | Base Address | IRQ |
|------|-------------|-----|
| COM1 | `0x3F8` | 4 |
| COM2 | `0x2F8` | 3 |
| COM3 | `0x3E8` | 4 |
| COM4 | `0x2E8` | 3 |

### Device Operations

The driver registers with the device filesystem using standard `read()` and `write()` operations. It maintains 1024-byte input and output ring buffers for each port and supports direct polling as a fallback.

---

## 9. Parallel Port Driver

**Source file:** `parallelport.c`  
**Header:** `include/parallelport.h`  
**Build gate:** `ENABLE_PARALLEL`

The parallel port driver supports up to three LPT ports in SPP (Standard Parallel Port) mode.

### Port Detection

Detection works by toggling the control register and checking the status register. A valid port returns `0xB8` from the status read after initialization.

### Data Transfer Protocol

1. **Wait for ready:** Poll the BUSY bit in the status register.
2. **Write data byte** to the data register.
3. **Strobe:** Pulse the STROBE control bit for 1 ms (assert then de-assert).
4. **Wait for ACK** (or BUSY clear) to confirm receipt.

### Port Addresses

| Port | I/O Base |
|------|----------|
| LPT1 | `0x378` |
| LPT2 | `0x278` |
| LPT3 | `0x3BC` |

---

## 10. Storage Controllers

Forest supports multiple storage controller types, each gated by its own feature flag.

### ATA/PI (`ata.c`) — `ENABLE_ATA`

The legacy ATA (IDE) driver operates on the primary and secondary channels:

- **I/O ports:** Primary `0x1F0`/`0x3F6`, Secondary `0x170`/`0x376`
- **PIO mode** for data transfer with 30-second timeout for identification
- Detects devices via the alternate status register
- Handles both ATA and ATAPI devices
- Uses `read_tsc()` for timeout measurement

### AHCI (`ahci.c`) — `ENABLE_AHCI`

The SATA AHCI driver provides high-performance storage access:

- Discovered via PCI class `0x01` (mass storage), subclass `0x06` (AHCI)
- MMIO-based register access with cache-disabled mappings
- Command slot management (up to 32 command slots)
- Port-level command list, FIS, and PRDT structures
- Timeout-based completion waiting with `timer_get_ticks()`
- Supports up to 32 ports per controller

### NVMe (`nvme.c`) — `ENABLE_NVME`

The NVMe driver supports high-speed NVMe storage:

- Discovered via PCI class `0x01`, subclass `0x08`
- Admin and I/O queue management
- TSC-based timeout for controller ready waiting
- IRQ-driven completion with interrupt status register acknowledgment
- Driver model integration via `drv_driver_t` registration

### SCSI (`scsi.c`) — `ENABLE_SCSI`

A SCSI device abstraction layer providing:

- Device allocation by target ID and LUN
- CDB (Command Descriptor Block) execution interface
- Reset and inquiry operations
- PCI SCSI controller discovery (class `0x01`, subclass `0x00`)

### Block Device Layer

All storage controllers feed into the unified block device layer (`block_devices.c`), which provides a common interface for the VFS.

---

## 11. VirtIO Device Support

**Source files:** `virtio.c`, `virtio_net.c`, `virtio_snd.c`  
**Headers:** `include/virtio.h`, `include/virtio_net.h`, `include/virtio_snd.h`  
**Build gate:** `ENABLE_VIRTIO`

Forest supports VirtIO devices for both legacy (transitional) and modern (1.0+) transports, primarily targeting virtualized environments (QEMU/KVM).

### Device Discovery

1. Walk PCI bus for vendor `0x1AF4` (Red Hat).
2. Classify by device ID:
   - Legacy: `0x1000–0x103F` → sub-ID = `device_id & 0xFF`
   - Modern: `0x1040–0x107F` → sub-ID = `device_id - 0x1040`
3. Recognized device types: network, block, console, balloon, input, GPU.

### Transport Detection

- If BAR0 bit 0 is set → I/O port space (legacy)
- If BAR0 bit 0 is clear → MMIO space (modern)

### Supported Devices

| Device | Type | Source |
|--------|------|--------|
| virtio-net | Network | `virtio_net.c` |
| virtio-blk | Block | `virtio.c` (probe) |
| virtio-snd | Audio | `virtio_snd.c` |
| virtio-console | Console | recognized, not fully wired |
| virtio-balloon | Memory | recognized, not fully wired |
| virtio-gpu | GPU | recognized, not fully wired |

---

## 12. I/O Port and MMIO Access Patterns

**Headers:** `include/io.h`, `include/io_ports.h`, `include/system.h`

Forest provides two fundamental hardware access mechanisms.

### Port I/O (x86 `in`/`out` instructions)

```c
// Byte access
uint8_t inb(uint16_t port);    // alias for inportb()
void outb(uint16_t port, uint8_t data);  // alias for outportb()

// Word access (16-bit)
uint16_t inw(uint16_t port);   // alias for inportw()
void outw(uint16_t port, uint16_t data);

// Double-word access (32-bit)
uint32_t inl(uint16_t port);   // alias for inportd()
void outl(uint16_t port, uint32_t data);
```

Used by: PCI Type1 config, serial ports, parallel ports, PS/2 controller, CMOS RTC, PIT, PIC, ATA.

### Memory-Mapped I/O (MMIO)

MMIO regions are accessed through pointers to mapped physical memory. The kernel uses `mm_map_physical_page()` to create virtual-to-physical mappings with the `PAGE_CACHE_DISABLE` flag to prevent CPU speculation from bypassing MMIO reads/writes.

Used by: APIC registers, I/O APIC registers, HPET registers, AHCI HBA, NVMe controller, PCIe ECAM.

---

## 13. DMA (Direct Memory Access) Management

**Source file:** `iommu.c`  
**Header:** `include/iommu.h`  
**Build gate:** `ENABLE_IOMMU`

### IOMMU Support

Forest includes a baseline IOMMU subsystem (Intel VT-d / AMD-Vi) that:

1. Detects IOMMU presence via ACPI tables (DMAR for Intel, IVRS for AMD).
2. Operates in **passthrough mode** by default — the BIOS's identity mapping is preserved, allowing device DMA to work without OS-managed page tables.
3. Exposes `iommu_map()` and `iommu_unmap()` for future per-device DMA remapping.

When `ENABLE_IOMMU=0` (the default), all IOMMU calls are no-ops, and devices DMA directly to physical memory. This is the standard configuration for most development and single-user scenarios.

### DMA Buffer Considerations

Storage and network drivers allocate DMA-capable buffers through the kernel's page allocator. The AHCI, NVMe, and VirtIO drivers use physically contiguous memory for command lists, FIS structures, and scatter-gather lists. The `PAGE_CACHE_DISABLE` flag is used on MMIO-mapped DMA descriptor regions to ensure coherency.

---

## 14. PIC (8259A) Legacy Support

**Source file:** `pic_8259a.c`  
**Build gate:** `ENABLE_PIC_8259A`

The 8259A PIC is Forest's **fallback interrupt controller**. It remains operational on systems without APIC and is disabled only after the APIC is confirmed working.

### Initialization

1. Save original IRQ masks.
2. **Remap** PIC vectors to avoid conflict with CPU exceptions:
   - Master PIC: IRQ 0–7 → vectors `0x20–0x27`
   - Slave PIC: IRQ 8–15 → vectors `0x28–0x2F`
3. Send ICW1 (initialization command) to both PICs.
4. Send ICW2 (vector base), ICW3 (cascade config), ICW4 (8086 mode).
5. Mask all IRQs except the cascade line (IRQ 2).
6. Register spurious interrupt handlers for IRQ 7 (master) and IRQ 15 (slave).

### Key API

```c
void pic_8259a_disable(void);       // Fully mask all IRQs
void pic_unmask_irq(uint8_t irq);   // Enable specific IRQ
void pic_mask_irq(uint8_t irq);     // Disable specific IRQ
void pic_send_eoi(uint8_t irq);     // End-of-interrupt
```

### Relationship with APIC

The PIC and APIC coexist with a clear handoff:

- The PIC is initialized first (it's the only interrupt controller at boot).
- Timer, keyboard, and other early interrupts use PIC vectors.
- After `local_apic_init()` succeeds, `pic_8259a_disable()` masks all PIC IRQs.
- The I/O APIC then takes over external interrupt routing.
- If APIC initialization fails (e.g., PIC-only QEMU mode), the PIC remains the active controller.

---

## Build System Integration

Hardware subsystems are gated through Make feature files:

| File | Subsystems |
|------|-----------|
| `build/features/hardware.mk` | PCI, PCIe, ACPI, serial, parallel, A20, TTY |
| `build/features/interrupts.mk` | PIC, APIC, I/O APIC, NMI, MSI, interrupt priorities |
| `build/features/timers.mk` | PIT, HPET, APIC timer, CMOS RTC, TSC, timer abstraction |
| `build/features/storage.mk` | ATA, AHCI, NVMe, SCSI, FDC, block devices |

Each `ENABLE_*=no` flag appends the corresponding source file to `EXCLUDED_CSOURCES`, removing it from the build. For advanced features that are excluded by default (MSI, interrupt priorities), `ENABLE_*=yes` adds the object to `KERN_EXTRA_OBJS`.

All wildcard guards ensure missing source files never break the build — a file that doesn't exist is simply skipped.

---

## Summary

Forest's hardware abstraction is designed around **pragmatic layering**: a minimal native ACPI parser runs at boot before the heap exists, individual hardware drivers are independently gated for minimal kernel images, and the uACPI library provides full ACPI namespace support once memory management is online. The PIC-to-APIC handoff, ECAM-to-Type1 fallback, and PIT-to-TSC timer fallback patterns ensure the kernel boots reliably across diverse hardware while taking advantage of advanced features when present.
