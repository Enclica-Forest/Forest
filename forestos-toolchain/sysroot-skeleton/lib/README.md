# sysroot lib/ — build outputs, NOT tracked here

This directory is intentionally empty in the tracked skeleton.

The runtime objects that land in `sysroot/lib/` — `crt0.o`, `crti.o`, `crtn.o`,
`libc.a` — are **build outputs**, produced when the toolchain / libc is built.
They are architecture-specific (i386 for `ARCH=32`, x86_64 for `ARCH=64`) and
can be multi-hundred-KB binaries, so they are `.gitignore`d and regenerated on
every build rather than committed.

`crtbegin.o` / `crtend.o` are supplied by GCC itself under
`install/lib/gcc/<triple>/<version>/` and never live here.

For the freestanding **Fern** kernel these objects are largely irrelevant to the
final `fern.bin` / `fern.elf` (the kernel links with `-nostdlib -T <script>`),
but they are needed to complete GCC's `libgcc` / target-libc phase and for any
future Forest-OS userspace.
