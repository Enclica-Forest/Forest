# Security

Forest OS implements a layered security architecture combining hardware-enforced memory protections, software stack defences, memory validation, and authentication subsystems. Features are gated at build time via `build-config.mk` and `build/features/security.mk`.

## Build Configuration

Key security flags in `build-config.mk`:

| Flag | Default | Description |
|---|---|---|
| `ENABLE_SMEP_SMAP` | yes | Hardware supervisor memory protection |
| `ENABLE_STACK_PROTECTION` | yes | Stack canaries and SSP |
| `ENABLE_MEMORY_VALIDATION` | yes | Pointer and region validation |
| `ENABLE_FAULT_PREVENTION` | yes | Exception handlers and fault recovery |
| `ENABLE_NX_BIT` | yes | No-execute page support |
| `ENABLE_LOCK_DEBUGGING` | yes | Spinlock contention tracking |
| `ENABLE_AUTH` | yes | User authentication subsystem |
| `ENABLE_GUARD_PAGES` | yes | Guard pages on stacks and heaps |

---

## SMEP and SMAP

**Source:** `fern/src/smep_smap.c`

SMEP (Supervisor Mode Execution Prevention) and SMAP (Supervisor Mode Access Prevention) prevent the kernel from accidentally executing or accessing user-space memory while in ring 0.

At boot, `smep_smap_detect_features()` queries CPUID leaf 7 for feature bits. If available, CR4 bits 20-21 are set. When the kernel needs to copy user data (syscalls), it temporarily clears the AC flag via `stac`, performs the copy, then restores protection via `clac`.

Convenience macros wrap this pattern:

```c
#define USER_ACCESS_BEGIN() enable_user_access()
#define USER_ACCESS_END()   disable_user_access()
```

`safe_user_memory_check()` validates that user pointers fall within the user address range (`< 0xC0000000`), preventing confused-deputy attacks. Debug functions exist to disable/reenable SMEP/SMAP but should never be used in production.

---

## Stack Smashing Protection (SSP)

**Source:** `fern/src/ssp.c`, `fern/src/include/ssp.h`

SSP implements stack canaries similar to GCC's `-fstack-protector`. The global `__stack_chk_guard` is a 32-bit random value generated from multiple entropy sources:

- `rdtsc()` (CPU timestamp counter)
- Address of internal data structures (provides ASLR-like entropy)
- A linear congruential PRNG seeded at boot

The canary is guaranteed to never be `0x00000000`, `0xFFFFFFFF`, `0xDEADBEEF`, or `0xFEEDFACE` -- common patterns an attacker might guess.

Functions marked `SSP_PROTECTED` (or `__attribute__((stack_protect))`) store the canary on entry and verify it on exit. If corrupted, `__stack_chk_fail()` fires with a carefully designed emergency response:

1. Interrupts disabled immediately (`cli`)
2. ESP/EBP switched to a static 1024-byte emergency stack via inline assembly (no function call needed)
3. Violation announced via direct VGA memory writes and COM1 serial output -- no heap allocation or stack usage
4. CPU halts forever with `hlt`

The SSP header also provides `SSP_VALIDATE_FRAME()` and `SSP_PROTECT_RETURN()` macros for manual frame and return-address validation. `ssp_validate_return_address()` checks that return addresses fall within kernel image bounds, and `ssp_validate_stack_frame()` verifies frame pointer alignment and range.

---

## Stack Overflow Protection

**Source:** `fern/src/stack_protection.c`, `fern/src/include/stack_protection.h`

Beyond SSP canaries, the kernel implements runtime stack monitoring for the boot-time kernel stack:

- A sentinel canary (`STACK_CANARY_VALUE = 0xDEADBEEF`) is placed at the bottom of the usable stack region.
- `stack_check_integrity()` verifies both ESP bounds and the canary value.
- `stack_check_overflow()` returns `-1` (overflow), `1` (warning, <1KB remaining), or `0` (OK).

The `STACK_CHECK_ENTER()` macro provides an inline check suitable for critical functions, writing directly to VGA memory on overflow before calling `kernel_panic()`.

A static 2KB `emergency_stack` is available via `switch_to_emergency_stack()`, which redirects ESP to a safe buffer when the main stack is exhausted.

After tasking begins, each task gets its own 8KB stack allocated via `kmalloc`. The boot stack checks are automatically skipped when running on a per-task stack, detected by checking if the current ESP falls within the boot stack bounds. `stack_quick_check()` provides a fast ESP sanity check (above 1MB, below 4GB, dword-aligned) used in the page fault handler to catch obviously corrupt stack pointers.

---

## Memory Corruption Detection

**Source:** `fern/src/memory_validation.c`, `fern/src/include/memory_safe.h`

The kernel validates memory at multiple levels:

**Pointer validation** -- `memory_validate_pointer()` rejects NULL, kernel-space, and reserved-region addresses.

**Region validation** -- `memory_validate_region()` checks ranges with overflow detection (`start > UINT32_MAX - length`) and reserved-region overlap checks.

**Heap block integrity** -- Every heap block carries magic headers (`0xDEADBEEF`/`0xFEEDFACE`) and a footer (`0CAFEBABE`). `memory_validate_heap_block()` verifies both.

**User buffer probing** -- `memory_probe_user_buffer()` determines how many bytes of a user buffer are safely readable before any copy, preventing kernel pointer leaks.

**Validation state integrity** -- The validation subsystem protects itself with a checksum over its configuration, detectable via `memory_check_validation_integrity()`.

---

## Secure VMM

**Source:** `fern/src/include/secure_vmm.h`

The Secure VMM defines a comprehensive virtual memory framework:

- **NX bit** -- `VMM_PAGE_NO_EXECUTE` (bit 63) marks data pages non-executable.
- **Guard pages** -- `VMM_PROT_GUARD` creates fault-on-access pages for overflow detection.
- **COW** -- `VMM_PROT_COW` enables fork()-style memory sharing with per-page write tracking.
- **Per-address-space security** -- `vmm_address_space_t` tracks ASLR, DEP, and guard-page flags.
- **Corruption detection** -- Memory areas and address spaces carry magic values and checksums.
- **Violation statistics** -- Tracks read/write/execute violations, guard page hits, and corruption events.

The actual VMM (`fern/src/vmm.c`) implements page table manipulation with TLB invalidation, temporary mappings for page table access, and COW-aware frame teardown via `cow_release()`.

---

## Process Isolation

Each task receives its own page directory via `vmm_create_page_directory()`. Kernel higher-half mappings are shared; user-space pages are private. `vmm_sync_kernel_pdes()` keeps kernel PDEs synchronized during task switches without overwriting user PDEs.

On `fork()`, `cow_fork_address_space()` creates a shallow copy. Both parent and child share physical pages until one writes, triggering a COW fault. `vmm_destroy_page_directory()` routes frame freeing through `cow_release()` to prevent freeing frames still used by siblings.

System calls validate all user pointers via `user_buffer_readable()`/`user_buffer_writable()` before use. `user_copy_string()` copies byte-by-byte with bounds checking. The ELF loader validates headers and checks memory availability before loading binaries.

---

## Memory Protection (NX, W^X)

On x86_64, `vmm_to_x64_flags()` in `arch/vmm.c` applies NX by default:

```c
if (!(flags & PAGE_EXECUTABLE)) f |= PAGE64_NX;
```

All pages are non-executable unless explicitly marked `PAGE_EXECUTABLE`, implementing W^X. The `secure_vmm.h` header defines explicit protection levels: `VMM_PROT_RW` (no execute), `VMM_PROT_RX` (no write), enforcing separation of code and data.

Architecture support varies: x86_64 uses PTE bit 63, AArch64 uses `PTE_UXN`/`PTE_PXN`, ARM32 uses `ARM_PAGE_XN`, and RISC-V uses SV39's `PTE_X` flag.

---

## Kernel Address Space Layout Randomization

**Status:** Defined but disabled by default.

`ENABLE_KASLR` (default `0`) in `build_options.h` and `ENABLE_ASLR=no` in `build-config.mk`. The `secure_vmm.h` interface defines `vmm_address_space_t.aslr_enabled` and `secure_vmm_enable_aslr()`, but the runtime implementation is incomplete.

When implemented, KASLR would randomize kernel load address, data structure locations, and user-space mmap regions. This requires a hardware RNG (RDRAND/RDSEED) for entropy -- noted as a gap in the auth subsystem as well.

---

## System Call Validation

Every syscall validates user arguments before processing:

- `user_buffer_readable()` / `user_buffer_writable()` use `memory_probe_user_buffer()` to verify buffers are entirely within user address space and don't cross into kernel memory or reserved regions.
- `user_copy_string()` copies strings byte-by-byte with explicit bounds, stopping at the first null terminator or the validated buffer boundary.
- `resolve_fd_alias()` follows alias chains with a depth limit of 8 to prevent infinite loops from corrupted alias tables.
- `vfs_handle_owner_pid[]` tracks fd ownership per task; `syscall_close_all_fds_for_task()` finds and closes all file descriptors owned by a dying task, preventing resource leaks.
- `sys_open()` copies the path string from user space into a kernel buffer before any VFS processing.

---

## Buffer Overflow Protections

Forest OS combines multiple defences:

- **SSP canaries** -- `SSP_PROTECTED` functions have stack canaries checked on every return.
- **Bounds-checking functions** -- `strncpy()` with explicit sizes, `snprintf()` over `sprintf()` throughout the codebase.
- **SMAP/SMEP** -- Prevents unauthorized user-memory access and execution from ring 0.
- **Guard pages** -- Stack/heap overflows hit unmapped pages instead of corrupting adjacent memory.
- **NX bit** -- Data pages are non-executable, preventing code injection attacks from turning overflows into arbitrary execution.
- **Auth hardening** -- 200,000-round iterated SHA-256 hashing, constant-time hash comparison, password length rejection, and legacy hash migration on login.

---

## Fault Prevention

**Source:** `fern/src/fault_prevention.c`, `fern/src/include/fault_prevention.h`

The fault prevention subsystem handles CPU exceptions with configurable recovery strategies. Three presets are provided: strict (panic on everything), permissive (attempt recovery), and default (balanced).

| Vector | Exception | Default Recovery |
|---|---|---|
| 0 | Divide Error | Skip instruction, set RAX=0 |
| 6 | Invalid Opcode | Optional instruction emulation |
| 8 | Double Fault | Stack integrity check, emergency recovery |
| 13 | General Protection | Optional instruction skip (error code 0) |
| 14 | Page Fault | Demand paging, null-pointer handling |
| 18 | Machine Check | MSR reset, optional recovery |

Dedicated 8KB IST stacks with guard patterns (`0xDEADBEEFCAFEBABE`) protect double faults, NMIs, and machine checks. Each stack's integrity is verified on fault entry; corruption triggers a critical error declaration.

`is_fault_rate_excessive()` monitors faults per second using a 1-second sliding window. Exceeding `max_faults_per_second` (default: 100) triggers emergency shutdown. Triple fault prevention halts the system when nested fault depth reaches 3, before the CPU can generate a hardware triple fault.

`fault_prevention_check_system_health()` provides a risk assessment (LOW / MEDIUM / HIGH / CRITICAL) based on fault history, double fault occurrence, stack integrity, and fault rate.

---

## Authentication

**Source:** `fern/src/auth.c`, `fern/src/include/auth.h`

The kernel includes a built-in authentication subsystem with user/group management and credential verification.

### Password hashing

Passwords are hashed using iterated SHA-256 with 200,000 rounds (`AUTH_HASH_ROUNDS`). The construction is:

1. `digest_0 = SHA256(salt || ':' || password)`
2. `digest_i = SHA256(digest_(i-1) || salt || password)` for 200,000 rounds
3. Output is the hex-encoded final digest

This iterated approach multiplies the CPU cost of offline brute-force attacks by roughly 200,000x compared to a single round, while remaining acceptable (tens of milliseconds) for interactive login.

### Salt generation

Salts are generated from timer ticks, uid/gid counters, and a monotonically incrementing call counter, mixed through an xorshift PRNG. The source code explicitly documents this as low entropy -- the salt's role is to defeat rainbow tables across accounts, not to serve as a secret.

### Constant-time comparison

`hash_hex_equal()` XORs every byte pair and OR-accumulates the result over the full fixed length, never short-circuiting on the first mismatch. This prevents timing side-channel attacks that could recover the hash character by character.

### Legacy migration

Accounts imported from `/etc/shadow` start with single-round hashes (`AUTH_HASH_VERSION_LEGACY`). On first successful login, the in-memory record is upgraded to the iterated format (`AUTH_HASH_VERSION_ITERATED`). This upgrade is in-memory only due to the read-only initrd backing store.

---

## Security Limitations

- **No hardware RNG** -- SSP canary and auth salt entropy relies on TSC and address layout, which are predictable to an attacker who can observe boot timing. This is a prerequisite gap for meaningful KASLR.
- **64-bit stubs** -- SMEP/SMAP, SSP, and stack protection are no-ops on x86_64 builds. Functions exist but return immediately.
- **No runtime ASLR** -- The `secure_vmm.h` interface defines ASLR support, but `ENABLE_ASLR=no` in the build config and no runtime implementation exists.
- **No page table write protection** -- Page directory and page table frames can be modified by any kernel code with direct memory access, enabling a wide class of privilege escalation attacks.
- **Single kernel address space** -- All kernel memory is in one shared address space. No per-subsystem isolation limits the blast radius of a vulnerability.
- **Auth persistence** -- Hash upgrades from legacy to iterated format are in-memory only. The `/etc/shadow` file lives on a read-only initrd, so upgrades are lost on reboot.
- **Debug backdoors** -- Functions like `debug_disable_smep_smap()` can disable hardware memory protection. These should be compiled out of release builds.

## Future Improvements

- **Hardware RNG integration** -- RDRAND/RDSEED on x86 or equivalent entropy sources for cryptographic canary and salt generation.
- **Full KASLR implementation** -- Randomize kernel load address, data structures, and user-space mmap regions using hardware entropy.
- **Stack canaries on x86_64** -- Port the SSP implementation to 64-bit, including emergency stack handling.
- **Page table write protection** -- Write-protect page directory and page table frames to prevent arbitrary memory mapping.
- **Kernel address space isolation** -- Split kernel memory into per-subsystem regions with different access permissions.
- **Per-task fault budgets** -- Kill tasks that fault too often; integrate with the OOM killer for memory-related faults.
- **User-space signal delivery** -- Deliver recoverable faults (e.g., page faults on demand-paged regions) as signals instead of panicking.

---

## Related Pages

- [Memory Management](Memory-Management.md)
- [Process Management](Process-Management.md)
- [Overview](Overview.md)
