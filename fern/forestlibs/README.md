# Forest Libraries (forestlibs)

This directory previously contained the Forest OS libc. The libc has been
consolidated into `libs/libc/` — the single source of truth for all libc
headers, sources, and build integration across the entire Forest OS ecosystem.

## Current State

The libc (headers, source files, Makefile.inc) now lives at:

```
libs/libc/
├── include/libc/    # All headers (merged from all previous locations)
├── src/             # Implementation files
├── Makefile.inc     # Build system integration
└── README.md        # Full documentation
```

## Why Consolidated?

The libc previously existed in 5 separate locations:
- `libs/libc/include/libc/` — exported headers
- `fern/forestlibs/libc/` — implementation + headers
- `fern/libs/libc/` — duplicate headers
- `forestos-toolchain/sysroot-skeleton/usr/include/libc/` — toolchain bootstrap
- `fern/temp/usr/libc/` — build artifact

This duplication caused constant conflicts between the kernel, toolchain, and
userspace apps. The consolidation into `libs/libc/` ensures all components use
the same single source of truth.

See `../../libs/libc/README.md` for full documentation.
