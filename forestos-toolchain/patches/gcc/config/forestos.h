/* Forest-OS target macros for GCC (i686-forestos / x86_64-forestos).
 *
 * This OS-config header is copied into gcc/config/forestos.h by
 * build-toolchain.sh and referenced from gcc/config.gcc for the
 * i[34567]86-*-forestos* and x86_64-*-forestos* targets.
 *
 * Forest-OS uses a freestanding, ELF-based, newlib-style startup model.
 * The cross-compiler is built with --without-headers / -nostdlib in mind:
 * the Fern kernel links with its own linker script and never pulls the
 * startfiles below, and userspace CRT objects (crt0.o/crti.o/crtn.o) are
 * supplied later from the sysroot. Referencing them here is harmless for
 * the libgcc build because libgcc compiles objects rather than linking
 * full executables.
 */

/* Predefined macros so target code can detect Forest-OS. */
#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()            \
  do {                                      \
    builtin_define ("__forestos__");        \
    builtin_define ("__forestos");          \
    builtin_define ("__ForestOS__");        \
    builtin_assert ("system=forestos");     \
    builtin_assert ("system=unix");         \
  } while (0)

/* Startup / shutdown files. Standard ELF crt ordering; only referenced
 * when linking a full executable (not during libgcc build, and suppressed
 * by -nostdlib which the Fern kernel always passes). */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!shared:crt0.o%s} crti.o%s crtbegin.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s crtn.o%s"

/* Default C library: link libc unless -nostdlib/-nolibc requested. */
#undef LIB_SPEC
#define LIB_SPEC "%{!nostdlib:%{!nolibc:-lc}}"

/* Dynamic linker path for future shared userspace (unused by the kernel). */
#undef DYNAMIC_LINKER
#define DYNAMIC_LINKER "/lib/ld-forestos.so.1"

/* Forest-OS is a freestanding-friendly target; do not assume a hosted libc
 * is present when configured with --without-headers. */
#undef TARGET_LIBC_PROVIDES_SSP
