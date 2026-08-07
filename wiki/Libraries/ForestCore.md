# ForestCore

ForestCore is the low-level kernel runtime library for the Forest operating system. It provides the foundational primitives that the kernel, boot-time services, and early userspace programs need before a full C library is available. Think of it as the "kernel's libc" — a compact, freestanding set of helpers for hardware access, string manipulation, memory operations, audio, and networking.

## Overview

ForestCore lives under `libs/forestcore/` and is split into:

- **`include/`** — Forest-specific headers (`types.h`, `system.h`, `net.h`, `string.h`, `util.h`, `driver.h`)
- **`src/`** — Freestanding implementations (`audio.c`, `string.c`, `system.c`, `util.c`)

It is designed to be self-contained: no dependencies on an external libc. The kernel links ForestCore directly, and a snapshot is exported for userspace via `make refresh-libc`.

---

## The Types System (`types.h`)

All of ForestCore's APIs use explicit-width integer types defined in `types.h`:

```c
typedef signed char int8;
typedef unsigned char uint8;
typedef signed short int16;
typedef unsigned short uint16;
typedef signed int int32;
typedef unsigned int uint32;
typedef signed long long int64;
typedef unsigned long long uint64;

typedef char* string;
```

In kernel builds (where `USERSPACE_BUILD` is not defined), `float` is redefined as `double`, and `true`/`false` are provided for environments without `<stdbool.h>`. Two utility macros extract 16-bit halves of an address:

```c
#define low_16(address)  (uint16)((address) & 0xFFFF)
#define high_16(address) (uint16)(((address) >> 16) & 0xFFFF)
```

---

## I/O Port Access (`system.h`)

These functions wrap x86 `in`/`out` instructions for direct hardware port communication:

```c
uint8  inportb(uint16 port);           // Read byte
uint16 inportw(uint16 port);           // Read word
uint32 inportd(uint16 port);           // Read dword
void   outportb(uint16 port, uint8 data);
void   outportw(uint16 port, uint16 data);
void   outportd(uint16 port, uint32 data);
void   io_wait(void);                  // Delay via port 0x80
```

All port functions validate that the port number is in range (`< 0x10000`) before issuing the instruction, returning zero or silently returning on invalid ports. Example:

```c
uint8 rtc_read_register(uint8 reg) {
    outportb(0x70, reg);
    io_wait();
    return inportb(0x71);
}
```

---

## MMIO (Memory-Mapped I/O) Access (`system.h`)

For devices memory-mapped into the physical address space:

```c
uint8  mmio_read8(const volatile void* address);
uint16 mmio_read16(const volatile void* address);
uint32 mmio_read32(const volatile void* address);
void   mmio_write8(volatile void* address, uint8 value);
void   mmio_write16(volatile void* address, uint16 value);
void   mmio_write32(volatile void* address, uint32 value);
```

Every access is guarded: the function probes the address through the memory safety layer (`memory_probe_buffer` / `memory_probe_user_buffer`) to verify the pointer is valid before dereferencing. If the probe fails, reads return `0` and writes silently no-op. The `volatile` qualifier ensures the compiler emits actual memory accesses rather than caching values.

---

## CPU and System Information (`system.h`)

```c
uint64 cpu_read_tsc(void);              // Read TSC, or fallback counter
bool   cpu_has_tsc(void);               // Check if CPU supports TSC
uint32 cpu_get_cr0(void);
void   cpu_set_cr0(uint32 value);
void   cpu_set_cr3(uintptr_t value);    // Set page table base
bool   rtc_read_time(rtc_time_t* out);  // Read Real-Time Clock
void   timer_sleep_ms(uint32 milliseconds);
```

The `rtc_time_t` structure holds the current date and time:

```c
typedef struct {
    uint8  seconds, minutes, hours, day_of_month, month;
    uint16 year;
} rtc_time_t;
```

`timer_sleep_ms` uses TSC-based busy-waiting when available, falling back to a port-I/O delay loop on hardware without TSC support.

---

## String Manipulation (`string.h`)

ForestCore provides a full suite of C string functions, all implemented with **memory safety guards**. Before every access, functions call `probe_guarded_span()` which validates that the pointer range is safe to read/write. This prevents kernel panics from bad pointers in userspace or driver code.

| Function | Description |
|---|---|
| `strlen(s)` | Length of string `s` |
| `strcpy(dest, src)` / `strncpy(dest, src, n)` | Copy string |
| `strcat(dest, src)` / `strncat(dest, src, n)` | Append string |
| `strcmp(s1, s2)` / `strncmp(s1, s2, n)` | Lexicographic comparison |
| `strchr(s, c)` / `strrchr(s, c)` | Find first/last occurrence |
| `strstr(haystack, needle)` | Find substring |
| `strtok(str, delim)` | Tokenize string (thread-unsafe) |
| `strerror(errnum)` | Map error code to string |

### Memory Functions

```c
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int   memcmp(const void* s1, const void* s2, size_t n);
```

`memmove` is optimized: when source and destination are word-aligned, it copies in bulk via `unsigned long` words instead of byte-by-byte — a significant performance win for framebuffer scrolling and TTY operations.

### Formatting

```c
int string_format(char* buffer, size_t size, const char* format, ...);
```

A safe `printf`-style formatter wrapping `vsnprintf`. Returns characters written, or `-1` on error.

### Legacy Helpers

```c
uint16 strlength(const char* ch);                  // Returns uint16
uint8  strEql(const char* ch1, const char* ch2);   // Returns 1 if equal
```

---

## Utility Functions (`util.h`)

### Memory and Number Conversion

```c
void   memory_copy(const char* source, char* dest, int nbytes);
void   memory_set(uint8* dest, uint8 val, uint32 len);
void   int_to_ascii(int n, char str[]);
string int_to_string(int n);             // Heap-allocated
string long_to_string(long n);           // Heap-allocated
int    str_to_int(string ch);
char*  itoa(int value, char* str, int base);
int    atoi(const char* str);
```

### Memory Allocation (Kernel Builds)

```c
void* malloc(size_t nbytes);
void  free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);
```

In kernel builds, these delegate to `kmalloc`/`kfree`/`krealloc`. In userspace builds (`USERSPACE_BUILD`), `malloc`/`free` are not provided — the standard libc handles allocation.

### Sorting, Random, Process Control

```c
void* bsearch(const void* key, const void* base, size_t num, size_t size,
              int (*compare)(const void*, const void*));
void  qsort(void* base, size_t num, size_t size,
            int (*compare)(const void*, const void*));  // Bubble sort
int   rand(void);     // Simple LCG PRNG
void  srand(unsigned int seed);
void  exit(int status);  // Halt CPU with `hlt`
void  abort(void);
```

---

## Audio Helpers

ForestCore provides a driver-agnostic audio layer. Audio hardware drivers register their hooks at init time:

```c
typedef struct {
    bool (*output)(const uint8* data, uint32 length,
                   const audio_format_t* format, void* context);
    void (*beep)(uint32 frequency_hz, uint32 duration_ms, void* context);
    void* context;
} audio_driver_hooks_t;

bool audio_register_driver(const audio_driver_hooks_t* hooks);
void audio_unregister_driver(void);
bool audio_driver_ready(void);
```

Playback and beeping are then available to any kernel subsystem:

```c
audio_play_pcm(data, length, &format);  // Play PCM audio
audio_beep(440, 200);                    // 440Hz beep for 200ms
```

Master volume is managed internally (0–100). Supported formats: 1–2 channels, 8/16 bits per sample, 8000–192000 Hz.

---

## Network Helpers (`net.h`)

### Address Structures

```c
typedef struct { uint16 sa_family; char sa_data[14]; }      sockaddr_t;
typedef struct { uint16 sin_family; uint16 sin_port;
                 uint32 sin_addr; uint8 sin_zero[8]; }      sockaddr_in_t;
typedef struct { uint16 sun_family; char sun_path[108]; }   sockaddr_un_t;
```

### Byte Order Conversion

```c
static inline uint16 htons(uint16 value);
static inline uint16 ntohs(uint16 value);
static inline uint32 htonl(uint32 value);
static inline uint32 ntohl(uint32 value);
```

### Socket API

```c
int32 net_socket_create(uint32 domain, uint32 type, uint32 protocol);
int32 net_bind(uint32 fd, uint16 port);
int32 net_send_datagram(uint32 fd, const uint8* buffer, uint32 length,
                         uint32 dest_addr, uint16 dest_port);
int32 net_recv_datagram(uint32 fd, uint8* buffer, uint32 length,
                         uint32* out_addr, uint16* out_port);
int32 net_close(uint32 fd);
```

Predefined constants include well-known ports (`NET_PORT_HTTP`, `NET_PORT_SSH`, etc.) and socket types (`SOCK_STREAM`, `SOCK_DGRAM`).

---

## Driver Framework (`driver.h`)

ForestCore includes a simple driver management system:

```c
typedef struct {
    const char* name;
    driver_class_t driver_class;    // INPUT, SOUND, STORAGE, NETWORK, MISC
    bool (*init)(driver_t* driver);
    void (*shutdown)(driver_t* driver);
    void (*main)(void);
    void* context;
    uint16 id;
    bool initialized;
} driver_t;
```

Drivers register themselves and emit typed events:

```c
driver_register(&my_driver);
driver_emit_event(driver_id, DRIVER_CLASS_NETWORK,
                  DRIVER_EVENT_NETWORK_RX_READY, payload, len);

driver_event_t event;
driver_event_pop(&event);
```

---

## Platform-Specific Implementations

ForestCore is x86-centric. Key platform details:

- **Inline assembly**: I/O port instructions (`inb`, `outb`, `inw`, `outw`, `inl`, `outl`), `rdtsc`, control register access (`mov %cr0`, `mov %cr3`), and `hlt` are all x86 inline assembly.
- **Architecture detection**: `system.c` and `util.c` check for `__x86_64__` to select 32-bit vs 64-bit code paths (e.g., `cpu_set_cr3` uses different register constraints).
- **Software division**: `system.c` provides `__divdi3`, `__udivdi3`, `__moddi3`, and `__umoddi3` as libgcc replacements for 64-bit integer division.
- **Freestanding**: No reliance on a host OS or standard library beyond `<stdbool.h>`, `<stddef.h>`, and `<stdint.h>`.

---

## How ForestCore Is Used

### Kernel Side

The kernel links ForestCore directly and uses it for:

- **Boot**: I/O port access to initialize PIC, PIT, and RTC
- **Drivers**: MMIO for PCI/device registers, driver framework for device management
- **Memory management**: `memset`/`memcpy` for page table manipulation
- **TTY/Framebuffer**: `memmove` for screen scrolling
- **Audio**: PCM output and beep via `audio_play_pcm` / `audio_beep`
- **Networking**: Socket API for TCP/UDP communication

### Userspace Side

A snapshot of ForestCore is exported into the userspace C library via `make refresh-libc`. Userspace programs get the string functions, number conversion, and networking helpers, but not the kernel-specific primitives (MMIO, port I/O, control registers).

The `USERSPACE_BUILD` preprocessor flag controls this split — when defined, `malloc`/`free` are excluded (the real libc provides them) and `float`/`true`/`false` are not redefined.

---

## Relationship to libc

ForestCore and the C standard library are complementary:

| Feature | ForestCore | libc |
|---|---|---|
| String functions | Guarded, kernel-safe | Standard implementations |
| `malloc`/`free` | Delegates to `kmalloc`/`kfree` | Full heap allocator |
| I/O ports | `inportb`, `outportb` | Not available |
| MMIO | `mmio_read32`, `mmio_write32` | Not available |
| CPU registers | `cpu_get_cr0`, `cpu_set_cr3` | Not available |
| Audio | Driver hook system | Not available |
| Networking | `net_socket_create`, `htons` | Full socket API |
| `printf` | `string_format` (via `vsnprintf`) | Full `printf` |

ForestCore is the minimal runtime the kernel needs *before* a full libc is available. Once userspace is running, the standard libc takes over, but ForestCore's guarded string functions and networking helpers remain available.

---

## Error Handling Patterns

ForestCore uses simple, consistent error handling:

- **Boolean returns**: Most functions return `bool` (`true` on success, `false` on failure) — e.g., `audio_register_driver`, `rtc_read_time`, `driver_register`.
- **Error codes**: Networking functions return `int32` with negative error codes — e.g., `net_socket_create` returns `-1` on failure.
- **Null safety**: Functions check for null pointers before dereferencing (e.g., `rtc_read_time` returns `false` if `out` is null).
- **Guarded access**: All string and memory functions validate pointer ranges before touching memory, silently failing on invalid addresses rather than panicking.
- **Graceful degradation**: `cpu_read_tsc` returns a fallback counter if TSC is unavailable; `timer_sleep_ms` falls back to port-I/O delays.

```c
// Example: safe RTC read
rtc_time_t time;
if (!rtc_read_time(&time)) {
    // Handle error — pointer was null or RTC unavailable
}

// Example: guarded MMIO write
mmio_write32(device_reg, 0x01);  // Silently no-ops if address is invalid
```
