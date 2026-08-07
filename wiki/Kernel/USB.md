# USB Subsystem

The USB subsystem provides support for Universal Serial Bus devices across all major host controller standards. It handles controller enumeration, device discovery, descriptor parsing, class driver management, and data transfers for keyboards, mice, storage devices, and more.

## Architecture Overview

The USB subsystem is organized into three layers:

1. **USB Core** (`usb_core.c`) — Device enumeration, address allocation, descriptor parsing, and class driver matching.
2. **Host Controller Drivers (HCDs)** — UHCI, OHCI, EHCI, and xHCI drivers that translate generic USB operations into controller-specific hardware register programming.
3. **Class Drivers** — USB HID (keyboards, mice) and USB Mass Storage (flash drives, external HDDs).

### Initialization Flow

```
usb_init()
  → pci_enumerate()           // Scan PCI bus for USB controllers
  → usb_probe_controller()    // Allocate controller, map MMIO, call HCD init
  → usb_enumerate_ports()     // For each port: detect device, reset, enumerate
  → usb_enumerate_device()    // Assign address, read descriptors, set config
  → usb_match_class_driver()  // Match interface to HID/MSC/other driver
```

### Key Data Structures

The core structures are defined in `fern/src/include/usb/usb.h`:

- **`usb_controller_t`** — Represents a USB host controller. Contains the PCI device, base address, port count, device table, and an ops vtable (`usb_controller_ops_t`) for controller-specific operations.
- **`usb_device_t`** — Represents an enumerated USB device. Holds vendor/product IDs, speed, configurations, endpoints, and HCD-specific data.
- **`usb_class_driver_t`** — A class driver registered with the core, matched by class/subclass/protocol.
- **`usb_controller_ops_t`** — Function pointer table (vtable) that each HCD implements: `init`, `shutdown`, `reset_port`, `control_transfer`, `bulk_transfer`, `interrupt_transfer`, `poll`.

### PCI Discovery

The USB core scans the PCI bus during initialization looking for devices with class code `0x0C` (Serial Bus Controller) and subclass `0x03` (USB). The programming interface byte identifies the controller type:

| prog_if | Controller |
|---------|------------|
| 0x00    | UHCI       |
| 0x10    | OHCI       |
| 0x20    | EHCI       |
| 0x30    | xHCI       |

### Build Configuration

USB components are gated by `ENABLE_USB` and per-controller flags in `fern/build/features/usb.mk`. Setting `ENABLE_USB=no` excludes the entire USB subsystem. Individual controllers and class drivers can also be excluded independently. QEMU support is available via `QEMU_USB=yes`, which appends `-device usb-ehci` to the QEMU options.

---

## UHCI (Universal Host Controller Interface)

**Source:** `fern/src/usb/uhci.c` · **Header:** `fern/src/include/usb/uhci.h`

UHCI is the simplest USB controller standard, designed for USB 1.0/1.1. It was created by Intel and uses an I/O port-based interface with a software-managed frame list.

### Register Layout

UHCI uses 16-bit I/O ports at a base address read from PCI BAR4. Key registers:

| Register | Offset | Size | Purpose |
|----------|--------|------|---------|
| USBCMD   | 0x00   | 16   | Run/Stop, Host Controller Reset |
| USBSTS   | 0x02   | 16   | Interrupt status, halted state |
| USBINTR  | 0x04   | 16   | Interrupt enable bits |
| FRNUM    | 0x06   | 16   | Current frame number |
| FRBASEADD| 0x08   | 32   | Frame list physical address |
| SOFMOD   | 0x0C   | 8    | Start-of-Frame timing |
| PORTSC1  | 0x10   | 16   | Port 1 status/control |
| PORTSC2  | 0x12   | 16   | Port 2 status/control |

### Initialization

`uhci_init()` performs:

1. Allocates `uhci_data_t` (controller-specific state).
2. Disables BIOS legacy support by writing to PCI config register 0xC0.
3. Performs a global reset followed by a host controller reset.
4. Allocates a 1024-entry frame list (4KB aligned).
5. Allocates a pool of 256 Transfer Descriptors (TDs) from a pre-allocated pool.
6. Programs the frame list base address and enables interrupts.
7. Starts the controller by setting the Run bit.
8. Auto-detects the number of ports by probing port status registers.

### Transfer Mechanism

UHCI uses **Transfer Descriptors (TDs)** linked into a frame list. Each TD is 16 bytes and 16-byte aligned. The controller processes one TD per frame (1ms interval).

**Control transfers** are implemented as a three-phase operation:

1. **SETUP stage** — TD with SETUP PID (0x2D), 8-byte setup packet.
2. **DATA stage** — One or more TDs with IN/OUT PID, data toggle alternates.
3. **STATUS stage** — Final TD in the opposite direction of data.

For each phase, a TD is placed into frame list entry 0, polled until the Active bit clears, then freed. The driver checks error bits (Stall, Babble, CRC/Timeout, etc.) to determine transfer success.

**Bulk transfers** follow the same pattern but without the SETUP phase. Data is chunked into max-packet-size segments with alternating data toggles.

**Interrupt transfers** are currently implemented as bulk transfers (a TODO note indicates proper periodic scheduling should be added).

### Speed Detection

UHCI only supports low-speed (1.5 Mbps) and full-speed (12 Mbps) devices. The `LSDA` (Low Speed Device Attached) bit in the port status register determines the speed.

---

## OHCI (Open Host Controller Interface)

**Source:** `fern/src/usb/ohci.c` · **Header:** `fern/src/include/usb/ohci.h`

OHCI is a more hardware-assisted USB 1.0 controller interface. Unlike UHCI, OHCI manages its own scheduling via a Host Controller Communication Area (HCCA) in memory.

### Memory-Mapped Interface

OHCI uses MMIO registers at the base address read from PCI BAR0. The register space is divided into a revision register, control/status registers, and a root hub section.

### HCCA (Host Controller Communication Area)

The HCCA is a 256-byte, 256-byte aligned structure containing:

- **Interrupt table** — 32 Endpoint Descriptor (ED) pointers for periodic interrupt transfers.
- **Frame number** — Current frame number written by hardware.
- **Done head** — Completed Transfer Descriptors (TDs) linked by hardware.

### Endpoint and Transfer Descriptors

OHCI separates endpoint configuration from transfer descriptors:

- **Endpoint Descriptor (ED)** — 16 bytes, 16-byte aligned. Contains device address, endpoint number, direction, speed, max packet size, and head/tail TD pointers.
- **Transfer Descriptor (TD)** — 16 bytes, 16-byte aligned. Contains buffer pointer, transfer length, condition code, and next-TD pointer.

EDs are organized into three lists:

1. **Control list** — For control transfers (head pointer in HcControlHeadED).
2. **Bulk list** — For bulk transfers (head pointer in HcBulkHeadED).
3. **Periodic list** — For interrupt transfers (EDs linked through the interrupt table in the HCCA).

### Initialization

`ohci_init()` performs:

1. Maps MMIO registers.
2. Reads the OHCI revision register (expects 1.0a).
3. Allocates the HCCA (256-byte aligned).
4. Allocates ED and TD pools (16-byte aligned).
5. Takes ownership from SMM (System Management Mode) if needed.
6. Performs a hardware reset (sets HCFS to Reset, issues HCR).
7. Programs the frame interval (default 11999 bit times for 1ms frames).
8. Enables control, bulk, and periodic lists.
9. Powers on root hub ports.
10. Reads port count from HcRhDescriptorA.

### Transfer Execution

For control transfers:

1. Allocate an ED and chain of TDs: setup, data (optional), status, and a dummy TD as the tail.
2. Program the ED with device address, endpoint, max packet size, and speed.
3. Link TDs: setup → data → status → dummy.
4. Write the ED address to HcControlHeadED and set the Control List Filled bit.
5. Poll the HCCA done head until the status TD's condition code is no longer `NOTACCESSED`.

For bulk transfers, the same pattern uses the bulk list (HcBulkHeadED).

### Condition Codes

OHCI reports transfer completion via condition codes in the TD status field:

| Code | Meaning |
|------|---------|
| 0    | No Error |
| 1    | CRC Check Error |
| 2    | Bit Stuffing Error |
| 3    | Data Toggle Mismatch |
| 4    | Stall |
| 5    | Device Not Responding |
| 6-9  | PID/Data errors |
| 14   | Not Accessed (pending) |

---

## EHCI (Enhanced Host Controller Interface)

**Source:** `fern/src/usb/ehci.c` · **Header:** `fern/src/include/usb/ehci.h`

EHCI provides USB 2.0 High-Speed (480 Mbps) support. It introduces two scheduling mechanisms: an asynchronous schedule for control and bulk transfers, and a periodic schedule for interrupt and isochronous transfers.

### Register Layout

EHCI uses a capability/operational register split. The capability registers at BAR0 provide basic parameters, and the operational registers at BAR0 + CAPLENGTH provide control.

**Capability registers:**

| Register    | Offset | Purpose |
|-------------|--------|---------|
| CAPLENGTH   | 0x00   | Operational register offset |
| HCSPARAMS   | 0x04   | Port count, companion controllers |
| HCCPARAMS   | 0x08   | 64-bit capable, extended capabilities |

**Operational registers:**

| Register    | Offset | Purpose |
|-------------|--------|---------|
| USBCMD      | 0x00   | Run/Stop, Schedule enables |
| USBSTS      | 0x04   | Status, Halted |
| USBINTR     | 0x08   | Interrupt enables |
| FRINDEX     | 0x0C   | Frame index |
| PERIODICLISTBASE | 0x14 | Frame list physical address |
| ASYNCLISTADDR | 0x18  | Async schedule head |
| CONFIGFLAG  | 0x40   | Port routing |
| PORTSC+4*n  | 0x44+  | Port status/control |

### Initialization

`ehci_init()` performs:

1. Reads capability registers to determine port count, companion controllers, and extended capabilities.
2. Takes ownership from BIOS via the USB Legacy Support extended capability (EECP).
3. Performs a host controller reset.
4. Allocates a 1024-entry frame list (4KB aligned).
5. Allocates QH and QTD pools (32-byte aligned) using a custom aligned allocation helper.
6. Sets up the async schedule head (a QH pointing to itself, configured as the Head of Reclamation).
7. Initializes the periodic frame list with terminated entries.
8. Programs the frame list base and async list address.
9. Enables periodic and async schedules, then starts the controller.
10. Sets the Configure Flag to route ports to EHCI.
11. Powers on all ports.

### Queue Heads and Transfer Descriptors

EHCI uses **Queue Heads (QHs)** and **Queue Transfer Descriptors (QTDs)**, both 32-byte aligned:

- **QH** (48 bytes) — Contains endpoint characteristics, endpoint capabilities, and a transfer overlay area that mirrors the currently executing QTD.
- **QTD** (32 bytes) — Contains next/alternate TD pointers, token (PID, byte count, status), and up to 5 buffer pointers for page-spanning transfers.

### Scheduling

**Async schedule** (control and bulk):

- A circular linked list of QHs.
- The async head is a special QH with the Head of Reclamation flag, pointing to itself.
- New transfers are inserted into the circular list by updating the horizontal link pointers.
- The controller processes QHs in round-robin fashion.

**Periodic schedule** (interrupt and isochronous):

- A 1024-entry frame list where each entry points to a QH or ITD.
- Interrupt transfers are currently implemented as bulk transfers (TODO: proper periodic scheduling).

### BIOS Ownership Handoff

EHCI implements the BIOS/OS handoff protocol via the USB Legacy Support extended capability. The driver:

1. Reads the legacy support register at the EECP offset in PCI config space.
2. If the BIOS-owned bit is set, writes the OS-owned bit.
3. Polls until the BIOS-owned bit clears (with a timeout).

### Companion Controller

EHCI only handles high-speed devices. Low-speed and full-speed devices are handed to companion controllers (UHCI/OHCI) by setting the Port Owner bit in the port status register.

---

## xHCI (eXtensible Host Controller Interface)

**Source:** `fern/src/usb/xhci.c` · **Header:** `fern/src/include/usb/xhci.h`

xHCI is the modern USB 3.0/3.1 host controller interface, supporting SuperSpeed (5 Gbps) and SuperSpeed+ (10 Gbps). It uses a ring-based architecture instead of linked lists.

### Register Layout

xHCI has four register spaces:

1. **Capability registers** (BAR0) — Controller parameters.
2. **Operational registers** (BAR0 + CAPLENGTH) — Controller control.
3. **Runtime registers** (BAR0 + RTSOFF) — Interrupter management.
4. **Doorbell registers** (BAR0 + DBOFF) — Per-slot doorbells for ringing the host controller.

### Memory Structures

xHCI requires several memory-mapped data structures:

- **DCBAA** (Device Context Base Address Array) — 64-byte aligned array of pointers to device contexts.
- **Command Ring** — Circular ring of Transfer Request Blocks (TRBs) for host commands.
- **Event Ring** — Circular ring of TRBs for host events (command completion, transfer events, port changes).
- **ERST** (Event Ring Segment Table) — Describes event ring segments.
- **Transfer Rings** — Per-endpoint rings for data transfers.
- **Device Contexts** — Per-device structures containing slot and endpoint contexts.
- **Scratchpad Buffers** — Optional buffers for controller use (allocated if required by HCSPARAMS2).

### Initialization

`xhci_init()` performs:

1. Reads capability registers for max slots, ports, interrupters, context size, and page size.
2. Performs a host controller reset (stop, wait for halt, set HCRST, wait for clear).
3. Sets the number of enabled device slots.
4. Allocates the DCBAA (64-byte aligned).
5. Allocates scratchpad buffers if required.
6. Allocates command and event rings with link TRBs.
7. Sets up the Event Ring Segment Table and interrupter 0.
8. Starts the controller and enables interrupts.

### Device Addressing

xHCI uses a slot-based model instead of simple USB addresses:

1. **Enable Slot** — Send an Enable Slot command to allocate a device slot.
2. **Address Device** — Build an input context with slot and EP0 endpoint contexts, then send an Address Device command. The DCBAA[slot] is updated with the output device context.
3. **Configure Endpoint** — Send a Configure Endpoint command to enable additional endpoints.

### Transfer Rings

Each endpoint gets its own transfer ring. Transfers are described by TRBs:

- **Normal TRB** — Bulk and interrupt transfers.
- **Setup Stage TRB** — Control setup phase.
- **Data Stage TRB** — Control data phase.
- **Status Stage TRB** — Control status phase.
- **Link TRB** — Wraps around the ring.

TRBs are 16 bytes and 16-byte aligned. The ring uses a cycle bit to distinguish new TRBs from old ones.

### Command and Event Processing

Commands are submitted by writing a TRB to the command ring and ringing doorbell 0. The driver polls the event ring for command completion events.

Events are consumed by checking the cycle bit, then dispatching based on the TRB type (Transfer Event, Command Completion, Port Status Change, etc.).

### Port Reset

xHCI supports both standard reset (USB 2.0) and warm reset (USB 3.0). The driver detects the port speed from the port status register and issues the appropriate reset type.

---

## USB HID (Human Interface Device)

**Source:** `fern/src/usb/usb_hid.c` · **Header:** `fern/src/include/usb/usb_hid.h`

The HID driver supports USB keyboards and mice using the boot protocol. It translates USB HID scancodes into Linux input event codes and integrates with the input event subsystem.

### Driver Registration

The HID class driver registers with class code `0x03` (HID) and matches all subclasses and protocols. During probe, it detects the device type from the interface descriptor's subclass (Boot) and protocol (Keyboard=0x01, Mouse=0x02).

### Keyboard Support

**Boot protocol report** (8 bytes):

| Offset | Field | Description |
|--------|-------|-------------|
| 0      | modifiers | Modifier key bitmap (Ctrl, Shift, Alt, GUI) |
| 1      | reserved | Always 0 |
| 2-7    | keycodes | Up to 6 simultaneous keycodes |

The driver maintains a `keys_pressed[256]` array to track key state across reports. When a keycode appears in the current report but not the previous, it's a key press. When a keycode disappears, it's a release.

**Modifier handling:**

- The driver emits separate modifier change events when the modifier byte changes between reports.
- Modifiers are mapped to Linux keycodes (e.g., Left Ctrl = 29, Left Shift = 42).

**LED control:**

- Caps Lock, Num Lock, and Scroll Lock toggle LEDs via `SET_REPORT`.
- LED state is maintained per-device and sent to the keyboard when toggled.

**Scancode conversion:**

- USB HID usage codes (0x04-0x65) are mapped to Linux keycodes via the `hid_usage_to_linux` table.
- ASCII conversion uses `scancode_to_ascii_lower/upper` tables, accounting for Shift and Caps Lock.

### Mouse Support

**Boot protocol report** (3+ bytes):

| Offset | Field | Description |
|--------|-------|-------------|
| 0      | buttons | Left, Right, Middle button bits |
| 1      | x | X movement (-127 to 127) |
| 2      | y | Y movement (-127 to 127) |
| 3      | wheel | Scroll wheel (IntelliMouse extension) |

The driver maintains absolute mouse position clamped to screen bounds (configurable via `usb_hid_mouse_set_bounds`). Movement deltas are computed and dispatched as `EV_REL` events.

### Input Event Integration

Events are dispatched through two paths:

1. **devfs** — `devfs_kbd_queue_event` and `devfs_mouse_queue_event` for userspace consumption via `/dev/input`.
2. **input_mux** — `input_mux_dispatch_event` for kernel-level input routing.
3. **Hotkey system** — F1-F12 keys are forwarded to `hotkey_process_key_event` for media keys and system shortcuts.

### Polling

`usb_hid_poll()` is called from the USB core's `usb_poll()` function. For each registered HID device, it performs an interrupt transfer on the interrupt IN endpoint and processes the report.

---

## USB Mass Storage

**Source:** `fern/src/usb/usb_mass_storage.c` · **Header:** `fern/src/include/usb/usb_mass_storage.h`

The Mass Storage driver implements the USB Mass Storage Class Bulk-Only (BBB) Transport protocol, supporting flash drives, external HDDs, and optical drives.

### BBB Protocol

The Bulk-Only Transport uses three phases:

1. **Command Phase** — Host sends a 31-byte Command Block Wrapper (CBW) containing the SCSI command.
2. **Data Phase** — Optional data transfer via bulk IN or OUT endpoint.
3. **Status Phase** — Host reads a 13-byte Command Status Wrapper (CSW) indicating success/failure.

### CBW and CSW Structures

**Command Block Wrapper (31 bytes):**

| Field | Size | Description |
|-------|------|-------------|
| Signature | 4 | Magic: 0x43425355 ("USBC") |
| Tag | 4 | Transaction tag (matched in CSW) |
| DataTransferLength | 4 | Bytes to transfer in data phase |
| Flags | 1 | Direction (bit 7: 0=OUT, 1=IN) |
| LUN | 1 | Logical Unit Number (bits 0-3) |
| CBLength | 1 | Command block length (1-16) |
| CB | 16 | SCSI command block |

**Command Status Wrapper (13 bytes):**

| Field | Size | Description |
|-------|------|-------------|
| Signature | 4 | Magic: 0x53425355 ("USBS") |
| Tag | 4 | Must match CBW tag |
| DataResidue | 4 | Difference between expected and actual transfer |
| Status | 1 | 0=Passed, 1=Failed, 2=Phase Error |

### SCSI Command Set

The driver translates high-level block I/O operations into SCSI commands sent via the BBB protocol:

| Operation | SCSI Command |
|-----------|--------------|
| Test Unit Ready | TEST UNIT READY (0x00) |
| Inquiry | INQUIRY (0x12) |
| Read Capacity (10) | READ CAPACITY(10) (0x25) |
| Read Capacity (16) | SERVICE ACTION IN(16) (0x9E) |
| Read Blocks (10) | READ(10) (0x28) |
| Read Blocks (16) | READ(16) (0x88) |
| Write Blocks (10) | WRITE(10) (0x2A) |
| Write Blocks (16) | WRITE(16) (0x8A) |
| Request Sense | REQUEST SENSE (0x03) |
| Synchronize Cache | SYNCHRONIZE CACHE(10) (0x35) |

### Device Initialization

`usb_msc_configure_device()` probes each LUN (up to `max_lun`):

1. Issue TEST UNIT READY (retry up to 3 times, with REQUEST SENSE between retries).
2. Issue INQUIRY to get vendor, product, and device type.
3. Wait for the device to become ready.
4. Issue READ CAPACITY to determine total blocks and block size.
5. For drives reporting 0xFFFFFFFF last LBA, fall back to READ CAPACITY(16).

### LUN Support

The driver supports up to 16 LUNs per device. Each LUN is tracked independently with its own capacity, block size, vendor/product strings, and present/removable status.

### Error Recovery

If a CSW indicates a phase error or the transfer fails, the driver performs a Bulk-Only Mass Storage Reset Recovery:

1. Issue Bulk-Only Mass Storage Reset (class request 0xFF).
2. Clear HALT on the bulk IN endpoint.
3. Clear HALT on the bulk OUT endpoint.

---

## USB Device Enumeration

Device enumeration is handled by `usb_enumerate_device()` in `usb_core.c`. The process:

1. **Read device descriptor (8 bytes)** — Gets the max packet size for endpoint 0.
2. **Set address** — Assigns a unique address (1-127) via `SET_ADDRESS`.
3. **Read full device descriptor** — Gets vendor ID, product ID, class, subclass, protocol, and configuration count.
4. **Read configuration descriptors** — Reads the first configuration, including all interface and endpoint descriptors.
5. **Set configuration** — Activates the first configuration via `SET_CONFIGURATION`.
6. **Match class drivers** — Iterates registered class drivers, matching on class/subclass/protocol. The first matching driver's `probe()` function is called.

### Descriptor Parsing

Descriptor types are defined as:

| Type | Value | Description |
|------|-------|-------------|
| Device | 0x01 | Device descriptor (18 bytes) |
| Configuration | 0x02 | Configuration descriptor (9 bytes header + interfaces) |
| String | 0x03 | String descriptor (variable length UTF-16) |
| Interface | 0x04 | Interface descriptor (9 bytes) |
| Endpoint | 0x05 | Endpoint descriptor (7 bytes) |
| HID | 0x21 | HID descriptor |
| Report | 0x22 | HID Report descriptor |

### Setup Packet

All control transfers use a standard 8-byte setup packet:

| Field | Size | Description |
|-------|------|-------------|
| bmRequestType | 1 | Direction, type, recipient |
| bRequest | 1 | Request code |
| wValue | 2 | Request-specific value |
| wIndex | 2 | Request-specific index |
| wLength | 2 | Data transfer length |

---

## Transfer Types

### Control Transfers

Used for device configuration and standard requests. Always on endpoint 0. Three phases: SETUP, DATA (optional), STATUS. All HCDs implement `control_transfer()`.

### Bulk Transfers

Used for large, non-time-critical data (e.g., mass storage). No guaranteed bandwidth. Transfers are chunked into max-packet-size segments. HCDs implement `bulk_transfer()`.

### Interrupt Transfers

Used for periodic, low-latency data (e.g., HID devices). The HID driver polls interrupt IN endpoints periodically. In UHCI/OHCI/EHCI, interrupt transfers are currently implemented as bulk transfers (with TODO notes for proper periodic scheduling). xHCI handles them similarly to bulk.

### Isochronous Transfers

Used for streaming data (e.g., audio, video). Guarantees bandwidth but no error correction. Currently not implemented in the driver (marked as TODO).

---

## Host Controller Management

### Controller Linked List

Controllers are stored in a singly-linked list (`g_controllers`). Each controller's `next` pointer links to the next controller. The USB core traverses this list during `usb_poll()` and `usb_shutdown()`.

### MMIO Mapping

For MMIO-based controllers (OHCI, EHCI, xHCI), the base address is identity-mapped into both the kernel page directory and the active page directory. During `usb_poll()`, the driver ensures MMIO mappings are present in the active page directory, handling task switches correctly.

### Pointer Validation

The USB core validates controller pointers before operations to detect corruption:

- Checks that the pointer falls within the kernel heap range.
- Verifies alignment.
- Validates the controller type enum.
- Caps port count at 64.

If corruption is detected, the poll loop is quarantined to prevent cascading failures.

### Ops Repair

If a controller's ops pointer becomes corrupted or points to the wrong type, the core repairs it by looking up the correct ops based on the controller type. This is logged and counted.

---

## Root Hub Management

Each host controller has an integrated root hub with one or more ports. Port operations are delegated to the HCD:

- **Reset port** — Asserts reset signal for 10-50ms, waits for completion.
- **Enable/Disable port** — Controls port power and enable state.
- **Get port speed** — Returns Low/Full/High/Super speed based on line status.
- **Port connected** — Checks current connect status bit.

### EHCI Companion Controller Handoff

EHCI only handles high-speed devices. When a port reset reveals a low-speed or full-speed device, the Port Owner bit is set to hand the port to the companion UHCI/OHCI controller.

### xHCI Warm Reset

xHCI distinguishes between USB 2.0 standard reset and USB 3.0 warm reset. The driver reads the port speed to determine which reset type to issue.

---

## Hot-Plug Support

Hot-plug is handled through the USB core's polling mechanism:

### Polling

`usb_poll()` is called periodically (from the timer IRQ context). It:

1. Validates all controller pointers and MMIO mappings.
2. Calls each controller's `poll()` function.
3. Calls `usb_hid_poll()` to read input reports.

### Port Change Detection

Each HCD's `poll()` function checks for port status changes:

- **UHCI** — Checks Connect Status Change (CSC) bit in port status register.
- **OHCI** — Checks Root Hub Status Change interrupt and per-port CSC.
- **EHCI** — Checks Port Change Detect (PCD) status bit and per-port CSC.
- **xHCI** — Processes Port Status Change events from the event ring.

When a device is detected:

1. Port status change bits are cleared (write-1-to-clear).
2. The driver logs the connection event.
3. The USB core's enumeration flow is triggered.

### Device Disconnect

Disconnect detection follows the same path — the CSC bit clears when a device is removed. The driver logs the event. Full disconnect cleanup (class driver disconnect, resource freeing) is a work in progress.

### Timer-Context Safety

Since `usb_poll()` runs from timer IRQ context, the driver takes care to:

- Sync kernel page directory entries to the active task's page directory.
- Validate all pointers before dereferencing.
- Quarantine the USB subsystem if corruption is detected, preventing kernel crashes.

---

## Summary of File Locations

| Component | Source | Header |
|-----------|--------|--------|
| USB Core | `fern/src/usb/usb_core.c` | `fern/src/include/usb/usb.h` |
| UHCI | `fern/src/usb/uhci.c` | `fern/src/include/usb/uhci.h` |
| OHCI | `fern/src/usb/ohci.c` | `fern/src/include/usb/ohci.h` |
| EHCI | `fern/src/usb/ehci.c` | `fern/src/include/usb/ehci.h` |
| xHCI | `fern/src/usb/xhci.c` | `fern/src/include/usb/xhci.h` |
| HID | `fern/src/usb/usb_hid.c` | `fern/src/include/usb/usb_hid.h` |
| Mass Storage | `fern/src/usb/usb_mass_storage.c` | `fern/src/include/usb/usb_mass_storage.h` |
| Hub | — | `fern/src/include/usb/usb_hub.h` |
| Build | `fern/build/features/usb.mk` | — |
