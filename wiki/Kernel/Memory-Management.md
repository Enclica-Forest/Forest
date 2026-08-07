# Memory Management

Forest OS implements a Linux-inspired memory management system across multiple architecture layers. This page covers the physical allocator, virtual memory, slab and buddy allocators, copy-on-write, swap, page cache, the kernel allocation API, userspace memory management, memory protection, page fault handling, and the overall memory layout.

## Table of Contents

- [Overview](#overview)
- [Physical Memory Management](#physical-memory-management)
- [Buddy Allocator](#buddy-allocator)
- [Virtual Memory Management](#virtual-memory-management)
- [Slab Allocator](#slab-allocator)
- [Copy-on-Write (COW)](#copy-on-write-cow)
- [Swap Space](#swap-space)
- [Page Cache](#page-cache)
- [Kernel Allocation API](#kernel-allocation-api)
- [Userspace Memory Management](#userspace-memory-management)
- [Memory Protection and Isolation](#memory-protection-and-isolation)
- [Page Fault Handling](#page-fault-handling)
- [Memory Layout](#memory-layout)

---

## Overview

Forest OS organizes memory management into distinct layers, each building on the one below:

```
┌─────────────────────────────────────────────────┐
│            Userspace (mmap, brk, etc.)          │
├─────────────────────────────────────────────────┤
│   Page Cache  │  COW  │  Swap  │  Reclaim      │
├─────────────────────────────────────────────────┤
│          SLAB Allocator (object caches)         │
├─────────────────────────────────────────────────┤
│       Buddy Allocator (page-level allocation)   │
├─────────────────────────────────────────────────┤
│     Bitmap PMM (physical frame tracking)        │
├─────────────────────────────────────────────────┤
│         Hardware (RAM, MMU, TLB)                │
└─────────────────────────────────────────────────┘
```

The initialization order in `mm_init()` (`src/mm_init.c:74`) is:

1. Memory zones (E820/multiboot parsing)
2. Buddy allocator (physical page management)
3. SLAB allocator (small object caching)
4. VMA system (per-process virtual areas)
5. Page cache (file-backed page caching)
6. Reclaim system (LRU-based eviction)
7. Page fault handler (demand paging)
8. OOM killer and swap (last-resort and overflow)

Each subsystem can be independently gated at build time via `ENABLE_*` flags in `build/features/memory.mk`.

---

## Physical Memory Management

Forest OS uses **two** physical memory managers that serve different roles:

### Bitmap PMM (`src/bitmap_pmm.c`)

The bitmap PMM is the earliest-stage allocator. It tracks every 4KB page frame in a flat bitmap using a `uint32_t[PMM_BITMAP_SIZE]` array. Each bit represents one page frame: 0 = free, 1 = used.

**Key characteristics:**

- Supports up to 4GB of physical memory (`PMM_MAX_PAGES = 1,048,576`)
- Pages are grouped in zones: **DMA** (below 16MB) and **Normal** (above 16MB)
- Includes corruption detection via checksumming the bitmap and verifying magic header/footer values
- Allocation preferences: `PMM_ALLOC_LOW_MEMORY` (DMA), `PMM_ALLOC_HIGH_MEMORY`, or `PMM_ALLOC_ANY_MEMORY`
- Provides both single-page and multi-page allocation with optional contiguity and alignment requirements

**Allocation flow:**

```
bitmap_pmm_alloc_page()
  → find_free_page(start_hint, max_pages)
      → scan bitmap words for a non-0xFFFFFFFF entry
          → scan individual bits for a clear bit
  → bitmap_set_bit(page_frame)     // mark as used
  → update statistics & checksum
```

For multi-page allocations, `find_contiguous_pages()` performs a linear scan looking for `count` consecutive free bits. If contiguous allocation fails, it falls back to individual allocations.

**The PMM also provides:**

- `bitmap_pmm_scrub_free_frames()` — zero all free frames at boot (security)
- `bitmap_pmm_analyze_fragmentation()` — count free blocks and largest contiguous run
- `bitmap_pmm_validate_bitmap_integrity()` — verify checksum hasn't drifted

### Cross-Architecture PMM (`src/arch/pmm.h`)

A separate abstraction (`pmm_alloc_frame()`, `pmm_alloc_frames()`, etc.) provides a hardware-independent interface. The bitmap PMM feeds into this. The arch PMM initializes from either a flat RAM range (`pmm_init()`) or a firmware-provided memory map (`pmm_init_from_memory_map()`), supporting Multiboot (BIOS), UEFI, and Device Tree (ARM/RISC-V) sources.

---

## Buddy Allocator

The buddy allocator (`src/mm_buddy.c`) manages physical page frames with O(log n) allocation and deallocation through block splitting and coalescing. It supports orders 0–11 (4KB to 8MB).

### Structure

Each memory zone contains an array of `free_area_t` structures, one per order. Each `free_area_t` holds a linked list of free blocks of that size.

```
Zone (e.g. ZONE_NORMAL, 16MB–end)
├── free_area[0]   → list of single 4KB pages
├── free_area[1]   → list of 8KB (2-page) blocks
├── free_area[2]   → list of 16KB (4-page) blocks
├── ...
└── free_area[11]  → list of 8MB (2048-page) blocks
```

### Allocation (`alloc_pages`)

1. Find the smallest order with a free block (or higher if needed).
2. Remove the block from the free list.
3. **Split** the block repeatedly until the desired order is reached. At each split, the "buddy" half is added back to the free list at the lower order.
4. Return the page descriptor.

```
alloc_pages(GFP_KERNEL, order=1)    // want 8KB (2 pages)

  free_area[3] has a 32KB block
  → split into two 16KB blocks, buddy goes to free_area[2]
  → split one 16KB into two 8KB blocks, buddy goes to free_area[1]
  → return first 8KB block from free_area[1]
```

### Deallocation (`__free_pages`)

1. Return the block to its free list at the given order.
2. **Coalesce** with its buddy if the buddy is also free and at the same order. The merged block moves up one order and the process repeats.
3. Continue until coalescing is no longer possible or `BUDDY_MAX_ORDER` is reached.

The buddy is found via XOR: `buddy_pfn = pfn ^ (1UL << order)`.

### Zones

The buddy allocator divides physical memory into zones:

| Zone | Range | Purpose |
|------|-------|---------|
| `ZONE_DMA` | 0–16MB | Legacy ISA DMA devices |
| `ZONE_NORMAL` | 16MB–end | General-purpose allocations |

Zone watermarks (`pages_min`, `pages_low`, `pages_high`) set thresholds that trigger background reclaim when free pages drop below the low watermark.

### Page Descriptors

Every physical page frame has a `page_t` descriptor in the global `mem_map` array. This structure holds:

- `flags` — PG_LOCKED, PG_DIRTY, PG_LRU, PG_ACTIVE, PG_SLAB, etc.
- `refcount` — atomic reference count
- A union for buddy order, SLAB cache pointer, or page cache mapping
- `virtual` — virtual address (identity-mapped for now)

Conversion helpers: `pfn_to_page(pfn)`, `page_to_pfn(page)`, `page_address(page)`.

---

## Virtual Memory Management

### 32-bit Paging (`src/vmm.c`, `src/include/memory.h`)

Forest OS uses x86 two-level paging in 32-bit mode:

- **Page Directory** (1024 entries) → each entry points to a Page Table
- **Page Table** (1024 entries) → each entry maps one 4KB page

Each `page_entry_t` is a packed 32-bit structure matching the x86 hardware format:

```c
typedef struct {
    uint32_t present     : 1;
    uint32_t writable    : 1;
    uint32_t user        : 1;
    uint32_t pwt         : 1;  // write-through
    uint32_t pcd         : 1;  // cache disable
    uint32_t accessed    : 1;
    uint32_t dirty       : 1;
    uint32_t pat         : 1;  // page attribute table
    uint32_t global      : 1;
    uint32_t avail       : 3;  // OS-available bits
    uint32_t frame       : 20; // physical frame number
} __attribute__((packed)) page_entry_t;
```

Key VMM operations:

- `vmm_map_page(dir, vaddr, paddr, flags)` — create or update a mapping
- `vmm_unmap_page(dir, vaddr)` — remove a mapping
- `vmm_get_physical_addr(dir, vaddr)` — walk page tables to translate
- `vmm_create_page_directory()` — allocate a new address space
- `vmm_switch_page_directory(dir)` — load CR3

The VMM maintains a **temporary mapping window** at `0xD0000000` (4MB, 1024 slots) for accessing physical frames that aren't directly identity-mapped. It uses round-robin slot allocation with `invlpg` flushing.

### 64-bit Paging (`src/paging64.c`)

On x86_64, Forest OS implements 4-level paging:

| Level | Name | Entries | Covers |
|-------|------|---------|--------|
| PML4 | Page Map Level 4 | 512 | 512GB each |
| PDPT | Page Directory Ptr Table | 512 | 1GB each |
| PD | Page Directory | 512 | 2MB each |
| PT | Page Table | 512 | 4KB each |

Supports 4KB, 2MB (huge), and 1GB (huge) page sizes. Recursive mapping uses PML4 entry 510, allowing the kernel to access its own page tables via fixed virtual addresses.

### Multi-Architecture Support

The VMM dispatches to architecture-specific implementations at compile time:

| Architecture | Root table type | Source file |
|-------------|-----------------|-------------|
| x86_64 | `pml4_t*` (uint64_t[512]) | `paging64.c` |
| AArch64 | `pgd_t*` | `aarch64/mmu.c` |
| ARM32 | `arm_l1_table_t*` | `arm32/mmu.c` |
| RISC-V | `sv39_pgd_t*` | `riscv64/mmu.c` |

---

## Slab Allocator

The slab allocator (`src/mm_slab.c`) provides fast, fixed-size object allocation for kernel data structures. It avoids the overhead of full page allocation for small objects.

### Architecture

```
kmem_cache_t
├── name, size, align, flags
├── cpu_cache → per-CPU freelist (up to 16 objects)
├── node
│   ├── slabs_partial → partially-used slabs
│   ├── slabs_full    → completely-used slabs
│   └── slabs_free    → empty slabs
└── ctor/destructor callbacks
```

### Standard Size Caches

On initialization, the slab allocator creates caches for these common sizes:

```
32, 64, 96, 128, 192, 256, 512, 1024, 2048, 4096, 8192 bytes
```

Each is named `kmalloc-<size>` and stored in the global `size_caches[]` array.

### Allocation Flow

```
kmem_cache_alloc(cache, flags)
  1. Check per-CPU cache (fast path — lock, pop from freelist, unlock)
  2. If empty, lock node:
     a. Try slabs_partial (take one object)
     b. Try slabs_free (take one object, move slab to partial)
     c. If both empty, call cache_grow() to allocate a new slab
  3. cache_grow():
     - alloc_pages(GFP_KERNEL, 0) for the slab memory
     - Allocate slab_t descriptor from cache_cache
     - Initialize and add to slabs_partial
```

### Deallocation Flow

```
kmem_cache_free(cache, obj)
  1. Try per-CPU cache (fast path — lock, push to freelist, unlock)
  2. If full, find slab containing the object (page→slab_cache backpointer)
  3. Return object to slab, adjust slab's inuse counter
  4. Move slab between lists as needed (full→partial, partial→free)
  5. If too many free slabs, destroy excess via slab_destroy()
```

### Slab vs. Buddy Integration

For allocations larger than `SLAB_MAX_SIZE` (2048 bytes), `slab_kmalloc()` bypasses the SLAB allocator and directly calls `alloc_pages()` from the buddy allocator.

---

## Copy-on-Write (COW)

COW (`src/mm_cow.c`, `src/mm_cow_impl.c`) enables efficient memory sharing between processes (e.g., after `fork()`) by deferring physical page copies until a write occurs.

### How It Works

1. **fork() sets up COW:** `cow_copy_mm()` creates a new `mm_struct` with copied VMAs. For every writable page in the parent, the PTE is marked **read-only** with the `_PAGE_COW` flag. The page's reference count is incremented.

2. **Write fault triggers copy:** When either process writes to a shared page, the CPU raises a page fault (write-protection violation). The fault handler calls `do_wp_page()`.

3. **Page is copied:** `do_cow_fault()` allocates a new page, copies the old page's content, updates the PTE to point to the new page (writable, dirty), decrements the old page's refcount, and frees the old page if its refcount reaches zero.

```
Parent PTE: [phys=0x5000 | R/O | COW]   →   Child PTE: [phys=0x5000 | R/O | COW]
                                                        │
                                          Child writes to 0x5000
                                                        │
                                          ┌─────────────┘
                                          ▼
                              1. Alloc new page at 0x8000
                              2. memcpy(0x8000, 0x5000, 4096)
                              3. Child PTE → [phys=0x8000 | R/W | Dirty]
                              4. Refcount(0x5000)-- → freed if 0
```

### Special Cases

- **Zero pages:** If the old page is a reserved zero page (`PG_RESERVED`), COW allocates a fresh zero-filled page instead of copying.
- **Single-owner pages:** If `refcount == 1`, no copy is needed — the PTE is simply made writable.

---

## Swap Space

The swap subsystem (`src/mm_swap.c`) allows the system to use disk storage as an extension of physical memory by evicting inactive pages.

### Architecture

```
swap_device_t
├── active flag
├── type (file or partition)
├── backing (file handle / device)
├── bitmap (tracks free swap slots)
├── extents (for non-contiguous swap files)
└── priority (higher = preferred for allocation)
```

- Supports up to **4 swap devices** with priority-based selection
- Maximum swap size: 1GB
- Swap entries encode device index (bits 24–31) and slot offset (bits 0–23)

### LRU Page Replacement

The swap system maintains **active** and **inactive** doubly-linked lists. Pages are added to the active list and promoted/demoted based on access patterns:

```
lru_lists_t
├── active_head / active_tail   (recently accessed)
└── inactive_head / inactive_tail (candidates for eviction)
```

`lru_get_victim()` returns the tail of the inactive list (least recently used). If the inactive list is empty, it falls back to the active list tail.

### Swap Out / Swap In

```
swap_out_page(dir, vaddr)
  1. Resolve physical address via VMM
  2. Select best swap device by priority
  3. Allocate a swap slot (bitmap scan)
  4. Write page contents to swap storage
  5. Unmap page and free physical frame
  6. Return swap entry (encoded offset + device)

swap_in_page(dir, vaddr, entry)
  1. Decode device index and slot from entry
  2. Allocate new physical frame
  3. Read page contents from swap storage
  4. Map new frame at the virtual address
  5. Free the swap slot
```

The actual disk I/O is currently a stub — `swap_write_page()` and `swap_read_page()` return success without performing real I/O. This is a known TODO for when block device integration matures.

---

## Page Cache

The page cache (`src/mm_pagecache.c`) provides a unified layer for caching file-backed pages in memory, avoiding redundant disk reads.

### Hash Table Lookup

Pages are indexed by `(address_space*, pgoff_t)` pairs using a 1024-bucket hash table with per-bucket spinlocks:

```
page_cache_hash[1024]
  bucket[hash(mapping, offset)]
    → linked list of page_t with matching (mapping, index)
```

### Address Spaces

Each file or device has an `address_space` structure tracking its cached pages:

```c
struct address_space {
    clean_pages, dirty_pages, locked_pages;  // page lists
    nr_pages;                                 // count
    ops;           // readpage, writepage callbacks
    readahead;     // read-ahead state machine
};
```

### Read-Ahead

The page cache implements adaptive read-ahead:

- **Sequential access:** window doubles up to 256 pages
- **Random access:** window shrinks to 4 pages
- Default window: 32 pages

`page_cache_sync_readahead()` triggers read-ahead when a page is accessed sequentially past the current window.

### Cache Lifecycle

1. **Insertion:** `page_cache_insert()` adds a page to the hash table and the appropriate clean/dirty list.
2. **Lookup:** `page_cache_lookup()` finds a cached page, increments refcount, and marks it referenced.
3. **Dirtying:** `page_cache_write()` moves a page from the clean to the dirty list.
4. **Removal:** `page_cache_remove()` removes from hash and list, decrements counts.

---

## Kernel Allocation API

### `kmalloc` / `kfree` (`src/arch/kheap.h`, `src/mm_slab.c`)

| Function | Description |
|----------|-------------|
| `kmalloc(size)` | Allocate `size` bytes, aligned to 8 bytes minimum |
| `kzalloc(size)` | Like `kmalloc` but zero-initialized |
| `kmalloc_aligned(size, align)` | Allocate with specific alignment |
| `kfree(ptr)` | Free a previous allocation (NULL-safe) |
| `krealloc(ptr, new_size)` | Resize allocation (NULL-safe, 0 = free) |
| `kheap_get_free()` | Free bytes in kernel heap |
| `kheap_check_pressure(needed)` | Check if `needed` bytes are available |

**Allocation path for `kmalloc(size)`:**

```
kmalloc(size)
  → size_caches[] lookup (if size ≤ 8192)
      → kmem_cache_alloc() (SLAB fast path)
  → else alloc_pages() (buddy allocator)
```

**`kfree(ptr)` path:**

```
kfree(ptr)
  → page = pfn_to_page(ptr >> PAGE_SHIFT)
  → if page->flags & PG_SLAB → kmem_cache_free()
  → else __free_pages() (buddy deallocation)
```

### GFP Flags

| Flag | Meaning |
|------|---------|
| `GFP_KERNEL` | Normal kernel allocation, may sleep |
| `GFP_ATOMIC` | Non-blocking, for interrupt context |
| `GFP_USER` | User-accessible memory |
| `GFP_ZERO` | Zero the allocated pages |
| `GFP_DMA` | Allocate from DMA zone |

### Kernel Heap Layout

The kernel heap is mapped in high memory on 64-bit architectures:

| Architecture | Heap Start | Max Size |
|-------------|------------|----------|
| x86_64 | `0xFFFF800000000000` | 256 MB |
| AArch64 | `0xFFFFFF8000000000` | 256 MB |
| RISC-V 64 | `0xFFFFFFC040000000` | 256 MB |
| x86_32 / ARM32 | `0xC0000000` | 64 MB |

Initial heap size is 16MB on all architectures, growing on demand.

---

## Userspace Memory Management

### Virtual Address Space

Each process has its own `mm_struct` (`src/include/mm.h:340`) containing:

- A linked list and red-black tree of `vm_area_struct_t` regions
- A pointer to the page global directory (`pgd`)
- Accounting fields (`total_vm`, `locked_vm`, `shared_vm`, `exec_vm`, `stack_vm`)
- Segment boundaries (`start_code`, `end_code`, `start_brk`, `brk`, `start_stack`)

### VMA Structure

```c
typedef struct vm_area_struct {
    unsigned long vm_start, vm_end;   // Address range
    unsigned long vm_flags;           // VM_READ, VM_WRITE, VM_EXEC, VM_SHARED, ...
    struct mm_struct *vm_mm;          // Owning process
    pgprot_t vm_page_prot;           // Page protection bits
    struct rb_node vm_rb;             // Red-black tree node
    struct list_head vm_list;         // Linked list node
    struct file *vm_file;            // Backing file (NULL for anonymous)
    const struct vm_operations_struct *vm_ops;  // Custom fault handlers
} vm_area_struct_t;
```

### `mmap` / `do_mmap`

`do_mmap()` (`src/mm_vma.c:371`) creates a new VMA:

1. Find unmapped address range in user space (1GB–3GB on 32-bit)
2. Allocate a `vm_area_struct_t` from the SLAB cache
3. Set flags, page protection, and file mapping
4. Insert into the process's VMA list and red-black tree

### `munmap` / `do_munmap`

`do_munmap()` removes a VMA range:

1. Find and split existing VMAs as needed
2. Unmap all pages in the range
3. Free the VMA structure

### `brk`-Style Heap Growth

The user heap grows via the `brk` VMA, which has `VM_GROWSUP` set. When the program accesses beyond `mm_struct.brk`, the page fault handler expands the VMA and allocates new pages on demand.

### Stack Growth

User stacks use `VM_GROWSDOWN`. When a stack access is below the VMA's `vm_start`, the fault handler expands the VMA downward (up to an 8MB limit) and allocates pages.

---

## Memory Protection and Isolation

### NX (No-Execute) Bit

`src/mem_protect.c` enables the NX bit via `EFER.NXE` in the Extended Feature Enable Register. This allows marking code pages as non-executable and data pages as non-executable, preventing code injection attacks.

### SMEP and SMAP

- **SMEP** (Supervisor Mode Execution Prevention): Prevents the kernel from executing userspace pages. Enabled via CR4 bit 20.
- **SMAP** (Supervisor Mode Access Prevention): Prevents the kernel from accessing userspace data directly. Enabled via CR4 bit 21. Can be temporarily overridden with the `AC` flag.

### Page Protection Bits

Each PTE carries user/kernel and read/write permission bits:

| Bit | Meaning |
|-----|---------|
| `PAGE_PRESENT` (0x001) | Page is mapped |
| `PAGE_WRITABLE` (0x002) | Page can be written |
| `PAGE_USER` (0x004) | Userspace can access |
| `PAGE_CACHE_DISABLE` (0x010) | Bypass CPU cache |
| `PAGE_EXECUTABLE` (0x010) | Page is executable (arch-dependent) |

### PAT (Page Attribute Table)

PAT controls memory caching types (WB, WT, UC, WC) on a per-page basis via the PAT MSR (`0x277`). The default configuration maps:

| PAT Index | Type |
|-----------|------|
| PAT0, PAT4 | Write-Back |
| PAT1, PAT5 | Write-Through |
| PAT2, PAT6 | Write-Combining |
| PAT3, PAT7 | Uncacheable |

### Process Isolation

Each process gets its own page directory via `vmm_create_page_directory()`. Kernel-space PDEs are shared across all address spaces via `vmm_sync_kernel_pdes()`, while user-space mappings are private. This provides strong isolation — a process cannot read or write another process's memory without explicit shared mappings.

---

## Page Fault Handling

Forest OS has a layered page fault handling system:

### Entry Point

The CPU raises exception 14 (page fault) with the faulting address in CR2 and an error code on the stack. The interrupt vector is registered to `enhanced_page_fault_handler()` (`src/mm_fault.c:520`).

### Error Code Bits

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `PF_PROT` | Protection violation (vs. not present) |
| 1 | `PF_WRITE` | Write access (vs. read) |
| 2 | `PF_USER` | Userspace access (vs. kernel) |
| 3 | `PF_RSVD` | Reserved bit violation |
| 4 | `PF_INSTR` | Instruction fetch |

### Main Handler (`mm_handle_page_fault`)

```
1. Get current process mm_struct
2. Take read lock on mmap_sem
3. Find VMA containing fault address
4. If no VMA → VM_FAULT_SIGBUS
5. If address below VMA start:
   - If VM_GROWSDOWN → expand stack, allocate pages
   - Else → VM_FAULT_SIGBUS
6. Validate access permissions (access_error)
7. Dispatch to __handle_mm_fault()
```

### `__handle_mm_fault`

```
1. Walk page tables to find PTE
2. If PTE doesn't exist:
   - If VMA has custom fault handler → call vma->vm_ops->fault()
   - Else → do_anonymous_page() (demand paging)
3. If PTE present but read-only + write fault:
   - → do_wp_page() (COW)
4. If PTE not present and in swap:
   - → swap_in_page()
```

### Demand Paging (`do_anonymous_page`)

When a process first accesses a new virtual address:

1. Allocate a page from the buddy allocator (`alloc_page(GFP_USER)`)
2. Zero-fill it (security: prevents information leaks from reused frames)
3. Create a PTE with appropriate protection bits
4. Install the PTE under `page_table_lock`
5. Flush TLB via `invlpg`

### Fault Types

| Code | Meaning | Typical Cause |
|------|---------|---------------|
| `VM_FAULT_MINOR` | Resolved without I/O | Demand page, COW |
| `VM_FAULT_MAJOR` | Required disk I/O | Swap-in, file fault |
| `VM_FAULT_SIGBUS` | Invalid access | No mapping, permission violation |
| `VM_FAULT_OOM` | Out of memory | Cannot allocate page |

---

## Memory Layout

### 32-bit Virtual Address Space

```
0xFFFFFFFF ┌─────────────────────┐
           │                     │
           │   Kernel Space      │  0xC0000000 – 0xFFFFFFFF (1GB)
           │   (shared across    │
           │    all processes)   │
           │                     │
0xC0000000 ├─────────────────────┤
           │                     │
           │   User Space        │  0x40000000 – 0xBFFFFFFF (2GB)
           │                     │
           │   Code, data, heap, │
           │   mmap, stack       │
           │                     │
0x40000000 ├─────────────────────┤
           │                     │
           │   Reserved /        │  0x00000000 – 0x3FFFFFFF (1GB)
           │   Kernel image      │
           │   PMM bitmap        │
           │   Early heap        │
           │                     │
0x00000000 └─────────────────────┘
```

### Kernel Space Layout (32-bit)

```
0xFFFFFFFF ┌─────────────────────┐
           │   Top of address    │
           │   space             │
0xFFFFF000 ├─────────────────────┤
           │   (gap)             │
           │                     │
0xD0000000 ├─────────────────────┤
           │   Temp map window   │  4MB (1024 slots)
           │   (page table acc.) │
0xCC000000 ├─────────────────────┤
           │   (gap)             │
0xC0400000 ├─────────────────────┤
           │   Kernel heap       │  Up to 64MB
           │   (grows on demand) │
0xC0000000 ├─────────────────────┤
           │   Kernel code/data  │  Linked here
0x00400000 ├─────────────────────┤
           │   PMM bitmap        │  256KB
0x00100000 ├─────────────────────┤
           │   Kernel load       │  1MB
0x00000000 └─────────────────────┘
```

### User Space Layout

```
0xBFFFF000 ┌─────────────────────┐
           │   Stack (grows ↓)   │  Default 256KB, max 8MB
           │                     │
           ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
           │                     │
           │   mmap regions      │  Files, shared memory
           │                     │
           ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
           │                     │
           │   Heap (grows ↑)    │  brk-based
           │                     │
           ├─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
           │   BSS (zero-init)   │
           │   Data (initialized)│
           │   Text (code)       │
           │                     │
0x00400000 └─────────────────────┘  Default ELF load address
```

### 64-bit Higher-Half Layout (x86_64)

```
0xFFFFFFFFFFFFFFFF ┌──────────────┐
                   │              │
0xFFFF800000000000 ├──────────────┤ Kernel heap (256MB)
                   │              │
                   │   Higher     │
                   │   half       │
                   │   kernel     │
                   │   space      │
                   │              │
0x00007FFFFFFFFFFF ├──────────────┤ End of user space
                   │              │
                   │   User       │
                   │   space      │
                   │              │
0x0000000000000000 └──────────────┘
```

---

## Build Configuration

Memory subsystem features can be toggled in `build/features/memory.mk`:

| Feature Flag | Source Files | Description |
|-------------|--------------|-------------|
| `ENABLE_PAGING` | `paging64.c`, `paging_modes.c`, `vmm.c`, `mm_vma.c`, `page_fault_*.c` | 64-bit paging and VMA |
| `ENABLE_SLAB` | `mm_slab.c` | SLAB object allocator |
| `ENABLE_COW` | `mm_cow.c`, `mm_cow_impl.c` | Copy-on-write |
| `ENABLE_SWAP` | `mm_swap.c` | Swap support |
| `ENABLE_PAGE_CACHE` | `mm_pagecache.c` | File page caching |
| `ENABLE_OOM_KILLER` | `mm_oom.c` | Out-of-memory killer |
| `ENABLE_MEMORY_RECLAIM` | `mm_reclaim.c` | LRU page reclaim |
| `ENABLE_TLB_SHOOTDOWN` | `tlb.c`, `tlb_manager.c` | TLB invalidation IPIs |
| `ENABLE_MEMORY_PROTECTION` | `mem_protect.c`, `secure_vmm.c` | NX, SMEP, SMAP |
| `ENABLE_MEMORY_DEBUG` | `memory_debug.c`, `mm_debug.c`, `memory_tests.c` | Debug/test harness |

Core allocators (`mm_init.c`, `mm_buddy.c`, `bitmap_pmm.c`, `kheap.c`) are always built and not gated.

---

## Key Source Files

| File | Purpose |
|------|---------|
| `src/bitmap_pmm.c` | Bitmap-based physical frame allocator |
| `src/mm_buddy.c` | Buddy allocator with zone support |
| `src/mm_slab.c` | SLAB object cache allocator |
| `src/vmm.c` | Virtual memory manager (32-bit) |
| `src/paging64.c` | 4-level paging for x86_64 |
| `src/mm_vma.c` | Virtual memory area management |
| `src/mm_fault.c` | Page fault handler with demand paging |
| `src/mm_cow.c` | Copy-on-write implementation |
| `src/mm_swap.c` | Swap space and LRU management |
| `src/mm_pagecache.c` | File page cache |
| `src/mm_reclaim.c` | Memory pressure and page reclaim |
| `src/mm_oom.c` | OOM killer |
| `src/mm_layout.c` | E820/memory map parsing and layout |
| `src/mm_init.c` | Subsystem initialization ordering |
| `src/mem_protect.c` | NX, SMEP, SMAP, PAT |
| `src/include/memory.h` | Core memory types and PMM/VMM API |
| `src/include/mm.h` | Buddy, SLAB, VMA, and page fault types |
| `src/arch/pmm.h` | Cross-architecture PMM interface |
| `src/arch/vmm.h` | Cross-architecture VMM interface |
| `src/arch/kheap.h` | Kernel heap API |
| `build/features/memory.mk` | Feature gating for memory subsystem |
