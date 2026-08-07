# Forest OS System Tools

Forest OS ships a set of userspace system tools that provide core functionality for process management, system control, information queries, and authentication. These tools are built on Forest OS libc and interact with the kernel through standard POSIX interfaces and custom Forest syscalls.

## Table of Contents

1. [Process Management](#1-process-management)
2. [System Initialization](#2-system-initialization)
3. [System Shutdown & Reboot](#3-system-shutdown--reboot)
4. [System Information](#4-system-information)
5. [Authentication](#5-authentication)
6. [Sleep & Timing](#6-sleep--timing)
7. [Kernel Syscall Interactions](#7-kernel-syscall-interactions)
8. [Implementation Highlights](#8-implementation-highlights)
9. [Usage Examples](#9-usage-examples)
10. [POSIX Compatibility Notes](#10-posix-compatibility-notes)
11. [Quick-Reference Table](#11-quick-reference-table)

---

## 1. Process Management

### ps — Process Status

**Source:** `userspace/ps/ps.c`

`ps` displays running processes by reading the kernel task list via the custom `SYS_GET_TASKS` syscall (syscall 528). It mirrors the kernel's `task_info_t` structure directly, providing real-time process data without scanning `/proc`.

**Key features:**
- Multiple display modes: default, BSD `aux`, full (`-f`), long (`-l`), jobs (`-j`), custom (`-o`), and forest/tree (`--forest`)
- Sorting by PID, PPID, name, CPU usage, memory, priority, or state
- Filtering by PID, TTY, user, or group
- Custom format fields: pid, ppid, pgid, uid, user, stat, time, comm, args, %cpu, %mem, vsz, rss, nice, pri, sz, tty, start, f, c, stime
- CPU/memory percentage calculations based on elapsed ticks and total system memory

**Process states:**

| State | Char | Description |
|-------|------|-------------|
| STATE_RUNNING | R | Currently executing |
| STATE_READY | R | Ready to run |
| STATE_WAITING | S | Sleeping/waiting |
| STATE_TERMINATED | X | Dead |
| STATE_ZOMBIE | Z | Zombie (awaiting reaping) |
| STATE_SUSPENDED | T | Suspended/stopped |

**Architecture:**
The tool defines its own `task_info_t` struct that mirrors the kernel's definition at `fern/src/include/task.h:294-305`. Data is fetched via `get_tasks()`, an external libc wrapper for `SYS_GET_TASKS`. This approach avoids the overhead of parsing procfs and gives direct access to kernel scheduler data.

### kill — Send Signals

**Source:** `userspace/kill/kill.c`

`kill` sends signals to processes using the standard `kill()` libc function. It supports the full POSIX signal set (SIGHUP through SIGSYS, 31 signals).

**Features:**
- Signal selection by name (`-s TERM`), number (`-9`), or long form (`--signal=TERM`)
- Default signal: SIGTERM
- Signal listing (`-l`) with bidirectional name/number translation
- Dry-run mode (`-p`) that prints what would be sent
- Proper error handling for ESRCH (no such process), EPERM (permission denied), EINVAL (invalid signal)

**Signal table:**

| # | Name | # | Name | # | Name |
|---|------|---|------|---|------|
| 1 | HUP | 12 | URG | 23 | IO |
| 2 | INT | 13 | PIPE | 24 | PWR |
| 3 | QUIT | 14 | ALRM | 25 | SYS |
| 4 | ILL | 15 | TERM | 26 | VTALRM |
| 5 | TRAP | 16 | STKFLT | 27 | PROF |
| 6 | ABRT | 17 | CHLD | 28 | WINCH |
| 7 | BUS | 18 | CONT | 29 | USR1 |
| 8 | FPE | 19 | STOP | 30 | USR2 |
| 9 | KILL | 20 | TSTP | 31 | — |
| 10 | SEGV | 21 | TTIN | — | — |
| 11 | USR1 | 22 | TTOU | — | — |

---

## 2. System Initialization

### init — PID 1 Process

**Source:** `userspace/init/init.c`

`init` is the first userspace process (PID 1). It handles system startup, zombie reaping, and graceful shutdown.

**Responsibilities:**

1. **Mount essential filesystems** — `/proc` (procfs), `/dev` (devtmpfs), `/sys` (sysfs), `/tmp`
2. **Set up environment** — PATH, HOME, TERM, SHELL, USER, LOGNAME, HOSTNAME
3. **Parse `/etc/inittab`** — Supports `respawn` and `once` actions (SysV-style)
4. **Spawn services** — Fork/exec child processes with optional respawn
5. **Reap zombies** — SIGCHLD handler calls `waitpid(-1, NULL, WNOHANG)` in a loop
6. **Handle shutdown** — SIGTERM/SIGINT triggers halt; SIGUSR1 triggers reboot

**inittab format:**
```
# id:runlevels:action:process
tty1:3:respawn:/bin/sh
console:3:once:/bin/init-helper
```

**Shutdown sequence:**
1. Send SIGTERM to all children
2. Wait 2 seconds for graceful exit
3. Send SIGKILL to stragglers
4. Call `sync()` to flush filesystem buffers
5. Call `reboot()` with appropriate flag (RB_POWER_OFF or RB_AUTOBOOT)

If the `reboot()` syscall fails, init enters an infinite `pause()` loop to prevent kernel panic from an unhandled PID 1 exit.

---

## 3. System Shutdown & Reboot

### shutdown — Scheduled Power Control

**Source:** `userspace/shutdown/shutdown.c`

`shutdown` provides scheduled and immediate system shutdown with user warnings, support for halt/reboot/poweroff, and a cancel mechanism.

**Time formats:**
- `now` — Immediate
- `+MINUTES` — Delay by N minutes
- `HH:MM` — Shutdown at specific time (next occurrence)

**Options:**
- `-P` / `--poweroff` — Power off (default)
- `-h` / `--halt` — Halt the system
- `-r` / `--reboot` — Reboot
- `-c` — Cancel a pending shutdown
- `-k` — Dry run (warn users but don't shut down)
- `-f` — Fast mode (skip shutdown scripts and warnings)
- `-F` — Force mode (skip process killing)

**Shutdown sequence:**
1. Write PID file to `/var/run/shutdown.pid` (for cancel support)
2. Log to wtmp (`/var/log/wtmp`)
3. Broadcast warning messages to `/dev/console` and `/dev/tty1-8`
4. Warn at intervals (every 60 seconds until shutdown)
5. Run scripts in `/etc/shutdown.d/` (executable files receive action as argument)
6. SIGTERM all processes, wait 5 seconds, SIGKILL remaining
7. Call `sync()` then `reboot()` with appropriate flag

**Cancel mechanism:** Sends SIGUSR1 to the shutdown process via its PID file.

**wtmp logging:** Writes `struct utmp` records compatible with Linux utmp format (ut_type, ut_pid, ut_line, ut_user, ut_time).

### reboot — Immediate Reboot

**Source:** `userspace/reboot/reboot.c`

`reboot` performs immediate system reboot, halt, or poweroff. Requires root privileges.

**Options:**
- `-p` — Power off
- `-h` — Halt
- `-f` — Fast (skip `sync()`)
- `-w` — Dry run (write wtmp record only)
- `-b` — Boot immediately

**Key behavior:** Always writes a `BOOT_TIME` record to `/var/log/wtmp` before rebooting, maintaining system accounting history across reboots.

---

## 4. System Information

### hostname — System Name

**Source:** `userspace/hostname/hostname.c`

`hostname` gets or sets the system hostname.

**Features:**
- Set hostname: `hostname new-name` (calls `sethostname()` syscall)
- Short name (`-s`), domain (`-d`), FQDN (`-f`), IP addresses (`-i`), aliases (`-a`), NIS domain (`-y`)
- DNS resolution via `getaddrinfo()`/`getnameinfo()` for FQDN and domain lookups
- Falls back to `uname()` if DNS resolution fails

**Syscalls:** `uname()`, `sethostname()`, `getaddrinfo()`, `getnameinfo()`

### uname — System Information

**Source:** `userspace/uname/uname.c`

`uname` prints system identification information from the `utsname` struct.

**Fields:**
- `-s` — Kernel name (e.g., "Forest")
- `-n` — Network node hostname
- `-r` — Kernel release
- `-v` — Kernel version
- `-m` — Machine hardware name
- `-p` — Processor type (prints machine name)
- `-i` — Hardware platform (prints "unknown")
- `-o` — Operating system (prints "GNU/Linux")
- `-a` — All fields

**Default:** `-s` only. With no flags, prints just the kernel name.

### date — Date & Time

**Source:** `userspace/date/date.c`

`date` prints or sets the system date and time.

**Features:**
- Custom format strings via `strftime()` (`+FORMAT`)
- Set time: `-s STRING` (supports `%Y-%m-%d %H:%M:%S`, ISO 8601, time-only)
- Display parsed time: `-d STRING`
- Read time from file: `-f FILE`
- RFC 2822 output: `-R`
- ISO 8601 output: `--iso-8601[=FMT]` (basic, seconds, minutes, date)
- UTC mode: `-u`

**Time setting:** Uses `settimeofday()` syscall. Requires root privileges.

**Timezone handling:** Uses `__tm_gmtoff` extension for timezone offset in RFC 2822 and ISO 8601 output. Falls back to UTC if unavailable.

### id — User/Group Identity

**Source:** `userspace/id/id.c`

`id` prints real and effective user/group IDs.

**Options:**
- `-u` — User ID only
- `-g` — Group ID only
- `-G` — All supplementary group IDs
- `-n` — Print names instead of numbers
- `-r` — Print real (not effective) IDs
- `-z` — Delimit with NUL instead of newline
- `--context` — Print security context (returns "unconfined" on Forest OS)

**Syscalls:** `getuid()`, `geteuid()`, `getgid()`, `getegid()`, `getgroups()`

**Default output:** `uid=0(root) gid=0(root) groups=0(root),1(bin),2(daemon)`

---

## 5. Authentication

### su — Substitute User

**Source:** `userspace/su/su.c`

`su` switches user identity, optionally starting a login shell.

**Features:**
- Default target: root
- Login shell (`-l` or `-`): Sets HOME, USER, LOGNAME, SHELL, PATH; changes to user's home directory
- Command execution (`-c CMD`): Runs command as target user
- Group control (`-g GROUP`, `-G GROUPS`): Set primary/supplementary groups
- Environment preservation (`-m`/`-p`): Don't reset environment
- Custom shell (`-s SHELL`)
- Password authentication via `/etc/shadow` (format: `username:salt:hash:uid:gid:mask`)
- Terminal echo disable for password input
- Verbose mode (`-V`) for debugging

**Authentication flow:**
1. Look up target user via `getpwnam()` or `getpwuid()`
2. If already the target user, skip authentication
3. Prompt for password with echo disabled (via `termios`)
4. Verify against `/etc/shadow`
5. On success: `setgroups()`, `setgid()`, `setuid()`, set up environment, exec shell

**Security:** Password is cleared from memory after verification (`memset`).

### sudo — Superuser Do

**Source:** `userspace/sudo/sudo.c`

`sudo` executes commands as root or another user with credential caching.

**Features:**
- Run as different user (`-u user`) and group (`-g group`)
- Login shell (`-i`), shell (`-s`), or command execution
- Sudoers file parsing (`/etc/sudoers`, `/etc/sudoers.d/`)
- Timestamp-based credential caching (600-second TTL) in `/var/run/sudo/`
- Privilege listing (`-l`)
- Credential validation (`-v`)
- Non-interactive mode (`-n`), stdin password (`-S`)
- Command logging to `/var/log/sudo.log`
- Signal forwarding to child process

**Sudoers format:**
```
user:host:(runas) command
ALL:ALL:(ALL) ALL
```

**Authentication flow:**
1. Check if timestamp exists and is fresh (< 600 seconds)
2. If expired: prompt for password, check sudoers rules
3. If allowed: create timestamp, fork child, drop privileges via `setgid()`/`setuid()`, exec command
4. Parent waits for child, forwarding signals as needed

**Logging:** Each sudo invocation is logged with user, timestamp, TTY, working directory, command, and success/failure.

---

## 6. Sleep & Timing

### sleep — Delay Execution

**Source:** `userspace/sleep/sleep.c`

`sleep` pauses execution for a specified duration.

**Features:**
- Floating-point precision (e.g., `sleep 0.5`)
- Suffixes: `s` (seconds), `m` (minutes), `h` (hours), `d` (days)
- Multiple arguments are summed: `sleep 1m 30s` = 90 seconds
- Signal-aware: Recursively handles EINTR from `nanosleep()` to sleep the remaining time

**Syscalls:** `nanosleep()` (via `clock_nanosleep` or equivalent)

**Signal handling:** SIGINT and SIGTERM set a flag; if interrupted, the remaining time from `struct timespec rem` is recursively slept.

---

## 7. Kernel Syscall Interactions

Forest OS tools interact with the kernel through the following interfaces:

| Tool | Syscall/Interface | Purpose |
|------|-------------------|---------|
| ps | `SYS_GET_TASKS` (528) | Read kernel task list |
| kill | `kill()` | Send signals to processes |
| init | `fork()`, `exec()`, `waitpid()`, `reboot()`, `mount()`, `setsid()` | Process management and system control |
| shutdown | `reboot()`, `kill(-1, ...)`, `sync()` | System power control |
| reboot | `reboot()`, `sync()` | Immediate reboot/halt/poweroff |
| hostname | `uname()`, `sethostname()`, `getaddrinfo()` | Network identification |
| uname | `uname()` | Kernel information |
| date | `time()`, `settimeofday()`, `localtime()`, `gmtime()`, `strftime()` | Time management |
| id | `getuid()`, `geteuid()`, `getgid()`, `getegid()`, `getgroups()` | Identity queries |
| su | `setuid()`, `setgid()`, `setgroups()`, `exec()`, `tcgetattr()`/`tcsetattr()` | Identity switching |
| sudo | `fork()`, `setuid()`, `setgid()`, `exec()`, `waitpid()` | Privileged execution |
| sleep | `nanosleep()`, `sigaction()` | Timer suspension |

**Forest-specific:** `SYS_GET_TASKS` (syscall 528) is a custom Forest OS syscall that returns the kernel's internal task list directly, bypassing procfs. This is more efficient than parsing `/proc` and provides access to fields like `cpu_ticks_total` and `memory_used_kb` that may not be exposed through procfs.

---

## 8. Implementation Highlights

### Direct Kernel Data Access (ps)
`ps` uses `get_tasks()` (wrapper for `SYS_GET_TASKS`) to read the kernel's `task_info_t` array directly. This avoids procfs parsing overhead and gives access to scheduler-internal fields.

### Graceful Shutdown Sequencing (init, shutdown)
Both `init` and `shutdown` implement a two-phase process termination: SIGTERM first, then SIGKILL after a timeout. This allows processes to clean up resources while ensuring the system eventually halts.

### Signal-Safe Sleep (sleep)
`sleep` uses `nanosleep()` with recursive EINTR handling to ensure precise timing even when interrupted by signals. The remaining time from `struct timespec rem` is propagated through recursion.

### Credential Caching (sudo)
`sudo` caches authentication credentials via timestamp files in `/var/run/sudo/`, with a 600-second TTL. This avoids repeated password prompts during a session.

### Password Memory Safety (su)
`su` clears the password buffer with `memset(password, 0, sizeof(password))` immediately after verification, reducing the window for memory disclosure attacks.

### Custom Output Formatting (ps)
`ps -o` supports 21 different field specifiers, parsed via `strtok_r()` against a field name table. This allows scripts to extract specific process attributes.

### Forest Tree Display (ps)
`ps --forest` renders a tree view of process hierarchies using Unicode box-drawing characters (│, └, ├), recursively walking the parent-child relationships.

---

## 9. Usage Examples

### Process Management
```bash
# List all processes
ps

# BSD-style all-user format
ps aux

# Show process tree
ps --forest

# Custom format: PID, command, CPU%, memory
ps -o pid,comm,%cpu,%mem

# Sort by memory usage
ps --sort=mem

# Kill a process with SIGKILL
kill -9 1234

# Send SIGHUP to reload config
kill -HUP $(cat /var/run/nginx.pid)
```

### System Initialization
```bash
# init runs as PID 1 automatically at boot
# It mounts filesystems and starts services from /etc/inittab

# Trigger reboot from init (sends SIGUSR1 to PID 1)
kill -USR1 1
```

### Shutdown & Reboot
```bash
# Power off immediately
shutdown now

# Reboot in 5 minutes with a message
shutdown -r +5 "Kernel upgrade in progress"

# Schedule halt at 23:00
shutdown -h 23:00 "Nightly maintenance"

# Cancel a pending shutdown
shutdown -c

# Immediate reboot
reboot

# Power off
reboot -p
```

### System Information
```bash
# Print hostname
hostname

# Set hostname
hostname forest-node-01

# Print all system info
uname -a

# Print kernel release
uname -r

# Print current date
date

# Set date
date -s "2026-08-07 12:00:00"

# Print ISO 8601 format
date --iso-8601=seconds

# Print current user identity
id

# Print just the user name
id -un
```

### Authentication
```bash
# Switch to root with login shell
su -

# Run a command as another user
su -c "ls /home/otheruser" otheruser

# Run a command as root via sudo
sudo ls /root

# Run a command as a different user
sudo -u postgres psql

# List available privileges
sudo -l

# Validate credentials
sudo -v
```

### Sleep
```bash
# Sleep for 5 seconds
sleep 5

# Sleep for 2.5 seconds
sleep 2.5

# Sleep for 1 minute 30 seconds
sleep 1m 30s

# Sleep for 1 day
sleep 1d

# Sleep for 2 hours
sleep 2h
```

---

## 10. POSIX Compatibility Notes

| Tool | POSIX Status | Notes |
|------|-------------|-------|
| ps | Non-standard | Uses Forest-specific `SYS_GET_TASKS` instead of procfs. Output format mimics BSD/Linux `ps` but field availability may differ. |
| kill | Mostly POSIX | Supports POSIX signal names and `-l` listing. Adds `--signal=` long option. |
| init | SysV-like | Parses `/etc/inittab` (SysV style). Does not implement runlevel switching beyond runlevel 3. |
| shutdown | Linux-compatible | Supports Linux-style time formats and options (`-h`, `-r`, `-P`, `-c`, `-k`, `-f`, `-F`). |
| reboot | Linux-compatible | Supports Linux `RB_*` reboot constants. |
| hostname | Mostly POSIX | Supports standard `-s`, `-d`, `-f` options. DNS resolution depends on system configuration. |
| uname | POSIX-compliant | Fully supports POSIX `uname` interface. `-p` and `-i` have limited data. |
| date | GNU-like | Supports GNU `date` options (`-s`, `-d`, `-f`, `-R`, `--iso-8601`). `strptime()` is not POSIX but widely available. |
| id | POSIX-compliant | Fully supports POSIX `id` interface. `--context` is a SELinux extension (returns "unconfined"). |
| su | Mostly POSIX | Supports standard `-`, `-l`, `-c` options. Shadow file format is Forest-specific. |
| sudo | Linux-compatible | Sudoers format compatible with Linux `sudo`. Timestamp caching is simplified. |
| sleep | GNU-like | Supports floating-point and suffixes (`s`, `m`, `h`, `d`). POSIX only requires integer seconds. |

**Forest OS libc notes:**
- `sig_atomic_t` is manually defined (Forest OS libc may lack it)
- `__tm_gmtoff` is used as a GNU extension for timezone offset
- `strptime()` is available as a non-POSIX extension
- `NGROUPS_MAX` defaults to 32 if not defined
- Exit codes: `EXIT_OK` (0), `EXIT_USAGE` (1), `EXIT_FAIL` (2)

---

## 11. Quick-Reference Table

| Tool | Command | Key Options | Syscall | Required Privs |
|------|---------|-------------|---------|----------------|
| **ps** | `ps [aux\|-f\|-l\|--forest\|-o FMT]` | `--sort`, `-p PID`, `-e` | `SYS_GET_TASKS` (528) | None |
| **kill** | `kill [-s SIG\|-SIG] PID...` | `-l` (list), `-p` (dry run) | `kill()` | Same user or root |
| **init** | Runs as PID 1 at boot | Parses `/etc/inittab` | `fork()`, `exec()`, `reboot()`, `mount()` | PID 1 (root) |
| **shutdown** | `shutdown [-h\|-r\|-P] TIME [MSG]` | `-c` (cancel), `-k` (dry run), `-f` (fast) | `reboot()`, `kill()` | Root |
| **reboot** | `reboot [-p\|-h\|-f\|-w]` | `-p` (poweroff), `-w` (wtmp only) | `reboot()`, `sync()` | Root |
| **hostname** | `hostname [NAME] [-s\|-d\|-f\|-i]` | `-s` (short), `-f` (FQDN) | `uname()`, `sethostname()` | Root to set |
| **uname** | `uname [-snrvmpioa]` | `-a` (all) | `uname()` | None |
| **date** | `date [-s STR\|-d STR] [+FMT]` | `-R` (RFC 2822), `-u` (UTC), `--iso-8601` | `time()`, `settimeofday()` | Root to set |
| **sleep** | `sleep NUMBER[SUFFIX]...` | `s`, `m`, `h`, `d` suffixes | `nanosleep()` | None |
| **id** | `id [-gGnruz] [--context]` | `-u` (user), `-G` (groups) | `getuid()`, `geteuid()`, `getgroups()` | None |
| **su** | `su [-] [-c CMD] [-s SHELL] [USER]` | `-l` (login), `-g` (group) | `setuid()`, `setgid()`, `exec()` | Root or target password |
| **sudo** | `sudo [-u USER] [-g GRP] CMD` | `-l` (list), `-k` (reset cache), `-v` (validate) | `fork()`, `setuid()`, `exec()` | Root or sudoers entry |
