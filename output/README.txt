Forest OS — Build Output
========================
Generated: Fri  7 Aug 23:44:50 BST 2026

Configuration:
  Architecture:  32
  Boot Mode:     bios
  Build Type:    debug
  OpenGL:        yes
  Networking:    yes
  Audio:         no
  SMP:           no
  X11 Server:    yes
  Initrd Style:  standard

Files:
  kernel/fern.bin       — Kernel binary (BIOS)
  kernel/fern.elf       — Kernel ELF (UEFI)
  kernel/BOOTX64.EFI    — UEFI application
  bootloader/           — BIOS boot stages
  initrd/initrd.tar     — Initial ramdisk
  forebo.img            — BIOS disk image (write to USB)
  esp.img               — EFI System Partition
  forebo.iso            — Hybrid ISO (burn to CD/DVD)

Boot in QEMU:
  BIOS:  qemu-system-i386 -drive format=raw,file=forebo.img -serial stdio -vga std
  UEFI:  qemu-system-x86_64 -drive format=raw,file=esp.img -bios /usr/share/ovmf/OVMF.fd

Install to USB:
  sudo dd if=forebo.img of=/dev/sdX bs=1M conv=fsync
  (REPLACE /dev/sdX — THIS ERASES THE TARGET DISK!)
