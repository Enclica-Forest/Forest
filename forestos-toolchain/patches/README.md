# Forest-OS toolchain patches

These files teach stock GNU binutils and GCC about the real Forest-OS target
triples `i686-forestos` and `x86_64-forestos`, instead of the old
`i686-elf` + symlink hack. `build-toolchain.sh` applies them idempotently
before configuring each source tree.

## Contents

| File | Purpose |
|------|---------|
| `gcc/config/forestos.h` | GCC OS-config header. Copied to `gcc/config/forestos.h` in the GCC source tree. Defines `__forestos__`, ELF startfile/endfile specs, and the dynamic-linker path. |
| `config.sub.forestos.patch` | Reference diff adding `forestos*` to the OS validation list in `config.sub` (applies to both binutils and gcc). |
| `config.gcc.forestos.patch` | Reference diff adding the `i686-forestos` / `x86_64-forestos` target cases to `gcc/config.gcc`. |

## How they are applied

`build-toolchain.sh` does **not** run `patch -p1` on the two config files,
because the anchor lines drift between binutils/gcc versions (e.g. the
`fiwix*` OS line has extra entries in binutils 2.43). Instead it performs
**guarded, idempotent `sed` insertions** keyed off stable tokens, so a
second run is a no-op and a resumed build stays consistent. The `.patch`
files above document exactly what those insertions produce, and can be
applied by hand (`patch -p1 < config.sub.forestos.patch`) if you prefer.

`gcc/config/forestos.h` is a genuine tracked asset: the script copies it
verbatim into the GCC source tree, so edit it here to change target macros.

## Pinned versions

The patches are validated against **binutils 2.43** and **GCC 13.2.0**
(the versions `build-toolchain.sh` pins). They should apply cleanly to
nearby releases but are only guaranteed for those two.
