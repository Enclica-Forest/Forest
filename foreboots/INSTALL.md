# Installing ForeB alongside your existing bootloader

`tools/forebo-install` is a host-side Linux tool that reads your **existing**
bootloader configuration and translates it 1:1 into ForeB's
`\forebo\forebo.cfg`, then — optionally — copies ForeB onto the ESP
**alongside** your current bootloader (it never replaces anything) and
registers a UEFI boot entry.

Supported source configs (auto-detected in this order):

| Bootloader   | Config location on the ESP                                   | Fidelity    |
|--------------|--------------------------------------------------------------|-------------|
| Limine       | `limine.conf`, `limine.cfg`, `limine/limine.conf` (or `.cfg`)| exact       |
| GRUB         | `grub/grub.cfg`                                              | best-effort |
| systemd-boot | `loader/loader.conf` + `loader/entries/*.conf`               | exact       |

Requires python3 >= 3.8, stdlib only. Pillow (`python3-pil`) is used for
wallpaper conversion when installed; otherwise a built-in minimal PNG
decoder is used.

## Quick start (CachyOS + Limine, ESP at /boot)

```console
$ ./tools/forebo-install scan                 # read-only report
$ ./tools/forebo-install generate             # preview the forebo.cfg
$ sudo ./tools/forebo-install install         # full install
$ sudo ./tools/forebo-install install --dry-run   # print actions only
```

Reboot, then pick **ForeB** in your firmware boot menu (or the UEFI boot
list). Your existing bootloader is untouched and remains the default, so if
anything goes wrong you simply boot it again from the firmware menu.

## Commands

### `scan`
Parses the configs found on the ESP and prints a report: the translated
entry tree (with icons, paths, cmdlines), the globals that would be emitted,
row counts against the firmware limit, and all warnings. Read-only.

### `generate`
Prints the translated `forebo.cfg` to stdout, or writes it to `--output
FILE`. Works fully offline, e.g.:

```console
$ ./tools/forebo-install generate --config /mnt/usb/limine.conf --esp /mnt/usb
```

### `install` (needs root)
1. Translates the config (same as `generate`).
2. Copies `BOOTX64.EFI` from the repo to `<ESP>/EFI/ForeB/BOOTX64.EFI`.
3. Copies `assets/bg.bmp` and `assets/icons/*.tga` to `<ESP>/forebo/…`.
4. Converts the wallpaper referenced by the source config (PNG → 24-bit BMP)
   to `<ESP>/forebo/wallpaper.bmp` when needed, and references it via
   `background=`.
5. Writes the generated config to `<ESP>/forebo/forebo.cfg`.
6. Unless `--no-nvram`, runs `efibootmgr`: skips creation if a boot entry
   pointing at `\EFI\ForeB\…` already exists, otherwise runs
   `efibootmgr -c -d <disk> -p <part> -L ForeB -l '\EFI\ForeB\BOOTX64.EFI'`.
   The new entry is **appended**; BootOrder is left untouched unless
   `--make-default` is given.
7. Prints a summary of what was installed where and how to boot it.

Writes are confined to `<ESP>/EFI/ForeB/` and `<ESP>/forebo/` — the tool
refuses to touch anything else, and the config write is done via
temp-file-then-rename.

## Flags

| Flag | Meaning |
|---|---|
| `--esp PATH` | ESP mount point (default: first vfat mount of `/boot`, `/boot/efi`, `/efi`) |
| `--config FILE` | Explicit source config; skips auto-detection |
| `--repo DIR` | ForeB repo root (default: parent of `tools/`) |
| `--default-entry N` | Override the default entry (1-based over bootable entries in file order) |
| `--max-entries N` | Safety cap on translated entries (default 56; firmware allows 64 rows total) |
| `--no-extras` | Do not append the ForeB utility entries (shell/recovery/tools/setup/reboot) |
| `--no-nvram` | Skip efibootmgr registration |
| `--make-default` | Put ForeB first in BootOrder (default: append only) |
| `--dry-run` | `install`: print every action without doing it |
| `-v`, `--verbose` | Also print notes (ignored keys, detection details) |
| `--selftest` | Run the offline test suite against `tools/tests/` fixtures |

Exit codes: `0` ok, `1` error, `2` usage.

## What gets translated

- **Limine**: `timeout`, `default_entry` (1-based → emitted as a title path
  like `default=CachyOS/linux-cachyos-rc-gcc`), `remember_last_entry`,
  `wallpaper` (PNG converted to BMP). Entries map by protocol:
  `linux` → `type=linux` (first `module_path` → `initrd=`, extras become
  comments), `efi_chainload`/`efi`/`limine` → `type=chainload`,
  `multiboot[1]` → `type=forest`, `multiboot2` → `type=forest` + warning.
  `#<hex>` content-hash suffixes are stripped from paths; `_sha256_<hex>`
  inside a filename is kept. Group hierarchy becomes nested `submenu`
  blocks.
- **GRUB** (best-effort): `menuentry`/`submenu` blocks, `linux`/`initrd`/
  `chainloader` lines, `--class` icon hints, `set default=` / `set timeout=`.
- **systemd-boot**: `default` pattern (matched against entry filenames and
  titles), `timeout`, and `title`/`linux`/`efi`/`initrd`/`options` from
  `loader/entries/*.conf`.

Icons are guessed from the entry title + kernel filename (`cachyos` →
`arch`, `windows` → `windows`, `snapshot`/`fallback` → `safe`,
`efi fallback` → `usb`, Linux kernels → `tux`, …); unmatched groups get no
icon.

## Caveats

- ForeB can only boot files that live **on the ESP**. Paths that clearly
  refer to another partition (limine `guid(<other>)`, GRUB `(hdX,Y)`
  prefixes) produce a warning — copy those kernels/initrds onto the ESP.
- Firmware limits: paths and cmdlines ≤ 255 chars, titles ≤ 63 chars, at
  most 64 menu rows (submenus + entries). The tool warns when a limit is
  exceeded and caps entries at `--max-entries`.
- If your distro regenerates its bootloader config (new kernel, new
  snapshot), re-run `sudo ./tools/forebo-install install` to refresh
  `\forebo\forebo.cfg`.

## Uninstalling

ForeB never modified your existing bootloader, so removing it is simply:

```console
$ sudo efibootmgr -b <XXXX> -B     # the ForeB entry, see 'efibootmgr -v'
$ sudo rm -rf /boot/EFI/ForeB /boot/forebo
```
