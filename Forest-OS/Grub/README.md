# Forest OS Boot Configuration

## Status

**ForeB is the default bootloader for Forest OS.** GRUB is retained as a
**legacy / optional fallback** for UEFI scenarios, recovery, and exotic
hardware where ForeB's BIOS-mode VBE detection is insufficient.

The build switch is `ENABLE_FOREB_BOOTLOADER` in `build-config.mk` / `conf.sh`
(`CONFIG_ENABLE_FOREB_BOOTLOADER`). When enabled, the top-level Makefile builds
ForeB via `make forebo` / `make forebo-image` and produces bootable media using
ForeB as the MBR / El Torito boot image. When disabled, the build falls back to
`grub-mkrescue` with `grub.cfg`.

## Files in this directory

| File         | Role                                                                 |
|--------------|----------------------------------------------------------------------|
| `forebo.cfg` | **Canonical** ForeB boot config (key=value). Source of truth for the |
|              | boot menu, kernel cmdline, and video defaults.                       |
| `grub.cfg`   | **Legacy** GRUB config, kept as a fallback for UEFI / recovery.      |

## Boot chain

```
BIOS -> ForeB stage1 (MBR) -> ForeB stage2 (GUI menu, VBE/E820/disk)
      -> ForeB stage3 (32-bit PM ELF loader)
      -> Forest OS kernel (EAX=0x2BADB002, EBX=&multiboot_info_t)
```

ForeB hands the kernel a standard **Multiboot1** info structure, so the kernel
boots identically whether loaded by ForeB or GRUB. See
`../foreboots/README.md` for the full boot protocol and the
`foreboots_boot_info` structure layout.

## When to use GRUB instead

- **UEFI boot**: ForeB is BIOS/CSM-only. Use GRUB or the EFI stub for UEFI.
- **Recovery**: GRUB's rescue shell is useful for unbricking.
- **Exotic video**: if ForeB's VBE enumeration fails on quirky firmware, GRUB's
  `gfxpayload` may work better.

## Migrating back to GRUB

Set `ENABLE_FOREB_BOOTLOADER=no` (or run `conf.sh` and disable the option) and
rebuild. The `grub.cfg` here is already a complete Forest OS menu.
