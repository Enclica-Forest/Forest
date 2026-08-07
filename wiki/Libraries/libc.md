# Forest OS libc

A lightweight, Linux-compatible C standard library for Forest OS. Forest OS libc provides the **middleman layer** between userspace applications and the Forest OS kernel, handling architecture abstraction, error translation, data type conversion, and POSIX-compatible function signatures.

**Source location:** `libs/libc/`

## Overview

Forest OS libc (internally called "Fern libc") is a C23-compatible standard library designed for embedded and OS-development contexts. It wraps kernel system calls behind familiar POSIX and C standard interfaces, enabling developers to write portable C code that targets Forest OS.

Key design goals:

- **Linux ABI compatibility** -- syscall numbers and register conventions match Linux x86/x86_64
- **POSIX conformance** -- stdio, stdlib, string, unistd, signal, pthread, dirent, and more
- **Freestanding operation** -- no dependency on external toolchain libraries; compiled with `-ffreestanding -nostdinc`
- **Minimal footprint** -- bump allocator for `malloc`, minimal buffering, no locale support

## Directory Structure

```
libs/libc/
├── include/libc/         # Public headers
│   ├── sys/              # System headers (types, stat, socket, mman, etc.)
│   ├── arpa/             # Network address functions (arpa/inet.h)
│   ├── netinet/          # Internet protocol headers (netinet/in.h)
│   ├── stdio.h           # Standard I/O
│   ├── stdlib.h          # General utilities, memory allocation
│   ├── string.h          # String and memory functions
│   ├── unistd.h          # POSIX system interface
│   ├── errno.h           # Error numbers
│   ├── signal.h          # Signal handling
│   ├── pthread.h         # POSIX threads (stubs)
│   ├── dirent.h          # Directory operations
│   ├── fcntl.h           # File control
│   ├── time.h            # Time functions
│   ├── math.h            # Mathematical functions
│   ├── termios.h         # Terminal I/O
│   ├── poll.h            # I/O multiplexing
│   └── ...               # 56 headers total
├── src/                  # Implementation
│   ├── syscalls.c        # Low-level syscall wrappers (1101 lines)
│   ├── string.c          # String/memory functions
│   ├── stdio.c           # Buffered I/O, printf family
│   ├── stdlib.c          # malloc, qsort, string conversion
│   ├── errno.c           # errno variable, perror()
│   ├── pthread.c         # Thread stubs (single-threaded)
│   ├── signal.c          # Signal handling
│   ├── dirent.c          # Directory reading (getdents)
│   └── assert.c          # __assert_fail()
└── README.md
```

## Supported Headers and Functions

### stdio.h -- Standard I/O

Buffered I/O built on raw `read()`/`write()` syscalls.

| Category | Functions |
|----------|-----------|
| File operations | `fopen`, `freopen`, `fclose`, `fflush`, `setvbuf` |
| Formatted I/O | `printf`, `fprintf`, `snprintf`, `sprintf`, `vsnprintf`, `dprintf` |
| Character I/O | `fgetc`, `fputc`, `getchar`, `putchar`, `ungetc` |
| String I/O | `fgets`, `fputs`, `puts` |
| Block I/O | `fread`, `fwrite` |
| Positioning | `fseek`, `ftell`, `rewind`, `fgetpos`, `fsetpos` |
| Error handling | `feof`, `ferror`, `clearerr`, `perror` |
| POSIX extensions | `fileno`, `fdopen`, `getline`, `getdelim` |
| GNU extensions | `asprintf`, `vasprintf` |
| Thread-safe | `flockfile`, `ftrylockfile`, `funlockfile` |
| Unlocked I/O | `getc_unlocked`, `putchar_unlocked`, `fread_unlocked`, etc. |

**FILE structure** (`stdio.h:51`):
```c
typedef struct _FILE {
    int fd;             /* File descriptor */
    int flags;          /* Mode and status flags */
    char *buffer;       /* Buffer pointer */
    size_t buf_size;    /* Buffer size */
    size_t buf_pos;     /* Current position in buffer */
    size_t buf_len;     /* Valid data length in buffer */
    int error;          /* Error indicator */
    int eof;            /* End-of-file indicator */
    int unget;          /* Pushed-back character (-1 if none) */
} FILE;
```

**Note:** `printf()` format specifier support includes `%s`, `%c`, `%d`/`%i`, `%u`, `%x`/`%X`, `%p`, and `%%`. Width and zero-padding are supported. `%f`/`%e`/`%g` (floating-point formatting) are not yet implemented.

### stdlib.h -- General Utilities

| Category | Functions |
|----------|-----------|
| Memory allocation | `malloc`, `calloc`, `realloc`, `free`, `posix_memalign`, `aligned_alloc` |
| String conversion | `atoi`, `atol`, `atoll`, `atof`, `strtol`, `strtoul`, `strtoll`, `strtoull`, `strtod`, `strtof` |
| Non-standard | `itoa`, `ltoa`, `lltoa` |
| Random numbers | `rand`, `srand`, `rand_r` |
| Environment | `getenv`, `setenv`, `unsetenv`, `putenv` |
| Process control | `exit`, `_Exit`, `abort`, `atexit`, `system` |
| Search/sort | `bsearch`, `qsort`, `qsort_r` |
| Math helpers | `abs`, `labs`, `llabs`, `div`, `ldiv`, `lldiv` |
| Temp files | `mkstemp`, `mkdtemp`, `mktemp`, `realpath` |

**Memory allocator** is a simple bump allocator using `brk()` (`stdlib.c:23-63`). Allocations are 16-byte aligned. `free()` is a no-op -- memory is never reclaimed. This is suitable for short-lived programs and OS development.

### string.h -- String Handling

| Category | Functions |
|----------|-----------|
| Copying | `memcpy`, `memmove`, `strcpy`, `strncpy`, `strlcpy`, `strdup`, `strndup` |
| Concatenation | `strcat`, `strncat`, `strlcat` |
| Comparison | `memcmp`, `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strcoll`, `strxfrm` |
| Search | `memchr`, `memrchr`, `memmem`, `strchr`, `strrchr`, `strstr`, `strpbrk` |
| Length | `strlen`, `strnlen` |
| Tokenization | `strtok`, `strtok_r`, `strsep` |
| Spans | `strspn`, `strcspn` |
| Set | `memset`, `memset_explicit` (C23) |
| Error strings | `strerror` |
| BSD compat | `bzero`, `bcopy`, `bcmp`, `index`, `rindex` |

`memcpy` uses word-sized copies when source, destination, and length are all aligned (`string.c:19-39`).

### unistd.h -- POSIX System Interface

| Category | Functions |
|----------|-----------|
| File I/O | `read`, `write`, `pread`, `pwrite`, `close`, `lseek`, `fsync`, `ftruncate` |
| File descriptors | `dup`, `dup2`, `dup3`, `pipe`, `pipe2` |
| Process control | `fork`, `vfork`, `execve`, `execv`, `execvp`, `execl`, `execlp`, `_exit` |
| Process info | `getpid`, `getppid`, `getpgrp`, `getpgid`, `setpgid`, `setsid`, `getsid`, `gettid` |
| User/group | `getuid`, `geteuid`, `getgid`, `getegid`, `setuid`, `setgid`, `seteuid`, `setegid` |
| Working directory | `getcwd`, `chdir`, `fchdir`, `chroot` |
| File system | `access`, `link`, `symlink`, `readlink`, `unlink`, `rmdir`, `chmod`, `chown` |
| Sleep | `sleep`, `usleep`, `alarm`, `pause` |
| Terminal | `isatty`, `ttyname`, `tcgetpgrp`, `tcsetpgrp` |
| Hostname | `gethostname`, `sethostname` |
| Misc | `sysconf`, `getopt`, `brk`, `sbrk`, `sync`, `nice` |
| Forest ext | `poweroff`, `reboot` |

### errno.h -- Error Numbers

All 133 Linux-compatible error codes are defined (`errno.h:22-155`), including:

- File errors: `ENOENT`, `EACCES`, `EEXIST`, `ENOTDIR`, `EISDIR`, `EMFILE`, `ENOSPC`
- Process errors: `ESRCH`, `ECHILD`, `EAGAIN`, `ENOMEM`
- Network errors: `ECONNREFUSED`, `ETIMEDOUT`, `EADDRINUSE`, `ENETUNREACH`
- Signal errors: `EINTR`

The `errno` macro expands to `(*__errno_location())` for thread-safe access (`errno.h:19`).

### signal.h -- Signal Handling

All standard POSIX signals (1-31) plus real-time signals (32-64) are defined.

| Category | Functions/Macros |
|----------|-----------------|
| Handler setup | `signal`, `sigaction` |
| Set manipulation | `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`, `sigismember` |
| Blocking | `sigprocmask`, `sigpending`, `sigsuspend` |
| Sending | `kill`, `killpg`, `raise`, `sigqueue` |
| Waiting | `sigwait`, `sigwaitinfo`, `sigtimedwait` |
| Info | `strsignal`, `psignal`, `psiginfo` |

The `sigaction` struct includes `sa_handler`/`sa_sigaction`, `sa_mask`, `sa_flags`, and `sa_restorer` -- matching the Linux kernel's `struct sigaction` layout.

### pthread.h -- POSIX Threads

**Important:** Full threading is not implemented in the Forest kernel. The pthread header provides a complete API surface for compatibility, but functions return error codes:

| Function | Return value |
|----------|-------------|
| `pthread_create` | `EAGAIN` (no thread support) |
| `pthread_join` | `ESRCH` |
| `pthread_self` | `getpid()` (process ID as thread ID) |
| `pthread_mutex_lock` | Always succeeds (single-threaded) |
| `pthread_cond_wait` | `EINVAL` (would deadlock) |
| `pthread_once` | Calls init routine, then marks as done |

**What works:** `pthread_mutex_init/lock/unlock`, `pthread_once`, thread-specific data (`pthread_key_create/setspecific/getspecific`), barriers, spinlocks.

**What doesn't:** `pthread_create`, `pthread_join`, `pthread_detach`, condition variable waits.

### dirent.h -- Directory Operations

| Functions |
|-----------|
| `opendir`, `fdopendir`, `closedir`, `readdir`, `readdir64` |
| `readdir_r`, `readdir64_r`, `rewinddir`, `seekdir`, `telldir`, `dirfd` |
| `scandir`, `scandir64`, `scandirat` |
| `getdents`, `getdents64` |

Directory reading uses the `getdents`/`getdents64` syscalls internally, parsing Linux-format directory entries.

### Additional Headers

| Header | Purpose |
|--------|---------|
| `fcntl.h` | File open flags (`O_RDONLY`, `O_CREAT`, `O_NONBLOCK`, etc.), `open`, `openat`, `fcntl`, `flock` |
| `time.h` | `clock`, `time`, `mktime`, `gmtime`, `localtime`, `strftime`, `clock_gettime`, `nanosleep`, `gettimeofday` |
| `math.h` | Full set of trig, log, power, rounding functions (software FPU) |
| `termios.h` | `tcgetattr`, `tcsetattr`, terminal flags |
| `poll.h` | `poll()` with `POLLIN`, `POLLOUT`, `POLLERR`, etc. |
| `sys/socket.h` | `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `setsockopt` |
| `netinet/in.h` | `sockaddr_in`, `sockaddr_in6`, protocol numbers, multicast |
| `arpa/inet.h` | `inet_ntop`, `inet_pton`, `htonl`, `htons`, `ntohl`, `ntohs` |
| `netdb.h` | `gethostbyname`, `getaddrinfo`, `getnameinfo` |
| `sys/mman.h` | `mmap`, `munmap`, `mprotect`, `madvise`, `mlock` |
| `sys/stat.h` | `struct stat`, `S_ISREG`, `S_ISDIR`, permission bits |
| `sched.h` | `sched_yield`, `sched_param`, `cpu_set_t`, scheduling policies |

## System Call Architecture

### How System Calls Work

When an application calls a libc function like `read()`:

```c
ssize_t bytes = read(fd, buffer, size);
```

The libc `read()` function (`syscalls.c:312-314`):
1. Places arguments in CPU registers (`fd` in `ebx/edi`, `buffer` in `ecx/rsi`, `size` in `edx/rdx`)
2. Places the syscall number in `eax/rax` (e.g., `SYS_read = 0`)
3. Issues `int $0x80` to transfer control to the kernel
4. Receives the return value in `eax/rax`
5. Checks for errors (negative return = error)
6. Sets `errno` if needed and returns `-1` or the actual value

### Internal Syscall Primitives

The libc defines six internal functions for invoking syscalls with 0-6 arguments (`syscalls.c:79-251`):

```c
static inline syscall_ret_t __syscall0(syscall_arg_t num);
static inline syscall_ret_t __syscall1(syscall_arg_t num, syscall_arg_t a1);
static inline syscall_ret_t __syscall2(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2);
static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2, syscall_arg_t a3);
static inline syscall_ret_t __syscall4(...);
static inline syscall_ret_t __syscall5(...);
static inline syscall_ret_t __syscall6(...);
```

The 6-argument variant uses `push %rbp` / `mov arg6, %rbp` to pass the sixth argument in EBP, then restores it.

### Generic Syscall Interface

Applications can make raw syscalls via `syscall()` (`syscalls.c:292-304`):

```c
#include <sys/syscall.h>
#include <unistd.h>

long result = syscall(SYS_write, STDOUT_FILENO, "hello\n", 6);
```

## Register Conventions

Forest OS uses Linux-compatible register conventions for system calls:

| Register (x86) | Register (x86_64) | Purpose |
|----------------|-------------------|---------|
| `eax` | `rax` | Syscall number (in) / return value (out) |
| `ebx` | `rdi` | Argument 1 |
| `ecx` | `rsi` | Argument 2 |
| `edx` | `rdx` | Argument 3 |
| `esi` | `r10` | Argument 4 |
| `edi` | `r8` | Argument 5 |
| `ebp` | `r9` | Argument 6 |

All syscalls are invoked via `int $0x80` (interrupt 0x80), which is the standard x86 Linux syscall vector.

## Error Handling

Forest OS libc follows the POSIX error convention:

1. On **success**: return the actual value (>= 0, or a valid pointer)
2. On **error**: return `-1` (for functions returning `int`/`ssize_t`) or `NULL` (for functions returning pointers), and set `errno` to the positive error code

The translation happens in `__syscall_ret()` (`syscalls.c:266-272`):

```c
static inline long __syscall_ret(syscall_ret_t ret) {
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return -1;
    }
    return (long)ret;
}
```

The kernel returns negative error codes (e.g., `-ENOENT = -2`). The libc negates them and stores the positive value in `errno`.

### perror and strerror

```c
#include <stdio.h>
#include <errno.h>

FILE *fp = fopen("/nonexistent", "r");
if (!fp) {
    perror("open failed");     // prints: "open failed: No such file or directory"
}
```

`strerror()` returns a human-readable string for any error code (`string.c:500-595`).

## Supported Syscalls

Forest OS implements Linux x86_64-compatible syscall numbers. The full list is defined in `sys/syscall.h`. Key syscalls with their numbers:

| Category | Syscall | Number |
|----------|---------|--------|
| File I/O | `read` | 0 |
| | `write` | 1 |
| | `open` | 2 |
| | `close` | 3 |
| | `lseek` | 8 |
| Memory | `mmap` | 9 |
| | `mprotect` | 10 |
| | `munmap` | 11 |
| | `brk` | 12 |
| Signals | `rt_sigaction` | 13 |
| | `rt_sigprocmask` | 14 |
| File control | `ioctl` | 16 |
| | `pipe` | 22 |
| | `select` | 23 |
| Networking | `socket` | 41 |
| | `connect` | 42 |
| | `accept` | 43 |
| | `bind` | 49 |
| | `listen` | 50 |
| Process | `fork` | 57 |
| | `vfork` | 58 |
| | `execve` | 59 |
| | `exit` | 60 |
| | `wait4` | 61 |
| | `kill` | 62 |
| User/group | `getuid` | 102 |
| | `getgid` | 104 |
| | `setuid` | 105 |
| Time | `gettimeofday` | 96 |
| | `clock_gettime` | 228 |
| | `nanosleep` | 35 |
| Mount | `mount` | 165 |
| | `umount2` | 166 |

### Forest-Specific Extensions (syscall >= 470)

| Syscall | Number | Purpose |
|---------|--------|---------|
| `netinfo` | 470 | Query network interface information |
| `mmap_fb` | 471 | Map framebuffer into userspace |
| `munmap_fb` | 472 | Unmap framebuffer |
| `get_fb_info` | 473 | Get framebuffer dimensions/format |
| `power` | 474 | Power management (shutdown/reboot) |
| `userctl` | 475 | User account management |
| `start_fb_watcher` | 476 | Start framebuffer change notifications |
| `stop_fb_watcher` | 477 | Stop framebuffer change notifications |
| `fb_flush` | 478 | Flush framebuffer to display |
| `read_kbd_event` | 479 | Read keyboard input event |
| `read_mouse_event` | 480 | Read mouse input event |
| `poll_input` | 481 | Check for pending input events |

## Networking Wrappers

Forest OS libc provides a complete BSD sockets API for TCP/UDP networking:

### Basic TCP Client

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = inet_addr("127.0.0.1"),
    };

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    const char *msg = "Hello, Forest OS!";
    write(sockfd, msg, strlen(msg));

    char buf[1024];
    ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("Received: %s\n", buf);
    }

    close(sockfd);
    return 0;
}
```

### Basic TCP Server

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = INADDR_ANY,
    };

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 128);

    printf("Listening on port 8080...\n");

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("Client said: %s\n", buf);
        write(client_fd, "Echo!", 5);
    }

    close(client_fd);
    close(server_fd);
    return 0;
}
```

### Byte Order Conversion

```c
#include <arpa/inet.h>

uint32_t net_order = htonl(0x01020304);  // host to network
uint16_t port = htons(8080);             // host to network (short)
uint32_t host_val = ntohl(net_order);     // network to host
```

### UDP Socket

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(9000),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    char buf[1024];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&sender, &sender_len);

    // Send response to sender
    sendto(fd, "ACK", 3, 0, (struct sockaddr *)&sender, sender_len);

    close(fd);
    return 0;
}
```

## Thread Support

Forest OS libc provides a full pthread API for source compatibility, but threading is not implemented in the kernel. The implementation is in `src/pthread.c`.

### What Works (Single-Threaded)

These functions operate correctly in a single-threaded context:

```c
#include <pthread.h>
#include <stdio.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int counter = 0;

void increment(void) {
    pthread_mutex_lock(&lock);
    counter++;
    pthread_mutex_unlock(&lock);
}

pthread_once_t init = PTHREAD_ONCE_INIT;
void init_func(void) {
    printf("Initialized once\n");
}

int main(void) {
    pthread_once(&init, init_func);  // Works
    pthread_once(&init, init_func);  // Skipped (already done)
    increment();                      // Works
    increment();                      // Works
    return 0;
}
```

### What Doesn't Work

```c
pthread_t tid;
int rc = pthread_create(&tid, NULL, worker, NULL);
// rc == EAGAIN (thread creation not supported)

rc = pthread_join(tid, NULL);
// rc == ESRCH (no such thread)
```

### Thread-Specific Data

```c
#include <pthread.h>

pthread_key_t my_key;

void cleanup(void *val) {
    free(val);
}

int main(void) {
    pthread_key_create(&my_key, cleanup);
    pthread_setspecific(my_key, "hello");
    char *val = pthread_getspecific(my_key);
    // val == "hello"
    pthread_key_delete(my_key);
    return 0;
}
```

## Building libc

### Integrated Build (with Forest OS build system)

```makefile
# In your Makefile
include libs/libc/Makefile.inc

CFLAGS += $(LIBC_INCLUDES)

my_program: my_program.o $(LIBC_LIBRARY)
    $(LD) -o $@ $^
```

### Standalone Build

```bash
# Create output directory
mkdir -p obj/forestlibc

# Compile all source files with freestanding flags
for src in libs/libc/src/*.c; do
    gcc -ffreestanding -nostdinc -Ilibs/libc/include/libc \
        -c "$src" -o "obj/forestlibc/$(basename "$src" .c).o"
done

# Create static library
ar rcs obj/libforest.a obj/forestlibc/*.o
```

### Compiler Flags

When using Forest OS libc, your code must be compiled with:

```
-ffreestanding    # No assumption about standard library
-nostdinc         # Don't search system include paths
-Ilibs/libc/include/libc  # Add our headers
```

Link against `libforest.a` (or the appropriate library target).

## Usage Examples

### Hello World

```c
#include <stdio.h>

int main(void) {
    printf("Hello from Forest OS!\n");
    return 0;
}
```

### File I/O

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    // Write a file
    int fd = open("/tmp/test.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "Hello, Forest!\n", 15);
        close(fd);
    }

    // Read it back with stdio
    FILE *fp = fopen("/tmp/test.txt", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            printf("%s", line);
        }
        fclose(fp);
    }

    // Check file info
    struct stat st;
    if (stat("/tmp/test.txt", &st) == 0) {
        printf("Size: %ld bytes\n", st.st_size);
    }

    return 0;
}
```

### Process Control

```c
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        printf("Child: PID=%d, PPID=%d\n", getpid(), getppid());
        _exit(0);
    } else if (pid > 0) {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        printf("Parent: child exited with status %d\n", WEXITSTATUS(status));
    }
    return 0;
}
```

### Memory Mapping

```c
#include <sys/mman.h>
#include <stdio.h>

int main(void) {
    // Map anonymous memory
    void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // Use the memory
    char *str = (char *)ptr;
    snprintf(str, 4096, "Mapped at %p", ptr);
    printf("%s\n", str);

    munmap(ptr, 4096);
    return 0;
}
```

### Directory Listing

```c
#include <dirent.h>
#include <stdio.h>

int main(void) {
    DIR *dir = opendir("/");
    if (!dir) {
        perror("opendir");
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        printf("%s (type=%d)\n", entry->d_name, entry->d_type);
    }

    closedir(dir);
    return 0;
}
```

### Time Functions

```c
#include <time.h>
#include <stdio.h>

int main(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("Current time: %s\n", buf);

    // High-resolution time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("Monotonic: %ld.%09ld seconds\n", ts.tv_sec, ts.tv_nsec);

    return 0;
}
```

### String Conversion and Sorting

```c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    // String conversion
    long val = strtol("0x1A", NULL, 16);
    printf("Parsed hex: %ld\n", val);  // 26

    // Sorting
    int arr[] = {5, 2, 8, 1, 9, 3};
    qsort(arr, 6, sizeof(int), cmp_int);

    for (int i = 0; i < 6; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");  // 1 2 3 5 8 9

    return 0;
}
```

## Limitations Compared to glibc/musl

| Feature | Forest OS libc | glibc | musl |
|---------|---------------|-------|------|
| `malloc` implementation | Bump allocator (no `free`) | Full ptmalloc2 | Full musl allocator |
| `printf` format specifiers | `%s`, `%c`, `%d`, `%u`, `%x`, `%p` | Full C11 | Full C11 |
| Floating-point formatting (`%f`) | Not implemented | Yes | Yes |
| Locale support | C locale only | Full i18n/l10n | Minimal locale |
| Wide characters | Minimal stubs | Full | Full |
| Threading | Stubs (single-threaded) | Full NPTL | Full musl threads |
| `system()` | Returns `ENOSYS` | Full implementation | Full implementation |
| `atexit` handlers | Stored but not called on `_exit` | Full | Full |
| `realloc` | Always allocates new copy | In-place when possible | In-place when possible |
| `math.h` | Software implementations | Hardware FPU | Hardware FPU |
| `regex.h` | Not implemented | Full POSIX | Full POSIX |
| `iconv.h` | Not implemented | Full | Full |
| Shared libraries | Not supported | Full ELF shared libs | Full ELF shared libs |
| Dynamic linking | Not supported | Full | Full |
| NSS/pam | Not supported | Full | Partial |

### Known Gaps

1. **`free()` is a no-op** -- memory is never returned to the kernel. Suitable for short-lived programs but will leak in long-running daemons.
2. **No `%f`/`%e`/`%g` in printf** -- floating-point formatting is not implemented. Use `snprintf` with integer arithmetic for numeric output.
3. **No thread creation** -- `pthread_create` returns `EAGAIN`. Mutex/once/TSD primitives work for single-threaded synchronization.
4. **No `system()`** -- always returns -1 with `ENOSYS`.
5. **`atexit` handlers** are registered but not invoked by `_exit()` (they are only invoked by `exit()` if it calls `_exit` after them).
6. **`readdir` skips `.` and `..`** -- unlike POSIX, these entries are filtered out in the implementation (`dirent.c:147-149`).

## Contributing

When adding new functionality:

1. Add the function declaration in the appropriate header under `include/libc/`
2. Implement in the appropriate `src/*.c` file
3. If it's a syscall wrapper, add to `src/syscalls.c` with the correct `SYS_xxx` number
4. Follow existing code style: 4-space indent, `snake_case`, `extern "C"` in headers
5. Add the function to the relevant section in the syscall number header if needed

## License

Forest OS libc is part of Forest OS and follows the same license terms.
