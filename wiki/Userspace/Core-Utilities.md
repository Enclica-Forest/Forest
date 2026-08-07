# Forest OS Core Utilities

Forest OS ships a full set of POSIX-compatible core utilities, each implemented as a standalone C program in `userspace/`. These follow Unix conventions — same flags, behavior, and exit codes — so shell scripts transfer directly from Linux or macOS.

## Quick Reference

| Utility | Purpose | Key Flags |
|---------|---------|-----------|
| `cat` | Concatenate and display files | `-n`, `-b`, `-s`, `-A`, `-v`, `-T`, `-E` |
| `echo` | Display a line of text | `-n`, `-e`, `-E` |
| `ls` | List directory contents | `-l`, `-a`, `-A`, `-1`, `-R`, `-h`, `-S`, `-t`, `-r`, `-i`, `-s` |
| `cp` | Copy files and directories | `-r`, `-f`, `-i`, `-p`, `-v`, `-a`, `-u`, `-l`, `-s` |
| `mv` | Move or rename files | `-f`, `-i`, `-n`, `-v`, `-u` |
| `rm` | Remove files and directories | `-f`, `-i`, `-I`, `-r`, `-v`, `-d` |
| `mkdir` | Create directories | `-m`, `-p`, `-v` |
| `touch` | Create files or update timestamps | `-a`, `-m`, `-c`, `-r`, `-t`, `-d` |
| `chmod` | Change file permissions | `-c`, `-f`, `-v`, `-R` |
| `ln` | Create links | `-s`, `-f`, `-i`, `-n`, `-v`, `-b`, `-T`, `-P` |
| `pwd` | Print working directory | `-L`, `-P` |
| `grep` | Search file contents | `-i`, `-v`, `-n`, `-c`, `-l`, `-L`, `-r`, `-w`, `-x`, `-o`, `-F`, `-E`, `-m` |
| `find` | Search for files | `-name`, `-type`, `-size`, `-mtime`, `-perm`, `-exec`, `-print` |
| `head` | Output the first lines | `-n NUM`, `-c NUM`, `-q`, `-v` |
| `tail` | Output the last lines | `-n NUM`, `-c NUM`, `-f`, `-q`, `-v` |
| `sort` | Sort text lines | `-r`, `-n`, `-h`, `-f`, `-u`, `-t`, `-k`, `-o`, `-m`, `-c`, `-s`, `-V` |
| `wc` | Count lines, words, bytes | `-l`, `-w`, `-c`, `-m`, `-L` |

## 1. Overview

Every utility lives in its own directory under `userspace/` (e.g., `userspace/cat/cat.c`). Most include `"forest.h"` for common helpers like `eprint()`, `eprint2()`, `EXIT_OK`, `EXIT_FAIL`, and `EXIT_USAGE`. Simpler utilities (`echo`, `head`, `tail`, `wc`) use standard libc headers directly.

Key design principles:
- **POSIX compatibility** — flags and behavior match POSIX/GNU specs
- **Self-contained** — each is a single `.c` file with no external deps
- **Graceful degradation** — errors print to stderr, processing continues

## 2. File Operations

### cat (`userspace/cat/cat.c`, 212 lines)

Concatenates files or stdin to stdout with line numbering, blank squeezing, and display modes.

| Flag | Description |
|------|-------------|
| `-n` | Number all lines |
| `-b` | Number non-blank lines only (overrides `-n`) |
| `-s` | Squeeze multiple blank lines into one |
| `-A` | Show non-printing (implies `-v`) |
| `-v` | Show non-printing with `^` and `M-` notation |
| `-T` | Show tabs as `^I` |
| `-E` | Show line endings as `$` |

Processes input character-by-character through `output_char()`. Line numbering uses a `bol` (beginning-of-line) flag. The squeeze logic tracks `last_was_nl` to skip consecutive newlines.

```bash
cat -n myfile.txt                 # with line numbers
cat -s chapter1.txt chapter2.txt  # squeeze blanks, concatenate
cat -A config.ini                 # show hidden characters
```

### cp (`userspace/cp/cp.c`, 494 lines)

Copies files and directory trees with preservation, hard/symlink creation, and update mode.

| Flag | Description |
|------|-------------|
| `-r`, `-R` | Recursive directory copy |
| `-f` | Force: remove dest, no prompt |
| `-i` | Interactive: prompt before overwrite |
| `-p` | Preserve mode, ownership, timestamps |
| `-v` | Verbose: print `source -> dest` |
| `-a` | Archive mode (`-dpR`, implies force) |
| `-u` | Update: copy only newer files |
| `-l` | Hard link instead of copy |
| `-s` | Symbolic link instead of copy |

Core logic in `copy_one()` dispatches by file type: symlinks re-created as symlinks, special files via `mknod()`, directories via recursive `copy_dir()`, regular files via `copy_file_data()` with 64KB buffer. The `-p` flag uses `utimensat()`, `chmod()`, and `chown()`.

```bash
cp -r projects/ backup/projects/       # recursive
cp -p config.ini /etc/app/             # preserve permissions
cp -av src/ /mnt/usb/                  # verbose archive
cp -u *.log /var/log/old/              # only newer
```

### mv (`userspace/mv/mv.c`, 555 lines)

Moves or renames files. Tries `rename()` first; falls back to copy+delete on `EXDEV` (cross-device).

| Flag | Description |
|------|-------------|
| `-f` | Force: no prompt |
| `-i` | Interactive: prompt before overwrite |
| `-n` | No-clobber: skip existing files |
| `-v` | Verbose |
| `-u` | Update: move only if source is newer |

`copy_entry()` handles files, symlinks, special files, and directories. `remove_entry()` recursively deletes the source after copy.

```bash
mv oldname.txt newname.txt          # rename
mv -i *.log /var/log/               # prompt before overwrite
mv -n file.txt existing.txt         # no-clobber
```

### rm (`userspace/rm/rm.c`, 326 lines)

Deletes files and directory trees with safety features.

| Flag | Description |
|------|-------------|
| `-f` | Force: ignore missing, no prompt |
| `-i` | Interactive: prompt every file |
| `-I` | Interactive once: prompt once for batch |
| `-r`, `-R` | Recursive |
| `-v` | Verbose |
| `-d` | Remove empty directories |
| `--preserve-root` | Refuse to operate on `/` (default) |

`rm_entry()` dispatches by type: `unlink()` for files, `rm_dir()` for directories. `rm_dir()` iterates children via `readdir()` then calls `rmdir()`. Write-protected files prompt even without `-i`.

```bash
rm -rf build/                       # force recursive
rm -I *.log                         # prompt once for batch
rm -v -d empty_dir                  # verbose, empty dir only
```

### touch (`userspace/touch/touch.c`, 472 lines)

Creates empty files or updates access/modification timestamps.

| Flag | Description |
|------|-------------|
| `-a` | Change access time only |
| `-m` | Change modification time only |
| `-c` | Do not create if missing |
| `-r FILE` | Use timestamps from reference file |
| `-t STAMP` | `[[CC]YY]MMDDhhmm[.ss]` format |
| `-d STRING` | Human-readable date string |

`-t` supports 8/10/12-digit formats with optional `.ss` seconds. `-d` tries multiple `strptime()` formats. Timestamps set via `utimensat()` (nanosecond precision).

```bash
touch -c nonexistent.txt             # no-create
touch -t 202401151030 file.txt      # set timestamp
touch -r reference.txt target.txt   # copy timestamps
```

### chmod (`userspace/chmod/chmod.c`, 356 lines)

Changes file permissions with octal or symbolic modes.

| Flag | Description |
|------|-------------|
| `-c` | Report only when changes made |
| `-f` | Suppress errors |
| `-v` | Verbose: show old and new modes |
| `-R` | Recurse into directories |

Symbolic modes: `[ugoa]+[-+=][rwxXst]+` (e.g., `u+x`, `a=r`, `g-w,o-rwx`). The special `X` adds execute only for directories or already-executable files. `-R` uses `readdir()` with `DT_DIR` checks.

```bash
chmod 755 script.sh                  # rwxr-xr-x
chmod u+x script.sh                  # add owner execute
chmod -R a+r shared/                 # recursive add read
```

### ln (`userspace/ln/ln.c`, 194 lines)

Creates hard or symbolic links.

| Flag | Description |
|------|-------------|
| `-s` | Symbolic links |
| `-f` | Force: remove existing target |
| `-i` | Interactive: prompt before remove |
| `-n` | Don't dereference symlinks to directories |
| `-v` | Verbose |
| `-b` | Backup existing (append `~`) |
| `-T` | Treat target as file, not directory |

Multiple sources with a directory target creates links using each source's basename.

```bash
ln -s /etc/hosts symlink.txt            # symbolic link
ln -sf new_target.txt link.txt          # force overwrite
```

## 3. Directory Operations

### mkdir (`userspace/mkdir/mkdir.c`, 172 lines)

Creates directories with optional mode and parent creation.

| Flag | Description |
|------|-------------|
| `-m MODE` | Set mode (octal, e.g., `0755`) |
| `-p` | Create parents; no error if exists |
| `-v` | Verbose |

`make_parents()` walks the path creating each component. Default mode is `0755` minus sticky bit. `-m` validates only octal digits.

```bash
mkdir -p /a/b/c/d                     # nested tree
mkdir -m 0700 private/                # restricted permissions
```

## 4. Text Processing

### echo (`userspace/echo/echo.c`, 111 lines)

Writes arguments to stdout separated by spaces.

| Flag | Description |
|------|-------------|
| `-n` | No trailing newline |
| `-e` | Enable escape sequences |
| `-E` | Disable escape sequences (default) |

Escape sequences with `-e`: `\a` (bell), `\b` (backspace), `\e` (escape), `\f` (form feed), `\n`, `\r`, `\t`, `\v`, `\\`, `\0NNN` (octal), `\xHH` (hex).

```bash
echo -n "no newline"
echo -e "line1\nline2\nline3"
echo -e "\x48\x65\x6C\x6C\x6F"    # "Hello"
```

### grep (`userspace/grep/grep.c`, 937 lines)

Searches file contents with a built-in regex engine supporting extended regex syntax.

| Flag | Description |
|------|-------------|
| `-i` | Case-insensitive |
| `-v` | Invert match |
| `-n` | Line numbers |
| `-c` | Match count |
| `-l` / `-L` | Files with/without matches |
| `-r`, `-R` | Recursive directory search |
| `-w` | Whole-word match |
| `-x` | Whole-line match |
| `-o` | Only matching parts |
| `-F` | Fixed strings |
| `-E` | Extended regex |
| `-m NUM` | Max matches |
| `-e PAT` | Specify pattern |
| `-f FILE` | Patterns from file |
| `--include`/`--exclude` | File filtering |

Three matching engines: `match_literal()` (substring), `match_fixed()` (for `-F`), and `match_regex()` (built-in recursive regex supporting `.`, `*`, `+`, `?`, `^`, `$`, `[]`, `()`, `|`, `\`). Supports up to 256 patterns.

```bash
grep -r "TODO" src/
grep -i -n "error" logfile.txt
grep -c "404" access.log
grep -E "^[0-9]{3}-" data.txt
grep --include="*.py" -r "import" .
```

### head (`userspace/head/head.c`, 142 lines)

Outputs the first part of files.

| Flag | Description |
|------|-------------|
| `-n NUM` | First NUM lines (default 10) |
| `-c NUM` | First NUM bytes |
| `-q` | Suppress headers |
| `-v` | Always show headers |

Reads character-by-character, stopping at the target count. Prints `==> filename <==` headers with multiple files.

```bash
head -n 5 file.txt
head -c 100 file.txt
```

### tail (`userspace/tail/tail.c`, 340 lines)

Outputs the last part of files, with follow mode.

| Flag | Description |
|------|-------------|
| `-n NUM` | Last NUM lines; `+NUM` starts from line NUM |
| `-c NUM` | Last NUM bytes |
| `-f` | Follow file growth (polls every 1s) |
| `-q` / `-v` | Header control |

For regular files, `tail_bytes()` uses `fseek()`; for pipes, a ring buffer. Follow mode detects truncation and rewinds.

```bash
tail -n 20 file.txt
tail -f /var/log/syslog
tail -n +5 file.txt                  # from line 5 onwards
```

### sort (`userspace/sort/sort.c`, 471 lines)

Sorts text lines with multiple comparison modes.

| Flag | Description |
|------|-------------|
| `-r` | Reverse |
| `-n` | Numeric |
| `-h` | Human-numeric (K, M, G) |
| `-f` | Case-fold |
| `-u` | Unique lines |
| `-t CHAR` | Field separator |
| `-k KEY` | Sort by field (1-based) |
| `-o FILE` | Output to file |
| `-m` | Merge sorted files |
| `-c` | Check if sorted |
| `-s` | Stable sort |
| `-V` | Version sort |

`parse_human()` handles `k/M/G/T/P/E` suffixes. `version_compare()` splits into numeric segments for natural ordering. Merge mode does k-way merge of pre-sorted files.

```bash
sort -n numbers.txt
sort -h sizes.txt
sort -t: -k3 /etc/passwd
sort -V versions.txt
sort -u names.txt
```

### wc (`userspace/wc/wc.c`, 193 lines)

Counts lines, words, bytes, characters, and max line length.

| Flag | Description |
|------|-------------|
| `-l` | Lines |
| `-w` | Words |
| `-c` | Bytes |
| `-m` | Characters (multibyte-aware) |
| `-L` | Max line length |
| `--total` | Print total line |

Defaults to `-lwc`. Word delimiters: space, tab, newline, CR, form feed, vertical tab. Character counting uses `mbrtowc()` for locale support.

```bash
wc -l *.c                            # line count
wc -h sizes.txt                      # human-readable sizes
wc -L file.txt                       # longest line
```

## 5. Navigation

### ls (`userspace/ls/ls.c`, 446 lines)

Lists files and directories with display formats and colors.

| Flag | Description |
|------|-------------|
| `-l` | Long format |
| `-a` | Show all (`.` and `..`) |
| `-A` | Almost all (hide only `.`/`..`) |
| `-1` | One per line |
| `-R` | Recursive |
| `-h` | Human-readable sizes |
| `-S` | Sort by size |
| `-t` | Sort by time |
| `-r` | Reverse sort |
| `-i` | Show inodes |
| `-s` | Show sizes |

Entries stored in `Entry` array, sorted via `qsort()`. Long format shows mode (with setuid/setgid/sticky), links, owner, group, size, time. Color output auto-enabled on terminals: blue=dirs, cyan=symlinks, green=executables, magenta=sockets. Symlinks display as `name -> target`.

```bash
ls -la                              # long, all files
ls -lhS                             # sort by size, human-readable
ls -lt                              # sort by time
ls -Ri                              # recursive, show inodes
```

### pwd (`userspace/pwd/pwd.c`, 59 lines)

Prints the current working directory.

| Flag | Description |
|------|-------------|
| `-L` | Use `$PWD` from environment |
| `-P` | Resolve symlinks (default) |

```bash
pwd -P                             # physical path
pwd -L                             # shell's PWD
```

## 6. Finding Files

### find (`userspace/find/find.c`, 712 lines)

Recursively searches directory trees with predicates and actions.

**Predicates:** `-name`, `-type` (f/d/l/c/b/p/s), `-size [+/-]N` (k/M/G), `-mtime`/`-atime`/`-ctime`, `-perm`, `-user`, `-group`, `-newer`, `-empty`, `-inum`, `-links`

**Actions:** `-print`, `-print0`, `-ls`, `-printf FORMAT`, `-exec COMMAND ;`

**Global:** `-maxdepth N`, `-mindepth N`, `-and`/`-or`/`-not`, `( ... )`

Predicates parsed into a linked list; `eval_predicates()` evaluates with short-circuit AND. `-exec` uses `fork()`/`execvp()` with `{}` replacement. `-printf` supports `%p` (path), `%f` (basename), `%d` (depth), `%i` (inode), `%s` (size), `%m` (mode), `%u`/`%g` (owner/group), `%t` (time).

```bash
find . -name "*.c"
find / -type f -size +1M
find . -mtime -7
find . -name "*.log" -exec rm {} \;
find . -printf "%p %s\n"
find . -maxdepth 2 -name "*.txt"
```

## 7. Command-Line Conventions

All utilities follow consistent patterns:
- **Short flags:** `-l`, `-r`, `-v`
- **Long flags:** `--recursive`, `--verbose`, `--help`
- **Combined:** `ls -laR` = `ls -l -a -R`
- **`--`** stops option parsing
- **`-`** means stdin
- **Exit codes:** 0=success, 1=error, 2=usage error
- **Errors** print `utility: message` to stderr and set an error flag

## 8. POSIX Compatibility

- All POSIX-mandated flags supported (`-r` recursive, `-f` force, `-i` interactive)
- Standard fd usage (stdin/stdout/stderr)
- `-` for stdin universally supported
- `wc` uses `setlocale()` and `mbrtowc()` for multibyte support

Differences from GNU coreutils: no i18n, some GNU-specific long options absent. Error messages are in English.

## 9. Implementation Highlights

**Error handling:** Each utility prints `name: message` to stderr, sets an error flag, continues processing, and returns the appropriate exit code.

**Memory:** Dynamic arrays grow via `realloc()`. Strings duplicated with `strdup()` or manual allocation. All memory freed before exit.

**I/O patterns:** Block reads (4KB–64KB) for data processing (`cp`, `wc`), `fgets()`/`getline()` for line processing (`grep`, `sort`), `putchar()` for character-level output (`cat`, `echo`).

**Built-in regex:** `grep` includes its own engine supporting `.`, `*`, `+`, `?`, `^`, `$`, `[]`, `()`, `|`, `\` — no POSIX regex library dependency.

## 10. Usage Examples

```bash
# Project setup
mkdir -p src/utils include
touch src/main.c src/utils/helper.c

# Review
cat -n src/main.c | head -30
wc -l src/*.c include/*.h
grep -rn "TODO" src/

# Organize
cp -p config.example config.ini
mv old_module.c src/
ln -s ../src/main.c build/main.c
chmod 755 build.sh

# Find
find . -name "*.c" -exec grep -l "malloc" {} \;
find . -mtime -1 -type f

# Monitor
tail -f /var/log/app.log | grep "ERROR"
```

---

For source code, see `userspace/<utility>/<utility>.c`. Each file is self-contained and well-commented.
