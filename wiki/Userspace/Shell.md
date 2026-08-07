# Forest Shell

Forest Shell (`forest-shell`) is the primary interactive shell for Forest OS. It's a POSIX-compatible command-line interpreter written in C, designed to feel familiar to anyone who has used bash or sh. It lives at `/bin/forest-shell` and handles everything from running simple commands to complex pipelines, job control, and scripting.

---

## Built-in Commands

Forest Shell includes **19 core builtins** plus additional system builtins. Builtins run inside the shell process itself (no `fork`/`exec`), so things like `cd` and `export` affect the shell's own state. Type `help` at any time to see the full list.

### Navigation & Filesystem

| Command | Description | Example |
|---------|-------------|---------|
| `cd` | Change directory | `cd /home/user`, `cd -`, `cd ~` |
| `pwd` | Print working directory | `pwd` |

`cd` supports: `cd` or `cd ~` (go to `$HOME`), and `cd -` (go back to previous directory, printing the path).

### Information & Output

| Command | Description | Example |
|---------|-------------|---------|
| `echo` | Print arguments | `echo "Hello World"` |
| `type` | Show how a command is interpreted | `type cd`, `type ls` |
| `which` | Show command path | `which bash` |
| `history` | Show command history | `history` |
| `env` | Print environment variables | `env` |
| `set` | Print all shell variables | `set` |
| `dmesg` | Show kernel messages | `dmesg` |

`echo` supports flags: `-n` (suppress trailing newline), `-e` (enable escape sequences like `\t`, `\n`, `\a`, `\b`, `\e`, `\f`, `\r`, `\v`, `\\`, `\0nnn` octal), `-E` (disable escape processing).

### Environment & Aliases

| Command | Description | Example |
|---------|-------------|---------|
| `export` | Set or export environment variable | `export PATH=/bin:/usr/bin` |
| `unset` | Remove environment variable | `unset TEMP_VAR` |
| `alias` | Define or list aliases | `alias ll='ls -la'` |
| `unalias` | Remove an alias | `unalias ll` |

### Job Control

| Command | Description | Example |
|---------|-------------|---------|
| `jobs` | List active jobs | `jobs` |
| `fg` | Bring job to foreground | `fg %1` |
| `bg` | Resume job in background | `bg %1` |
| `wait` | Wait for all background jobs | `wait` |

### Scripting & Configuration

| Command | Description | Example |
|---------|-------------|---------|
| `source` / `.` | Execute commands from a file | `source ~/.profile` |
| `exit` | Exit the shell | `exit 0` |

### System Builtins

| Command | Description | Example |
|---------|-------------|---------|
| `mount` | Mount a filesystem | `mount /dev/sda1 /mnt` |
| `umount` | Unmount a filesystem | `umount /mnt` |
| `hostname` | Show or set hostname | `hostname forest-pc` |
| `reboot` | Reboot the system | `reboot` |
| `halt` | Halt the system | `halt` |
| `poweroff` | Power off the system | `poweroff` |
| `login` | Re-authenticate user | `login alice` |
| `clear` | Clear terminal screen | `clear` |

These system builtins try to execute an external command of the same name via `fork`/`exec`. If none exists, some fall back to built-in behavior (e.g., `dmesg` reads `/dev/kmsg` directly).

---

## Piping and I/O Redirection

Forest Shell supports the full set of POSIX I/O operators. Commands in a pipeline run as separate processes connected by pipes.

### Standard Piping

Chain commands together with `|`. Each command's stdout becomes the next command's stdin:

```bash
cat /proc/mounts | grep ext4
ls -la | wc -l
dmesg | grep -i error | head -20
```

### Redirection Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `>` | Write stdout to file (truncate) | `echo hello > file.txt` |
| `>>` | Append stdout to file | `echo line >> log.txt` |
| `<` | Read stdin from file | `sort < names.txt` |
| `2>` | Redirect stderr to file | `cmd 2> errors.log` |
| `2>>` | Append stderr to file | `cmd 2>> errors.log` |
| `&>` | Redirect both stdout and stderr | `cmd &> output.log` |

```bash
find / -name "*.conf" &> results.txt   # Save everything
ls /nonexistent 2> /dev/null           # Suppress errors
```

Internally, the shell parses these operators during tokenization and sets up `dup2()` calls in the child process to wire file descriptors before `exec`.

---

## Job Control

Forest Shell has full job control support. You can run commands in the background, suspend them, and bring them back.

### Running Background Jobs

Append `&` to any command to run it in the background:

```bash
sleep 100 &
long_running_task &
```

The shell prints a job ID and process ID:
```
[1] 1234
[2] 1235
```

### Managing Jobs

```bash
jobs          # List all active jobs with their status
fg %1         # Bring job 1 to the foreground
bg %1         # Resume job 1 in the background
```

### Suspending Jobs

Press **Ctrl+Z** to suspend the current foreground job:
```
[3]  Stopped  vim document.txt
```

Then use `fg %3` to resume it in the foreground, or `bg %3` to continue it in the background.

### How It Works

The shell manages jobs using process groups (`setpgid`, `tcsetpgrp`). When you run a foreground command, the shell:

1. Forks a child and creates a new process group for it
2. Gives the child terminal control via `tcsetpgrp()`
3. Waits for the child to finish or stop
4. Reclaims terminal control when done

Background jobs run in their own process groups without terminal control. The shell reaps finished children automatically via the `SIGCHLD` handler.

---

## Environment Variables

Environment variables store configuration that child processes inherit.

### Setting Variables

```bash
export MY_VAR="hello world"     # Set and export
export PATH="/bin:/usr/bin"      # Modify PATH
```

### Viewing Variables

```bash
echo $MY_VAR                     # Print a variable
echo ${MY_VAR}                   # Same, with braces
env                              # Show all exported variables
set                              # Show all shell variables
```

### Special Variables

| Variable | Meaning | Example |
|----------|---------|---------|
| `$?` | Exit status of last command | `echo $?` |
| `$$` | Shell's process ID | `echo $$` |
| `$#` | Number of arguments | `echo $#` |
| `$0` | Shell name (`forest-shell`) | `echo $0` |
| `$@` / `$*` | All arguments | `echo $@` |
| `$!` | Last background PID | `echo $!` |
| `$HOME` | Home directory | `cd ~` |
| `$USER` | Current username | `whoami` |
| `$PATH` | Command search path | `which ls` |
| `$SHELL` | Shell path | `echo $SHELL` |
| `$PWD` | Current directory | `echo $PWD` |
| `$OLDPWD` | Previous directory | `cd -` |

### Variable Expansion Syntax

```bash
echo $VAR            # Simple expansion
echo ${VAR}          # Braced expansion
echo "Hello $USER"   # Expansion inside double quotes
echo 'Hello $USER'   # NO expansion inside single quotes
```

Shell initialization sets defaults: `HOME=/home/root`, `SHELL=/bin/forest-shell`, `TERM=forest`, `USER=root`.

---

## Aliases

Aliases let you create shorthand for longer commands. They're expanded when a command is parsed, before execution.

```bash
alias ll='ls -la'           # Create an alias
alias grep='grep --color=auto'
alias                        # Show all aliases
alias ll                     # Show a specific alias
unalias ll                   # Remove an alias
```

Aliases are expanded **before** variable expansion and command substitution. Single quotes in definitions prevent premature expansion:

```bash
alias today='echo The date is `date`'  # `date` runs when alias is used
```

---

## Command History

Forest Shell remembers the last 100 commands you've typed. History is session-based (not persisted to disk).

```bash
history                      # Show all commands with numbers
```

Output:
```
   1  ls -la
   2  cd /tmp
   3  cat readme.txt
```

### Navigating History

Use the **arrow keys** to browse through previous commands:

- **Up Arrow** — previous command
- **Down Arrow** — next command
- **Left/Right Arrow** — move cursor within the current line

Duplicate consecutive commands are not stored. When the buffer is full, the oldest entry is dropped.

---

## Globbing (Wildcard Expansion)

Forest Shell expands wildcard patterns in arguments before executing commands automatically.

### Supported Patterns

| Pattern | Matches | Example |
|---------|---------|---------|
| `*` | Any characters | `*.txt` matches all `.txt` files |
| `?` | Exactly one character | `file?.txt` matches `file1.txt` |
| `[abc]` | Any character in the set | `[aeiou]` matches vowels |
| `[a-z]` | Any character in the range | `[0-9]` matches digits |
| `[!abc]` or `[^abc]` | Any character NOT in the set | `[!0-9]` matches non-digits |

```bash
ls *.c              # List all C source files
cat file?.txt       # Match file1.txt, fileA.txt, etc.
rm temp[0-9].log    # Remove temp0.log through temp9.log
```

The glob engine (`expand_glob` at `shell.c:856`) scans the directory for each argument containing wildcard characters. Results are sorted alphabetically. Hidden files (starting with `.`) are only matched if the pattern itself starts with `.`. If no matches are found, the original pattern is passed through unchanged.

---

## Command Substitution

Command substitution lets you embed the output of a command inside another command using backtick syntax:

```bash
echo "Today is `date`"
echo "Files: `ls | wc -l`"
echo "You have `find . -name '*.c' | wc -l` C files"
```

When the shell encounters backticks (`` ` ``), it:

1. Extracts the command between the backticks
2. Forks a child process to run it
3. Captures the child's stdout via a pipe
4. Strips trailing newlines
5. Substitutes the result into the original command

Command substitution happens **after** tokenization but **before** variable expansion and globbing. Note: Forest Shell uses backtick syntax (`` `cmd` ``), not `$(cmd)` syntax.

---

## Line Editing

Forest Shell provides a built-in line editor for interactive use. No external library (like readline) is required.

| Key | Action |
|-----|--------|
| Any character | Insert at cursor |
| Backspace / Delete | Remove character before cursor |
| Left Arrow | Move cursor left |
| Right Arrow | Move cursor right |
| Up Arrow | Previous command in history |
| Down Arrow | Next command in history |
| Enter | Execute command |
| Ctrl+D | Exit shell (if line is empty) |

The shell reads input one character at a time from stdin, updating an internal line buffer and echoing it back. Escape sequences (arrow keys) are detected by reading the `[` and final byte after the ESC character.

---

## Prompt Customization

The default prompt displays:

```
user@hostname:~/current/dir$
```

- `user@host` is bold green, `path` is bold blue
- The path is abbreviated: inside `$HOME`, it shows as `~/relative/path`

| Component | Source |
|-----------|--------|
| `user` | `$USER` env var, or `root` if UID is 0 |
| `hostname` | `$HOSTNAME` env var, or `gethostname()` |
| `path` | Current directory, with `$HOME` replaced by `~` |

The prompt is generated in `print_prompt()` at `shell.c:169`. It's hardcoded — not configurable via a `PS1` variable.

---

## Signal Handling

Forest Shell carefully manages Unix signals for proper job control.

| Signal | Default Action | Shell Behavior |
|--------|---------------|----------------|
| `SIGINT` (Ctrl+C) | Terminate | Caught; sets flag, doesn't kill shell |
| `SIGQUIT` (Ctrl+\) | Core dump | Ignored in shell |
| `SIGTSTP` (Ctrl+Z) | Stop | Ignored in shell; children handle it |
| `SIGCHLD` | Child exit | Caught; triggers `job_reap()` to clean up |
| `SIGTTIN` / `SIGTTOU` | Stop (bg I/O) | Ignored |

1. The **shell process** ignores `SIGINT`, `SIGQUIT`, and `SIGTSTP` so Ctrl+C doesn't kill it.
2. **Child processes** restore default handlers (`SIG_DFL`) after forking, so Ctrl+C properly terminates the running command.
3. **`SIGCHLD`** triggers `job_reap()`, which calls `waitpid()` with `WNOHANG | WUNTRACED | WCONTINUED` to non-blockingly update the job table.
4. **Terminal control** is managed via `tcsetpgrp()`. The shell gives the terminal to foreground commands and reclaims it when they finish.

---

## How the Shell Interacts with the Kernel

Forest Shell communicates with the Forest OS kernel through standard POSIX interfaces:

| Category | System Calls |
|----------|-------------|
| Process management | `fork`, `execv`, `execl`, `waitpid`, `setpgid`, `getpgrp` |
| File I/O | `open`, `read`, `write`, `close`, `dup2`, `pipe` |
| Directory | `opendir`, `readdir`, `closedir`, `getcwd`, `chdir` |
| Environment | `getenv`, `setenv`, `unsetenv` |
| Signals | `sigaction`, `signal`, `kill` |
| Terminal | `tcsetpgrp`, `isatty`, `gethostname` |
| System info | `uname`, `sync` |

Key kernel interfaces:

- **`/proc` filesystem**: `mount` reads `/proc/mounts`, `dmesg` reads `/dev/kmsg` or `/proc/kmsg`, and the shell uses `/proc/self/exe` to find its own directory.
- **Process groups**: `setpgid()` groups pipeline members; `tcsetpgrp()` manages terminal ownership for job control.
- **Signals**: `SA_RESTART` ensures system calls restart after signal delivery.
- **Environment**: The environment block (`environ`) is inherited by child processes via `execv()`.

---

## Shell Configuration

### Initialization

When the shell starts, `shell_init()` (`shell.c:2355`):

1. Detects whether stdin is a terminal (`isatty()`)
2. Installs signal handlers
3. Sets up the shell's own process group
4. Detects if running on Forest OS (via `uname()`)
5. Prepends its own directory to `PATH` so it can find Forest OS binaries
6. Sets default environment variables
7. Changes to the home directory

### Default PATH

```
/path/to/shell/dir:/bin:/usr/bin:/usr/local/bin:/sbin:/usr/sbin
```

On non-Forest systems (for testing), `/sbin` and `/usr/sbin` are omitted.

### Running Scripts

```bash
./myscript.sh                  # Run a script file
forest-shell -c "echo hello"   # Execute a single command
forest-shell -s < script.sh    # Read commands from stdin
source ~/.profile              # Source a file (execute in current shell)
. ~/.profile                   # Same as source
```

Lines starting with `#` are comments. The `;` operator separates commands unconditionally; `&&` runs the next command only if the previous one succeeded.

### Exit Codes

The shell tracks exit status in `$?`:

- `0` — success
- `1-125` — command-specific error
- `126` — command not executable
- `127` — command not found
- `128+N` — killed by signal N (e.g., `130` for Ctrl+C)

---

## Limits

| Resource | Limit |
|----------|-------|
| Max line length | 4096 characters |
| Max arguments | 256 per command |
| Max history | 100 entries |
| Max aliases | 128 |
| Max jobs | 64 |
| Max glob matches | 512 |
| Max path length | 1024 characters |

---

*This page covers forest-shell as found in `userspace/forest-shell/shell.c` (2507 lines). The shell is intentionally self-contained — no external dependencies like readline or ncurses are required.*
