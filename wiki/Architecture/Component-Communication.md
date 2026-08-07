# Component Communication in Forest OS

Forest OS is a hobby operating system built from scratch, with the kernel (Fern), a custom C library, and a set of userspace utilities. Understanding how these pieces talk to each other is key to understanding the whole system. This page walks through every major communication pathway, from hardware interrupts all the way up to your shell prompt.

---

## The Layered Communication Model

At its core, Forest OS follows a classic layered architecture. Think of it like a set of nested boxes, each one only talking to the layer directly above or below it:

```
+--------------------------------------------------+
|               USERSPACE APPLICATIONS              |
|   (shell, X11 server, utilities, user programs)   |
+--------------------------------------------------+
          | syscall (int 0x80)           ^
          v                              |
+--------------------------------------------------+
|                    LIBC                           |
|   (POSIX wrappers, errno translation, ABI)        |
+--------------------------------------------------+
          | int 0x80 / register args     ^
          v                              |
+--------------------------------------------------+
|                 FERN KERNEL                        |
|   (syscall dispatcher, VFS, net, drivers)          |
+--------------------------------------------------+
          | port I/O / MMIO / DMA       ^
          v                              |
+--------------------------------------------------+
|                  HARDWARE                          |
|   (PS/2, PCI devices, NICs, USB, framebuffer)     |
+--------------------------------------------------+
```

The rule is simple: userspace never touches hardware directly. It always goes through libc, which goes through syscalls, which go through the kernel. The kernel is the only thing that talks to hardware. This keeps things safe and clean (mostly).

---

## System Calls: The Primary Communication Channel

Syscalls are the backbone of everything in Forest OS. When an application wants to read a file, send network data, or even get the current time, it makes a syscall.

### How Syscalls Work

Forest OS uses `int 0x80` for syscall invocation, regardless of whether you're on 32-bit or 64-bit. The convention (borrowed from Linux) is:

- `rax` / `eax`: syscall number
- `rbx` / `edi`: argument 1
- `rcx` / `esi`: argument 2
- `rdx` / `edx`: argument 3
- And so on for up to 6 arguments

The libc provides the `__syscall0` through `__syscall6` helper functions that handle the inline assembly. For example, from `libs/libc/src/syscalls.c`:

```c
static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1,
                                       syscall_arg_t a2, syscall_arg_t a3) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}
```

The kernel side (`fern/src/syscall.c`) has a giant dispatch table mapping syscall numbers to handler functions. It looks up the function pointer, calls it, and returns the result. The kernel returns negative errno-style values on error; libc translates these into `-1` + `errno` for POSIX compatibility.

```
Application          libc                    Kernel
-----------          ----                    ------
write(fd, buf, n)
  |-> __syscall3(SYS_write, fd, buf, n)
        |-> int $0x80  ----------------->  syscall_table[SYS_write]
              CPU switches to ring 0       sys_write(fd, buf_ptr, len)
              registers saved                  |-> validate user pointer
              dispatch table lookup            |-> find fd in vfs_handles[]
              sys_write called                 |-> tty_putc() or vfs_write()
              return value in rax              |-> return result
        |<- rax -------------------------
  |<- errno translation
  |<- return value
```

### What Syscalls Exist

Forest OS implements a large subset of the Linux syscall ABI (see `fern/src/include/syscall.h`). The major categories are:

- **File I/O**: read, write, open, close, lseek, stat, dup, pipe
- **Process control**: fork, vfork, execve, exit, wait4, getpid, kill
- **Memory**: mmap, munmap, brk, mprotect
- **Signals**: rt_sigaction, rt_sigprocmask, rt_sigsuspend
- **Networking**: socket, bind, listen, accept, connect, sendto, recvfrom
- **Time**: gettimeofday, clock_gettime, nanosleep
- **Forest-specific**: mmap_fb, read_kbd_event, read_mouse_event, fb_flush, netinfo

---

## IPC Mechanisms

Forest OS provides several IPC mechanisms. Most of them live in the kernel's syscall layer (`fern/src/syscall.c`).

### Pipes

Pipes are implemented as a simple circular buffer. The kernel maintains a `pipe_t` struct with a 4096-byte buffer, read/write positions, and end-point flags:

```
Process A (writer)                Process B (reader)
        |                               |
        | write(fd, data, len)          |
        |   -> pipe buffer [][][]       |
        |                               |  read(fd, buf, len)
        |                               |  <- data from pipe buffer
```

When you call `pipe()`, the kernel creates two file descriptors pointing to the same `pipe_t` struct. Writing to the write-end fills the buffer; reading from the read-end drains it. If the buffer is full, writes return 0 (no blocking in this implementation). If the read-end is closed, writes get `EPIPE`.

### Unix Domain Sockets (socketpair)

Forest OS supports `AF_UNIX` socket pairs. These are implemented as two unidirectional ring buffers (one for each direction) inside a `unix_socketpair_t` struct:

```
Process A                    Kernel                     Process B
    |                         |                           |
    | send(fd, data, len)     |                           |
    |  -> a_to_b ring buf --> |                           |
    |                         | --> recv(fd, buf, len)    |
    |                         |                           |
    | <-- recv(fd, buf, len)  | <-- send(fd, data, len)  |
    |  <- b_to_a ring buf <-- |                           |
```

Each direction has its own 4096-byte buffer with read/write cursors. This gives you bidirectional communication between two processes.

### Unix Domain Sockets (path-based)

Forest OS also supports path-bound `AF_UNIX` sockets (like Linux's `/tmp/.X11-unix/X0`). The X11 server uses exactly this mechanism. The kernel maintains:

- `unix_path_socket_t`: represents a socket that can be bound to a path
- `unix_path_connection_t`: represents a connected pair
- A pending connection queue for `listen()` / `accept()`

When a client connects, the kernel creates a `unix_path_connection_t` with two ring buffers (one for each direction) and returns a new fd to the accepting server.

### Shared Memory (SysV shm)

The kernel implements `shmget`, `shmat`, `shmdt`, and `shmctl`. Shared memory segments are tracked by `shm_segment_t` structs. Attachments map the segment into the calling process's address space.

### Signals

Signals work similarly to Linux. The kernel maintains per-task signal masks and pending signals. `kill(pid, sig)` sets the pending bit; on return to userspace, the kernel checks for pending signals and invokes the registered handler via `sigaction`.

### PTY (Pseudo-Terminals)

PTYs are essential for terminal emulation. The kernel implements `/dev/ptmx` (master) and `/dev/pts/N` (slave). Each PTY has two ring buffers (master-to-slave and slave-to-master). The shell opens ptmx, forks, and the child opens the slave -- this gives the shell full control over the terminal, including raw mode, job control via `tcsetpgrp`, and SIGTTIN/SIGTTOU enforcement.

---

## How the Shell Communicates with the Kernel

The shell (`userspace/forest-shell/shell.c`) is just a regular userspace program. It communicates with the kernel entirely through libc calls, which in turn use syscalls.

```
forest-shell
    |
    | printf("\033[1;32muser@forest\033[0m$ ")   -- writes to stdout
    | read(STDIN_FILENO, &c, 1)                   -- reads keyboard input
    | fork()                                       -- create child process
    | execv("/bin/ls", argv)                       -- replace child with command
    | waitpid(pid, &status, 0)                     -- wait for child
    | pipe(pipefd)                                 -- create pipe for redirection
    | dup2(pipefd[1], STDOUT_FILENO)               -- redirect stdout to pipe
    |
    v
libc: write(1, "\033[1;32m...", 30)
    |
    | int $0x80  (syscall: SYS_write = 1)
    v
Kernel: sys_write(1, buf_ptr, 30)
    |-> fd 1 goes to the TTY
    |-> tty_putc() renders each character to the framebuffer console
    |-> debuglog_write_char() logs to serial
```

### Reading Input

When the shell calls `read(STDIN_FILENO, &c, 1)`, the kernel's `sys_read` checks the controlling TTY. If it's in canonical mode (ICANON set), it calls `readStr()` which blocks until the user presses Enter, then returns the full line. If it's in raw mode (ICANON cleared), it calls `keyboard_read_raw()` which returns bytes one at a time. The shell uses raw mode for its own line editing (arrow keys, backspace handling).

### Job Control

The shell uses `tcsetpgrp()` to give foreground process groups control of the terminal. When you press Ctrl+C, the kernel sends SIGINT to the foreground process group of the controlling TTY. The shell tracks jobs in its `g_jobs[]` array and uses `waitpid(-1, &status, WNOHANG | WUNTRACED)` to reap background processes.

---

## How the X11 Server Communicates with Clients

The X11 server (`userspace/forest-x11/main.c`) is a userspace process that provides windowing services. It uses Unix domain sockets for client communication and Forest-specific syscalls for framebuffer and input access.

### Server Architecture

```
                    +-------------------+
                    |  X11 Application  | (e.g., a window manager or GUI app)
                    +-------------------+
                           |
                    socket(AF_UNIX, SOCK_STREAM)
                    connect("/tmp/.X11-unix/X0")
                           |
                    X11 protocol messages (requests/events)
                           |
                    +-------------------+
                    |   forest-x11     | (the X11 server)
                    |                   |
                    | - Accepts clients via socket
                    | - Maintains window state
                    | - Composites all windows
                    | - Writes to framebuffer
                    | - Reads input events
                    +-------------------+
                           |
                    sys_fb_init(), sys_fb_flush()
                    sys_poll_input(), sys_read_kbd()
                    sys_read_mouse()
                           |
                    +-------------------+
                    |    Fern Kernel    |
                    +-------------------+
                           |
                    Framebuffer memory + PS/2 hardware
```

### The Event Loop

The X11 server runs a single-threaded event loop:

1. **Accept new clients**: `accept()` on the listening socket, send connection setup reply
2. **Read client requests**: Non-blocking `read()` from each client fd, dispatch to handler
3. **Poll input**: Call `sys_poll_input()` to check for keyboard/mouse events, route them to the focused window
4. **Composite**: Draw all mapped windows onto the back buffer in z-order
5. **Flip**: Copy the back buffer to the framebuffer via `memcpy` per scanline
6. **Flush**: Call `sys_fb_flush()` to tell the kernel the frame is ready

### Communication Protocol

The X11 protocol is a binary protocol. Clients send requests (4-byte header + body), and the server replies with 32-byte replies and/or events. For example, creating a window:

```
Client sends:  REQ_CREATE_WINDOW (opcode 1)
               + window ID, parent, x, y, width, height, background pixel

Server replies: 32-byte reply with success status + window geometry

Server also sends: MapNotify event when the window is mapped
                   Expose event telling the client to redraw
```

Mouse and keyboard input are routed as X11 events (MotionNotify, KeyPress, KeyRelease, FocusIn, FocusOut, EnterNotify) to whichever window has focus.

### Framebuffer Access

The X11 server maps the kernel's framebuffer into its address space using two syscalls:

1. `SYS_GET_FB_INFO` (473): Returns width, height, pitch, bpp, format
2. `SYS_MMAP_FB` (471): Maps the framebuffer memory into userspace

Then it writes pixel data directly to that mapped memory and calls `SYS_FB_FLUSH` (478) to tell the kernel to update the display.

---

## Networking End-to-End

Networking in Forest OS spans the full stack: from application syscalls through the kernel's TCP/IP implementation down to the VirtIO NIC driver.

```
Application                libc                Kernel                    Hardware
-----------                ----                ------                    --------
send(fd, buf, len)
  |-> SYS_sendto
       |-> net_send() ---> TCP/IP stack
                             |-> route lookup
                             |-> TCP segment assembly
                             |-> IP packet construction
                             |-> Ethernet frame
                             |-> NIC driver tx()
                                |-> VirtIO virtqueue submit
                                    |-> MMIO doorbell write
                                        |-> Hardware DMA
                                            |-> Network wire
```

### The NIC Driver Layer

Forest OS supports VirtIO NIC and registers NIC drivers via `net_nic_driver_t` with probe, tx, rx, get_mac, and irq_handler callbacks. The VirtIO net driver parses FDT for the MMIO node, initializes via MMIO registers, registers as a NIC, submits TX via virtqueue descriptors, and processes RX via interrupt-driven used ring polling.

### Socket Layer

The kernel maintains socket state per fd. Socket syscalls dispatch to `net_connect()`, `net_send()`, `net_recv()`, etc. Loopback traffic stays in-kernel; real traffic goes through the routing table, gets encapsulated in IP/Ethernet, and is handed to the NIC driver.

### Key Network Syscalls

| Syscall | Kernel Function | What It Does |
|---------|----------------|--------------|
| `socket()` | `net_socket_create()` | Create a socket endpoint |
| `bind()` | `net_bind()` | Bind to a local port |
| `listen()` | `net_listen()` | Start listening for connections |
| `accept()` | `net_accept()` | Accept an incoming connection |
| `connect()` | `net_connect()` | Connect to a remote host |
| `send/recv` | `net_send/recv()` | Send/receive data |
| `setsockopt()` | `net_setsockopt()` | Configure socket options |

---

## Input Flow: Hardware to Desktop

Input starts at the PS/2 hardware and flows through several layers to reach your application.

```
+-------------+     +--------+     +----------+     +---------+     +----------+
| PS/2 Device | --> | Kernel | --> |  Input   | --> |  TTY /  | --> |  Shell / |
| (keyboard / |     | PS/2   |     |  Ring    |     |  X11    |     | Desktop  |
|  mouse)     |     | Driver |     |  Buffer  |     |  Server |     |          |
+-------------+     +--------+     +----------+     +---------+     +----------+
      |                  |               |                |              |
      | IRQ 1 (kbd)      | push event    | poll_input()   | read event   |
      | IRQ 12 (mouse)   | to ring buf   | syscalls       | to app      |
      v                  v               v                v              v
  Hardware sends    PS/2 handler     Kernel buffers   X11 reads     App gets
  scancode bytes    decodes to       events in        events and    keyboard/
  via port I/O      input_event_t    input_ring_t     routes them   mouse data
```

### PS/2 Driver

The PS/2 controller driver (`fern/src/include/ps2_controller.h`) communicates with hardware via port I/O:
- `inportb(PS2_DATA_PORT)` to read scancodes
- `outportb(PS2_DATA_PORT, ...)` to send commands

When a scancode arrives (IRQ 1 for keyboard, IRQ 12 for mouse), the PS/2 driver decodes it into an `input_event_t` (16-byte struct with timestamp, type, code, and value) and pushes it onto an `input_ring_t`.

### Input Ring Buffer

The `input_ring_t` (`fern/src/include/input_ring.h`) is a lock-free circular buffer (256 entries) designed for single-producer (IRQ handler) / multi-consumer access. Events are typed using the Linux evdev convention:
- `EV_KEY` (0x01): key press/release with `KEY_*` codes
- `EV_REL` (0x02): relative movement with `REL_X`, `REL_Y`, `REL_WHEEL`
- `EV_SYN` (0x00): synchronization markers

### Kernel-to-Userspace Delivery

Userspace reads input events via Forest-specific syscalls:
- `SYS_POLL_INPUT` (481): Returns a bitmask indicating which event types are available (bit 0 = keyboard, bit 1 = mouse)
- `SYS_READ_KBD` (479): Pops a keyboard event from the ring buffer
- `SYS_READ_MOUSE` (480): Pops a mouse event from the ring buffer

The X11 server (`userspace/forest-x11/sys.c`) polls these in its event loop, accumulates mouse position deltas, tracks key states, and routes events to the focused window.

---

## Graphics End-to-End

Graphics in Forest OS use a framebuffer-based approach. There's no GPU acceleration -- everything is software-rendered.

```
Application (GUI)           X11 Server              Kernel                Display
----------------           -----------              ------                -------
Draw to window surface
  x11_draw_rect()
  x11_draw_text()
  x11_put_image()
       |
  Client sends X11
  rendering requests
  over Unix socket
       |
       v
  Server receives request
  Draws to window's
  in-memory surface
       |
  composite_all():
  Z-order compositing
  onto back buffer
       |
  flip_to_fb():
  memcpy scanlines to
  framebuffer memory
       |
  sys_fb_flush() ------>  SYS_FB_FLUSH
                           tty_putc() or
                           direct fb update
                                |
                           Framebuffer is
                           memory-mapped at
                           0xF0000000
                                |
                           Hardware scans out
                           framebuffer to display
```

### Framebuffer Setup

During boot, the kernel maps the framebuffer (provided by the bootloader via multiboot) to virtual address `0xF0000000` with `PAGE_CACHE_DISABLE` for correct MMIO behavior. The `kernel_finalize_framebuffer_mapping()` function handles the page-by-page mapping with verification.

### X11 Server Compositing

The X11 server maintains a back buffer (`g_backbuf`) and per-window surfaces. When a client draws, it sends X11 protocol requests (PolyFillRectangle, PutImage, ImageText8, etc.) over the Unix socket. The server renders these to the window's surface. During each frame:

1. Clear the back buffer with the background color
2. Sort windows by z-order
3. For each window: copy its surface pixels to the back buffer at the window's position
4. Convert from the internal pixel format to the framebuffer format (BGRA/RGBA)
5. Copy scanlines from back buffer to the mapped framebuffer memory
6. Call `sys_fb_flush()` to synchronize

### Kernel TTY (Non-X11)

Without the X11 server, the kernel's built-in TTY renders text directly to the framebuffer with ANSI escape codes and an 8x8 bitmap font. This is what you see during boot and on the console.

---

## USB Device Discovery and Communication

USB support in Forest OS handles device enumeration and class-specific drivers.

USB controllers are discovered during PCI enumeration (class 0x0C). The kernel dispatches to the right driver based on the programming interface: UHCI (`0x00`), OHCI (`0x10`), EHCI (`0x20`), or xHCI (`0x30`).

When a device is plugged in, the USB core resets the port, reads the device descriptor, assigns an address, reads configuration/interface descriptors, and matches the interface class to a registered driver (HID, mass storage, hub). The xHCI/EHCI drivers communicate via MMIO and set up DMA transfer rings for data movement.

USB HID devices (keyboards, mice) push `input_event_t` events into the same `input_ring_t` buffer as PS/2 devices, so the X11 server and shell handle USB input identically.

---

## Device Driver Communication Patterns

Forest OS drivers use three main hardware communication methods:

**Port I/O** (x86): The simplest method, used for PS/2, serial ports, VGA. `inportb()` / `outportb()` read/write to I/O ports. Example: `inportb(0x60)` reads a PS/2 scancode.

**MMIO** (Memory-Mapped I/O): Used by VirtIO, USB controllers, and modern PCI devices. Device registers are mapped into physical memory; the kernel maps them to virtual addresses and reads/writes directly. Example: VirtIO net driver reads MMIO registers via `mmio_read32()`.

**DMA** (Direct Memory Access): Used by VirtIO NIC, USB, and storage. The kernel places descriptor rings in DMA-capable memory, writes the physical address to the device, and the device transfers data independently.

**PCI Config Space**: All PCI devices have a config space accessed via I/O ports `0xCF8` (address) and `0xCFC` (data). Used during enumeration to read vendor IDs, BARs, and IRQ lines.

---

## Interrupt Handling: From Hardware to Userspace

Interrupts are how hardware gets the kernel's attention.

```
Hardware Signal
      |
      v
CPU receives IRQ, saves registers, switches to ring 0
      |
      v
Assembly stub (interrupt_stubs.s) pushes registers, calls C handler
      |
      v
interrupt_common_handler(): look up handler in irq_desc[], call it, send EOI
      |
      v
Driver handler: read hardware, process data, queue events (ring buffer / net buffer)
      |
      v
Task returns to userspace (IRET), syscall returns to application
```

### IDT Setup

The kernel sets up 256 IDT entries during boot. Vectors 0-31 are CPU exceptions (page fault, etc.). Vector 32+ are hardware IRQs (after PIC remap). Vector 128 is the syscall entry point. Each entry points to an assembly stub that saves registers and calls `interrupt_common_handler()`.

### IRQ Registration

Drivers call `request_irq(irq, handler, flags, name, dev_id)` to register handlers. The kernel maintains a linked list of `irq_action` structs per IRQ line, supporting shared interrupts. When an IRQ fires, all registered handlers for that line are called in sequence, and the kernel sends an EOI to the interrupt controller.

### Timer Interrupt

The timer interrupt (IRQ 0, vector 32) is special -- it drives the entire scheduling system. On each tick, the kernel decrements task sleep counters, checks for preemption, and reschedules if needed.

---

## Summary

Here's how all the pieces fit together for a typical interaction -- you type `ls` in the shell:

1. **Keyboard hardware** sends scancode via port I/O
2. **PS/2 driver** IRQ handler decodes scancode, pushes `input_event_t` to ring buffer
3. **Kernel** wakes the shell's blocked `read()` syscall
4. **Shell** receives the character, echoes it to stdout via `write()`
5. **Kernel's TTY** renders the character to the framebuffer
6. **Shell** sees Enter, parses the command `ls`
7. **Shell** calls `fork()` -> `execve("/bin/ls")`
8. **Kernel** loads the ELF binary, creates the new process
9. **`ls`** calls `opendir()`, `readdir()`, `write()` -- all syscalls
10. **Kernel** reads directory entries from the initrd VFS
11. **`ls`** outputs filenames to stdout
12. **Kernel's TTY** renders the output to the framebuffer
13. **`ls`** exits, **shell** calls `waitpid()` to reap it
14. **Shell** displays a new prompt

Every step is a clean boundary crossing: hardware -> kernel -> libc -> userspace -> libc -> kernel -> hardware. That's the Forest OS communication model.
