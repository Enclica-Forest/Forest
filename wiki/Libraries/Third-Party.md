# Third-Party Libraries

Forest OS pulls in a handful of well-chosen third-party libraries to avoid
reinventing wheels that are already spinning perfectly. This page covers the
two libraries that live under `libs/` as third-party subtrees: **uACPI** and
**qrcodegen**.

---

## uACPI — ACPI Implementation

### What It Is

[uACPI](https://github.com/UltraOS/uACPI) is a portable, high-performance
implementation of the Advanced Configuration and Power Interface (ACPI)
specification. It provides a complete AML (ACPI Machine Language) interpreter,
namespace management, event subsystem, operation regions, sleep state
management, and device enumeration — all in a library designed from the ground
up to run inside a kernel.

### Why Forest Uses It

Forest originally had a hand-rolled ACPI table parser (`acpi.c`) that could
find the RSDP, parse the MADT, and handle a few essentials. That's fine for
boot, but real hardware ACPI is vastly more complex — firmware-written AML blobs
describe power management, PCI routing, thermal zones, battery status, and
dozens of other subsystems. Writing a full AML interpreter from scratch would be
a massive undertaking with little payoff when uACPI already exists.

uACPI was chosen because:

- **Performance** — roughly 3.5x faster than ACPICA in synthetic benchmarks,
  and 1.75–2x faster on real hardware.
- **NT compatibility** — built to match Windows AML semantics natively, without
  the accumulated workarounds that ACPICA carries.
- **Safety** — assumes the worst about AML bytecode, with careful object
  lifetime tracking and extensive fuzzing.
- **No recursion** — the AML interpreter is fully non-recursive, critical for
  kernels with tiny stacks.
- **Small footprint** — ships with its own overridable stdlib, making it easy
  to port.
- **MIT licensed** — no legal headaches.

### How It Integrates with the Kernel

Forest includes uACPI as a subtree under `libs/uacpi/`. The library's CMake
file (`uacpi.cmake`) exports `UACPI_SOURCES` and `UACPI_INCLUDES` variables
that the Forest build system consumes. All source files from `source/` are
compiled directly into the kernel.

The kernel communicates with uACPI through the **kernel API bridge** implemented
in `fern/src/uacpi_port.c`. This file provides every function declared in
`uacpi/kernel_api.h`, translating them to Forest-specific subsystems:

| uACPI API                    | Forest Implementation                     |
|------------------------------|-------------------------------------------|
| `uacpi_kernel_get_rsdp`     | Calls `acpi_find_rsdp()` from `acpi.c`    |
| `uacpi_kernel_map/unmap`    | Identity-maps via VMM (`vmm_identity_map_range`) |
| `uacpi_kernel_log`          | Routes to `debuglog_write()`              |
| `uacpi_kernel_pci_*`        | Delegates to `pci_config_read/write*()`   |
| `uacpi_kernel_io_*`         | Uses `inportb/w/d` and `outportb/w/d`     |
| `uacpi_kernel_alloc/free`   | Wraps `kmalloc`/`kfree`                   |
| `uacpi_kernel_stall/sleep`  | Uses `sleep_busy()` and `sleep_interruptible()` |
| `uacpi_kernel_create_*`     | Wraps Forest spinlocks, mutexes, semaphores |
| `uacpi_kernel_schedule_work`| Calls the handler synchronously (minimal stub) |

Some features like firmware request handling and full IRQ routing are stubbed
out for now — they'll be wired up as the kernel's power management matures.

### ACPI Table Parsing

uACPI handles table discovery automatically during `uacpi_initialize()`:

1. Scans for the RSDP (Root System Description Pointer) via the kernel API.
2. Parses the RSDT or XSDT to find all registered ACPI tables.
3. Validates checksums and records table addresses for later use.

Forest's own `acpi.c` still runs first during early boot to find the MADT
(Multiple APIC Description Table) and FADT (Fixed ACPI Description Table)
before uACPI is initialized. This gives the kernel the information it needs
to set up the interrupt controller and identify CPUs without waiting for the
full namespace to load.

### Device Enumeration

After `uacpi_namespace_load()` parses all DSDT/SSDT tables, the call to
`uacpi_namespace_initialize()` walks the ACPI namespace and evaluates `_STA`
(Status) and `_INI` (Initialize) methods on each device node. This lets
uACPI determine which devices are present, enabled, and ready for use.

The kernel can then search for specific devices using:

```c
uacpi_namespace_node *node;
uacpi_status st = uacpi_namespace_find(
    uacpi_namespace_root(), "\\_SB.PCI0", &node
);
```

Or iterate over all children of a parent node with
`uacpi_namespace_for_each_child_simple()` or the more advanced
`uacpi_namespace_for_each_child()` which supports type filtering and depth
limits.

### Namespace Traversal

The ACPI namespace is a tree of named objects (devices, methods, buffers,
etc.) rooted at `\`. uACPI provides several ways to walk it:

- **Simple iteration** — `uacpi_namespace_for_each_child_simple()` does a
  depth-first walk, calling a single callback per node.
- **Full walk** — `uacpi_namespace_for_each_child()` accepts descending and
  ascending callbacks, a type mask to filter nodes, and a max depth.
- **Peer iteration** — `uacpi_namespace_node_next()` returns the next sibling,
  useful for building custom iterators without recursion.
- **Node lookup** — `uacpi_namespace_node_find()` resolves a path relative to
  a parent, and `uacpi_namespace_node_resolve_from_aml_namepath()` handles
  AML-provided name paths with upward resolution.

Each node can be queried for its name, type, depth, and parent via helper
functions like `uacpi_namespace_node_name()`, `uacpi_namespace_node_type()`,
and `uacpi_namespace_node_depth()`.

### Evaluation API

Once you have a namespace node, you can evaluate methods or read/write objects
using the family of `uacpi_eval*` functions:

```c
// Simple evaluation
uacpi_object *ret;
uacpi_eval_simple_integer(node, "_CRS", &ret);

// Typed evaluation with arguments
uacpi_object_array args = { .count = 1, .objects = &arg };
uacpi_eval_typed(parent, "METHOD", &args,
                 UACPI_OBJECT_INTEGER_BIT, &ret);
```

Convenience wrappers exist for integers, strings, buffers, and packages.

---

## qrcodegen — QR Code Generation

### What It Is

[qrcodegen](https://github.com/nayuki/QR-Code-generator) is a compact,
well-tested QR Code generation library by Project Nayuki. It supports all 40 QR
Code versions (1 through 40), four error correction levels, and multiple
encoding modes (numeric, alphanumeric, byte, kanji). The library is a single
`.c`/`.h` pair with no external dependencies beyond the C standard library.

### How Forest Uses It

Forest uses qrcodegen in the **kernel panic screen** (`fern/src/panic.c`). When
the kernel panics, it generates a QR code containing a diagnostic token derived
from a hash of the panic context (error code, instruction pointer, faulting
address, etc.). This QR code is drawn directly to the framebuffer so that users
can scan it with a phone to get a machine-readable error token for support or
bug reporting.

The relevant code from `panic.c`:

```c
static uint8_t qr_temp_buffer[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static uint8_t qr_code_buffer[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];

// Generate a QR code from a diagnostic token string
char token[20] = "TOKEN:00000000";
// ... fill token with hex hash ...
qrcodegen_encodeText(token, qr_temp_buffer, qr_code_buffer,
                     qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true);

// Read modules and draw to framebuffer
int qr_size = qrcodegen_getSize(qr_code_buffer);
for (int my = 0; my < qr_size; my++) {
    for (int mx = 0; mx < qr_size; mx++) {
        int on = qrcodegen_getModule(qr_code_buffer, mx, my);
        // Draw pixel based on module state
    }
}
```

### API Overview

The library exposes a clean, minimal API:

#### Encoding Functions

| Function | Description |
|----------|-------------|
| `qrcodegen_encodeText()` | Encode a text string, auto-detecting the best mode |
| `qrcodegen_encodeSegments()` | Encode pre-built segments |
| `qrcodegen_encodeSegmentsAdvanced()` | Full control over encoding parameters |
| `qrcodegen_generateBytes()` | Encode raw byte data |

#### Module Access

| Function | Description |
|----------|-------------|
| `qrcodegen_getSize()` | Get the module count (width/height) of the QR code |
| `qrcodegen_getModule()` | Read a single module (dark/light) |
| `qrcodegen_setModule()` | Set a single module |
| `qrcodegen_clearScreen()` | Reset a QR code buffer |

#### Segment Builders

| Function | Description |
|----------|-------------|
| `qrcodegen_makeNumeric()` | Create a numeric segment |
| `qrcodegen_makeAlphanumeric()` | Create an alphanumeric segment |
| `qrcodegen_makeBytes()` | Create a byte segment |
| `qrcodegen_makeKanji()` | Create a kanji segment |

#### Content Detection

| Function | Description |
|----------|-------------|
| `qrcodegen_isNumeric()` | Check if text is numeric-only |
| `qrcodegen_isAlphanumeric()` | Check if text is alphanumeric |
| `qrcodegen_isKanji()` | Check if data is kanji |

### Buffer Sizes

The library requires two buffers:

- **QR code buffer** — holds the encoded QR code matrix. Size is calculated by
  the `qrcodegen_BUFFER_LEN_FOR_VERSION(version)` macro.
- **Temporary buffer** — used during encoding. Same size requirement.

For version 10 (the maximum used in Forest's panic screen), each buffer needs
1,777 bytes:

```c
#define qrcodegen_BUFFER_LEN_FOR_VERSION(10)  // = 1777
```

The absolute maximum for version 40 is about 6,593 bytes.

### Usage Example

A minimal example for encoding a URL:

```c
#include "qrcodegen.h"

uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];

bool ok = qrcodegen_encodeText(
    "https://forest-os.dev/report",
    tmp, qr,
    qrcodegen_Ecc_LOW,    // error correction level
    1, 10,                // min/max version
    qrcodegen_Mask_AUTO,  // auto-select mask
    true                  // boost error correction
);

if (ok) {
    int size = qrcodegen_getSize(qr);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            printf(qrcodegen_getModule(qr, x, y) ? "##" : "  ");
        }
        printf("\n");
    }
}
```

---

## How Third-Party Libraries Are Vendored

Third-party libraries in Forest are managed as **git subtrees** (or manually
copied sources for very small libraries). They live directly under `libs/` and
follow the same directory conventions as first-party libraries:

```
libs/
├── uacpi/           # Full subtree from upstream
│   ├── include/     # Public headers
│   ├── source/      # Implementation files
│   ├── LICENSE      # Upstream license
│   ├── README.md    # Upstream documentation
│   └── uacpi.cmake  # Build integration
├── qrcodegen/       # Single-file library, manually vendored
│   ├── qrcodegen.h
│   └── qrcodegen.c
```

The `libs/README.md` documents each library and its purpose. Third-party
libraries are expected to follow the same layout (`include/`, `src/`, tests,
etc.) to keep the toolchain configuration straightforward.

### Build Integration

- **uACPI** uses CMake via `uacpi.cmake`, which exports `UACPI_SOURCES` and
  `UACPI_INCLUDES`. The Forest Makefile includes these variables when building
  with ACPI support enabled.
- **qrcodegen** is compiled directly — its `.c` file is added to the kernel
  sources via the Makefile when userspace or panic-screen features require it.

---

## Update Policy

Third-party libraries are updated **conservatively and intentionally**:

- **No automatic updates.** Libraries are pinned to known-good versions and
  only updated when there's a specific reason — a bug fix, security patch, or
  new feature that Forest needs.
- **Test before merging.** Any update goes through the same build and boot
  testing as any other kernel change. A regression in a third-party library can
  be just as devastating as one in first-party code.
- **Document the change.** When updating a library, the commit message should
  note the old and new versions, what changed, and why the update was needed.
- **Maintain compatibility.** If an upstream update changes its API or build
  integration, the Forest glue code (like `uacpi_port.c`) must be updated in
  the same commit.

For uACPI specifically, Forest tracks the upstream repository and pulls in
releases that have been validated on real hardware. The current version is
**v3.2**.

---

## License Considerations

Both third-party libraries are MIT-licensed, which is the ideal situation for
vendoring into a project:

| Library | License | Copyright |
|---------|---------|-----------|
| uACPI   | MIT     | 2022–2025 Daniil Tatianin |
| qrcodegen | MIT  | Project Nayuki |

The MIT license permits use, modification, and redistribution without
attribution requirements in binaries (though Forest's `libs/README.md` and
individual library directories retain the original LICENSE files and copyright
notices as a matter of good practice).

Forest OS itself is also MIT-licensed, so there are no license compatibility
issues. If a future third-party library were GPL-licensed, it would need to be
isolated in a separate userspace process to avoid copyleft contamination of the
kernel.

### What This Means for Contributors

- You **can** copy, modify, and distribute code from these libraries without
  restriction.
- You **should** keep the original LICENSE files in place when updating or
  vendoring new libraries.
- You **must not** remove copyright notices from upstream sources.
- If you vendor a new library with a different license, add it to this page
  and `libs/README.md` so everyone is aware.
