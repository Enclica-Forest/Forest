# Contributing to Forest OS

Welcome! Whether you are fixing a typo, adding a userspace utility, or tackling a kernel subsystem, every contribution matters. Forest OS is a collaborative, from-scratch operating system, and there is a place for every skill level.

---

## Table of Contents

1. [Ways to Contribute](#ways-to-contribute)
2. [Project Structure Overview](#project-structure-overview)
3. [Setting Up a Development Environment](#setting-up-a-development-environment)
4. [Finding Things to Work On](#finding-things-to-work-on)
5. [Code Contribution Guidelines](#code-contribution-guidelines)
6. [Pull Request Process](#pull-request-process)
7. [Issue Reporting](#issue-reporting)
8. [Documentation Contributions](#documentation-contributions)
9. [Testing Requirements](#testing-requirements)
10. [Code Review Process](#code-review-process)
11. [Licensing (GPLv3)](#licensing-gplv3)
12. [Community Guidelines](#community-guidelines)

---

## Ways to Contribute

You do not need to write kernel code to help Forest OS:

- **Bug fixes** -- a one-line fix in a userspace app counts just as much as a scheduler rewrite.
- **New features** -- filesystem drivers, userspace utilities, kernel modules, bootloader enhancements.
- **Documentation** -- improving wiki pages, adding code comments, writing guides.
- **Testing** -- booting Forest OS on hardware or in QEMU and reporting what works.
- **Code review** -- reading open pull requests and leaving feedback.
- **Bug reports** -- filing detailed issues with reproduction steps.
- **Art and design** -- boot menu icons, wallpapers, fonts, themes.

---

## Project Structure Overview

Forest OS is assembled from independently named source components:

```
$FOREST/                          <- repository root
├── fern/                         <- the Fern kernel tree
│   ├── Makefile                  <- top-level build orchestrator
│   ├── conf.sh                   <- Kconfig-style configurator
│   ├── build/                    <- make fragments (toolchain, flags, image, etc.)
│   ├── src/                      <- kernel C sources + src/include headers
│   └── initrd/                   <- initrd filesystem tree
├── foreboots/                    <- ForeB bootloader
│   ├── stage1/2/3.asm            <- BIOS stages (NASM)
│   └── uefi/                     <- UEFI loader (C, clang/lld)
├── forestos-toolchain/           <- cross-toolchain source package
├── libs/                         <- shared libraries
│   ├── libc/                     <- consolidated POSIX C library
│   ├── forestcore/               <- low-level runtime helpers
│   ├── leafgfx/                  <- graphics library
│   └── leafui/                   <- UI framework
├── userspace/                    <- 44 POSIX-compatible applications
├── wiki/                         <- project documentation
├── MAKE_AN_OS.md                 <- full build walkthrough
└── LICENSE                       <- GPLv3
```

| Name | What it is |
|------|------------|
| **Forest OS** | The complete operating system |
| **Fern** | The kernel (like Linux is the kernel, not the whole distro) |
| **ForeB** (foreboots) | The bootloader: BIOS 3-stage + UEFI EFI application |
| **forestos-toolchain** | GCC cross-compiler targeting i686-forestos / x86_64-forestos |
| **libc** | Consolidated POSIX C library at libs/libc/ |

---

## Setting Up a Development Environment

### Prerequisites (Debian/Ubuntu)

```bash
# For the cross-toolchain
sudo apt install build-essential gcc g++ make flex bison gawk \
                 texinfo curl wget tar xz-utils \
                 libgmp-dev libmpfr-dev libmpc-dev

# For building Fern + foreboots + images
sudo apt install nasm clang lld xorriso mtools python3 \
                 qemu-system-x86 ovmf dialog
```

### Quick build

```bash
export FOREST="$(git rev-parse --show-toplevel)"

# 1. Build the cross-toolchain
cd $FOREST/forestos-toolchain
./build-toolchain.sh --arch both

# 2. Configure and build the kernel
cd $FOREST/fern
./conf.sh --defconfig
make all

# 3. Run in QEMU
make run
```

For the full walkthrough, see `MAKE_AN_OS.md` at the repository root.

### Self-hosting

Once Forest OS boots with a working GCC, `./build-toolchain.sh --arch 64 --skip-deps` rebuilds the toolchain natively on the OS itself.

---

## Finding Things to Work On

### Good first issues

Look for **good first issue** tags on the repository. These typically involve:

- Adding or improving a userspace utility in `userspace/`
- Fixing a warning or style issue in existing code
- Writing or improving documentation
- Adding a test case

### Areas that always need help

- **Userspace apps** -- 44 apps exist, but many have incomplete option parsing or missing features.
- **Documentation** -- the wiki at `wiki/` is growing but incomplete. Every page helps.
- **Testing** -- boot Forest OS on different hardware, report what works.
- **Initrd** -- `fern/initrd/` needs configuration files, default programs, and resource assets.
- **Library work** -- `libs/` contains several libraries that are still maturing.

### Picking a task

1. Browse open issues or ask in the project discussion channels.
2. Pick something that matches your skill level and interest.
3. Comment on the issue to let others know you are working on it.
4. If no issue exists, open one first to discuss the approach.

---

## Code Contribution Guidelines

### General principles

- **Keep changes focused.** One logical change per pull request. Unrelated fixes go in separate PRs.
- **Match existing style.** Look at surrounding files and follow their conventions.
- **Keep it small.** Smaller PRs are reviewed faster. A 50-line change beats a 500-line one.

### Language-specific notes

| Component | Language(s) | Notes |
|-----------|------------|-------|
| **Fern kernel** | C, x86/ARM Assembly | Freestanding (-ffreestanding), no libc dependency. Uses forestos/syscalls.h. |
| **ForeB BIOS** | NASM | 16/32-bit. Stage 2 and 3 must stay under 8 KiB each. |
| **ForeB UEFI** | C (clang/lld) | Freestanding EFI app, no gnu-efi, no CRT. |
| **libc** | C | Source of truth is libs/libc/. |
| **Userspace apps** | C | Compiled with i686-forestos-gcc as freestanding ELF32. |
| **Build system** | Make, Bash | GNU Make with Kconfig-style conf.sh. |

### Coding style

- Use the existing style of the file you are editing.
- Do not add comments unless they explain *why*, not *what*.

### Commit messages

- Subject line under 72 characters, imperative mood ("Add feature" not "Added feature").
- Reference related issues with #123.
- A body is optional but encouraged for non-trivial changes.

---

## Pull Request Process

### Before you open a PR

1. Fork the repository (or clone directly if you have write access).
2. Create a feature branch from main: `git checkout -b my-feature`
3. Make your changes following the guidelines above.
4. Build and test locally (see [Testing Requirements](#testing-requirements)).

### Opening the PR

- Give your PR a clear, descriptive title.
- Explain what the change does and why.
- Reference related issues.
- Include screenshots or terminal output for visual/behavioral changes.
- Note limitations or areas you want reviewers to focus on.

### After opening a PR

- **Be patient.** Reviews take time, especially for kernel or bootloader changes.
- **Respond to feedback** promptly and thoughtfully.
- **Push additional commits** to address review comments -- do not force-push during review unless asked.
- **Keep the PR updated** with the base branch if it drifts.

### Merge criteria

- At least one approving review.
- All automated checks pass.
- No unresolved review comments.
- Squash-merge is preferred for clean history.

---

## Issue Reporting

### What to include

- **Title** -- concise summary of the problem.
- **Environment** -- host OS, Forest OS commit, architecture, QEMU or hardware.
- **Steps to reproduce** -- minimal, numbered steps.
- **Expected behavior** -- what you expected.
- **Actual behavior** -- what happened, with error messages or logs.
- **Relevant logs** -- serial output, QEMU console, kernel messages.

### Before filing

- Search existing issues to avoid duplicates.
- Try to reproduce on the latest version.

---

## Documentation Contributions

Documentation is one of the most valuable contributions you can make.

### Where docs live

- **Wiki pages** -- `wiki/` directory, written in Markdown.
- **README files** -- component-level (fern/README.md, foreboots/README.md, libs/README.md).
- **Code comments** -- inline documentation in source files.
- **Guides** -- MAKE_AN_OS.md at the repository root.

### How to contribute docs

- Fork, create a branch, edit the Markdown files, and open a PR.
- Follow the existing tone: technically precise but approachable.
- Include code examples when explaining concepts.
- Test any commands you document -- they should work as written.

Each wiki page should have a clear title, a brief introduction, and well-organized sections. Link to related pages where appropriate.

---

## Testing Requirements

### What to test before submitting a PR

1. **Build succeeds** -- kernel, bootloader, toolchain, and userspace apps compile without errors.
2. **Boot test** -- run `make run` and verify Forest OS boots to a shell prompt in QEMU.
3. **Functional test** -- verify your change works and existing functionality is not broken.
4. **Size constraints** -- BIOS stage 2/3 must be under 8 KiB each (enforced by `make check`).

### Testing in QEMU

```bash
cd $FOREST/fern
make run          # boots with configured settings
make run-bios     # force BIOS path
make run-uefi     # force UEFI path
make debug        # boot with GDB stub on :1234
```

### Testing on real hardware

Not required for most contributions, but valuable. If you do: note hardware specs, test both BIOS and UEFI if possible, and report differences from QEMU behavior.

---

## Code Review Process

### What reviewers look for

- **Correctness** -- does the code do what it claims?
- **Style** -- does it match existing conventions?
- **Safety** -- memory safety, buffer overflows, race conditions?
- **Size** -- do binary sizes stay within budget?
- **Testing** -- has the change been tested?

### Responding to review feedback

- Be open-minded -- suggestions aim to improve the code.
- Ask questions if a comment is unclear.
- Make changes in new commits so reviewers can see incremental progress.
- Mark conversations as resolved once addressed.

### For reviewers

- Be constructive and encouraging, especially with new contributors.
- Distinguish between blocking issues (must fix) and nitpicks (nice to have).
- Approve when the change is good enough -- perfection is the enemy of progress.

---

## Licensing (GPLv3)

Forest OS is licensed under the **GNU General Public License v3.0**. This means:

- All source code is free software, redistributable and modifiable under GPLv3.
- You must pass on the same freedoms to recipients.
- Modified versions must carry notices stating they were changed and the date.
- There is no warranty.

### What this means for contributors

- By contributing, you agree your code will be licensed under GPLv3.
- Your code must be your own work, or you must have the right to license it under GPLv3.
- Third-party libraries (libs/uacpi/ MIT, libs/qrcodegen/ MIT) have their own licenses. Respect them.

### Adding new files

New C files should include the GPLv3 header:

```c
/*
 * Forest OS
 * Copyright (C) 2024 Forest OS Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
```

---

## Community Guidelines

### Code of conduct

- **Be respectful.** Treat everyone with dignity regardless of experience level.
- **Be constructive.** Offer helpful suggestions, not just criticism.
- **Be patient.** Not everyone works at the same pace, and that is okay.
- **Be inclusive.** Welcome newcomers. Everyone was a beginner once.

### Communication channels

- **Issues** -- bug reports, feature requests, technical discussions.
- **Pull requests** -- code review and collaboration.
- **Project discussions** -- general questions and planning.

### Getting help

If you are stuck, ask. Start with a clear description of what you are trying to do and where you are stuck. Include your OS, the component you are working on, and what you have tried so far.

### Recognition

All contributors are recognized in the project. Your work helps build an operating system from the ground up -- that is a remarkable thing to be part of.

---

*Thank you for contributing to Forest OS! Every patch, every bug report, every documentation fix makes the project better.*
