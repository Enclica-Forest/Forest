# build/features/memory.mk
#
# Memory-management source gating.
#
# This fragment is included by the main Makefile AFTER build/kernel-sources.mk.
# It appends to the shared EXCLUDED_CSOURCES variable (defined in
# kernel-sources.mk) to drop memory-management sources from the kernel build
# when the corresponding ENABLE_* feature is "no".
#
# Gating convention (per build-spec.md):
#   ifeq ($(ENABLE_FOO),no)
#   EXCLUDED_CSOURCES += $(SRCDIR)/foo.c $(SRCDIR)/foo_helper.c
#   endif
#
# Every path is wrapped in $(wildcard ...) so a missing file expands to empty
# and does not break the build. Only files verified to exist in src/ are
# listed; core allocators (mm_init.c, mm_buddy.c, bitmap_pmm.c, pmm.c,
# kheap.c) are always kept and are NOT gated here.
#
# Feature -> source map (verified against src/):
#   ENABLE_PAGING                     paging64.c paging_modes.c
#                                     page_fault_minimal.c page_fault_recovery.c
#                                     vmm.c mm_vma.c
#   ENABLE_SLAB                       mm_slab.c
#   ENABLE_COW                        mm_cow.c mm_cow_impl.c
#   ENABLE_SWAP                       mm_swap.c
#   ENABLE_PAGE_CACHE                 mm_pagecache.c
#   ENABLE_OOM_KILLER                 mm_oom.c
#   ENABLE_MEMORY_RECLAIM             mm_reclaim.c
#   ENABLE_MEMORY_STATS               mm_stats.c
#   ENABLE_TLB_SHOOTDOWN              tlb.c tlb_manager.c
#   ENABLE_MEMORY_CORRUPTION_DETECTION memory_corruption.c memory_corruption_test.c
#   ENABLE_MEMORY_PROTECTION          mem_protect.c secure_vmm.c
#   ENABLE_MEMORY_VALIDATION          memory_validation.c
#   ENABLE_MEMORY_DEBUG               memory_debug.c mm_debug.c memory_tests.c
#
# No dedicated source files (logic lives in shared/core files, so not gated):
#   ENABLE_GUARD_PAGES  -> guard-page logic is in memory.c (core, kept)
#   ENABLE_ASLR         -> ASLR logic is in mm_layout.c / paging_modes.c
#                          (no dedicated aslr.c; paging_modes.c is gated by
#                          ENABLE_PAGING above)
#   ENABLE_NX_BIT       -> NX enforcement is in mem_protect.c (gated by
#                          ENABLE_MEMORY_PROTECTION above when that is off)
# ---------------------------------------------------------------------------

# ENABLE_PAGING: 64-bit paging, paging modes, page-fault handlers, the virtual
# memory manager, and VMA tracking. mm_init.c stays (core initializer).
ifeq ($(ENABLE_PAGING),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/paging64.c) \
    $(wildcard $(SRCDIR)/paging_modes.c) \
    $(wildcard $(SRCDIR)/page_fault_minimal.c) \
    $(wildcard $(SRCDIR)/page_fault_recovery.c) \
    $(wildcard $(SRCDIR)/vmm.c) \
    $(wildcard $(SRCDIR)/mm_vma.c)
endif

# ENABLE_SLAB: SLAB allocator.
ifeq ($(ENABLE_SLAB),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_slab.c)
endif

# ENABLE_COW: copy-on-write support.
ifeq ($(ENABLE_COW),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_cow.c) \
    $(wildcard $(SRCDIR)/mm_cow_impl.c)
endif

# ENABLE_SWAP: swap-out / swap-in support.
ifeq ($(ENABLE_SWAP),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_swap.c)
endif

# ENABLE_PAGE_CACHE: page cache for file-backed pages.
ifeq ($(ENABLE_PAGE_CACHE),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_pagecache.c)
endif

# ENABLE_OOM_KILLER: out-of-memory killer.
ifeq ($(ENABLE_OOM_KILLER),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_oom.c)
endif

# ENABLE_MEMORY_RECLAIM: page reclamation / shrinker logic.
ifeq ($(ENABLE_MEMORY_RECLAIM),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_reclaim.c)
endif

# ENABLE_MEMORY_STATS: memory usage statistics.
ifeq ($(ENABLE_MEMORY_STATS),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mm_stats.c)
endif

# ENABLE_TLB_SHOOTDOWN: TLB shootdown IPI support.
ifeq ($(ENABLE_TLB_SHOOTDOWN),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/tlb.c) \
    $(wildcard $(SRCDIR)/tlb_manager.c)
endif

# ENABLE_MEMORY_CORRUPTION_DETECTION: corruption detectors and their tests.
ifeq ($(ENABLE_MEMORY_CORRUPTION_DETECTION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/memory_corruption.c) \
    $(wildcard $(SRCDIR)/memory_corruption_test.c)
endif

# ENABLE_MEMORY_PROTECTION: memory protection enforcement and secure VMM.
ifeq ($(ENABLE_MEMORY_PROTECTION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/mem_protect.c) \
    $(wildcard $(SRCDIR)/secure_vmm.c)
endif

# ENABLE_MEMORY_VALIDATION: memory access validation hooks.
ifeq ($(ENABLE_MEMORY_VALIDATION),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/memory_validation.c)
endif

# ENABLE_MEMORY_DEBUG: debug helpers and memory test harness.
ifeq ($(ENABLE_MEMORY_DEBUG),no)
EXCLUDED_CSOURCES += \
    $(wildcard $(SRCDIR)/memory_debug.c) \
    $(wildcard $(SRCDIR)/mm_debug.c) \
    $(wildcard $(SRCDIR)/memory_tests.c)
endif
