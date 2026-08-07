# ForeB Boot Entry Types

ForeB entries now carry an explicit `type=` (goal 4, 5, 8, 10). This documents
each type's boot flow. The config model is in `include/forebo_cfg.h`
(`enum forebo_entry_type`), the new EFI plumbing in `uefi/efi_ext.h`, and the
arch gating in `uefi/arch.h`.

| type      | mechanism                                   | arches            |
|-----------|---------------------------------------------|-------------------|
| forest    | Multiboot1 ELF handoff (EAX=0x2BADB002)     | x86_64 only       |
| linux     | EFI-stub `vmlinuz` LoadImage/StartImage     | x64 / ARM / RISC-V|
| chainload | LoadImage/StartImage another EFI app        | x64 / ARM / RISC-V|
| shell     | open interactive shell window               | all               |
| recovery  | open recovery / disk-tools window           | all               |
| reboot    | RuntimeServices->ResetSystem                | all               |

Default when `type=` is omitted: **forest** (a zeroed struct == legacy behaviour).
Legacy `kernel=reboot` still maps to `type=reboot`.

---

## 1. `forest` — Multiboot1 Forest kernel (unchanged, x86_64 only)

The existing, working path. Load `/forebo/kernel.elf`, place multiboot modules,
`ExitBootServices`, then `handoff64to32.asm` drops to 32-bit protected mode and
jumps to the kernel entry with `EAX=0x2BADB002`, `EBX=&multiboot_info`.

Guarded by `FOREB_MULTIBOOT_SUPPORTED` (arch.h). On aarch64/riscv64 this path is
compiled out; a `forest` entry there calls `foreb_multiboot_unsupported()` which
prints a notice and returns to the menu. **This flow must never regress.**

---

## 2. `linux` — boot a real Linux distro via the EFI stub

Modern vmlinuz images are themselves EFI PE applications ("EFI stub"). ForeB
boots them the firmware-native way — no bzImage real-mode parsing.

```
config: type=linux  vmlinuz=/forebo/vmlinuz  initrd=/forebo/initrd.img
        cmdline="root=... ro quiet"

1. Build a device path to vmlinuz on the current ESP:
     LoadedImage->DeviceHandle  +  MEDIA_FILEPATH node  L"\forebo\vmlinuz"
     (FILEPATH_DEVICE_PATH in efi_ext.h; terminate with an END node)

2. foreb_LoadImage(bs, FALSE, ThisImage, dpVmlinuz, NULL, 0, &hKernel)
     -> firmware loads + relocates the stub PE.

3. Set the kernel command line as UTF-16 LoadOptions on the loaded image:
     HandleProtocol(hKernel, LOADED_IMAGE, &li);
     li->LoadOptions      = (CHAR16*)cmdline_utf16;   // "root=... ro quiet"
     li->LoadOptionsSize  = (len+1) * sizeof(CHAR16);

4. Expose the initrd via the Linux initrd media protocol so the stub can pull it:
     - Build FOREB_INITRD_DEVICE_PATH = VENDOR node (Guid=LINUX_EFI_INITRD_MEDIA)
       + END node.
     - Implement EFI_LOAD_FILE2_PROTOCOL.LoadFile:
         BootPolicy must be FALSE (else return EFI_UNSUPPORTED).
         Buffer==NULL  -> set *BufferSize and return EFI_BUFFER_TOO_SMALL.
         Buffer!=NULL  -> copy the initrd bytes, return EFI_SUCCESS.
       (Load the initrd file from the ESP into RAM first; serve from that.)
     - Publish BOTH interfaces on a fresh handle:
         InstallMultipleProtocolInterfaces(&hInitrd,
             &gEfiDevicePathProtocolGuid, &initrdDP,
             &gEfiLoadFile2ProtocolGuid,  &initrdLoadFile2, NULL);
       The stub locates this handle by matching the vendor GUID and calls LoadFile2.

5. foreb_StartImage(bs, hKernel, &exitSize, &exitData);
   (StartImage returns only if the kernel bails; then uninstall the initrd handle
    and FreePool buffers.)
```

Boundary: this is the EFI-stub path (what real distros ship). Booting a *bare*
`bzImage` with no EFI stub (the legacy 16-bit boot protocol) is out of scope.

---

## 3. `chainload` — switch to another EFI bootloader (GRUB on USB, etc.)

```
config: type=chainload  chain=/EFI/BOOT/BOOTX64.EFI     (explicit)
   or:  type=chainload                                   (auto-scan)

A) explicit 'chain=<path>':
     Resolve which volume holds it (default: the ESP ForeB booted from, or scan
     if not found), build DeviceHandle + MEDIA_FILEPATH device path, then
     foreb_LoadImage(bs, FALSE, ThisImage, dp, NULL, 0, &h) + foreb_StartImage.

B) auto-scan (chain empty):
     LocateHandleBuffer(ByProtocol, SimpleFileSystem, &n, &handles)
     for each handle (includes USB volumes):
         OpenVolume -> root
         probe, in order:
             \EFI\BOOT\BOOTX64.EFI   (\EFI\BOOT\BOOTAA64.EFI on ARM, etc.)
             \EFI\*\grubx64.efi
             \EFI\*\shimx64.efi
         first hit -> LoadImage + StartImage that volume's loader.
```

Arch-aware default filenames come from `FOREB_EFI_IMAGE_NAME` (arch.h):
`BOOTX64.EFI` / `BOOTAA64.EFI` / `BOOTRISCV64.EFI`. If a USB stick with GRUB is
present, this is how ForeB hands off to it.

---

## 4. `shell` / `recovery` — open a tool window (goal 9, 10)

Selecting one of these from the menu (or the `c` key) opens the interactive shell
/ the recovery disk-tools as a **compositor window** (see `WM_DESIGN.md §4`). No
image is loaded, no `ExitBootServices`. The window returns the same verdict as
today's `shell_run()`:

- `FOREB_SHELL_REBOOT` → the menu performs a reset,
- a boot index `>= 0` → the menu boots that entry,
- `FOREB_SHELL_BACK` → return to the menu.

Recovery tools (in the shell / recovery window): `gpt`, `parts`, `fsprobe`,
`rescue <src> <dst>`, `fatfix`, undelete-scan, magic-carve. Destructive ops are
gated behind an explicit `yes` confirmation.

---

## 5. `reboot` — firmware reset (fixes goal 8)

```
ST->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
```

`EFI_RESET_SYSTEM` is already a real callable typedef in `efi.h` and valid both
before and after `ExitBootServices` — **no header change needed**.

The old bug where `reboot` made QEMU *exit* instead of restarting is NOT in the
reset call: it is the **`-no-reboot`** flag on the interactive QEMU targets in the
Makefile. With `-no-reboot`, QEMU treats a guest-requested reset as "power off".

Fix:
- **Remove `-no-reboot`** from the INTERACTIVE qemu run targets (so a reset
  actually restarts the VM).
- **Keep `-no-reboot`** only on the HEADLESS verification targets, where a reset
  should terminate the run so the harness doesn't loop forever.

Use `EfiResetCold` for a full restart; `EfiResetWarm` for a lighter reset. On the
BIOS path, use a proper reset (e.g. triple-fault / keyboard-controller 0xFE / or
jump to the reset vector) rather than a halt.
