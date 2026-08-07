# =============================================================================
# makeconfigs/features/memory.mk — Memory subsystem feature gating
# =============================================================================
# Gates memory-related kernel sources when ENABLE_PAGING / ENABLE_SLAB /
# ENABLE_MEMORY_PROTECTION / etc. are `no`. Appends to EXCLUDED_CSOURCES.
#
# Shared variable: EXCLUDED_CSOURCES (do NOT redefine -- append only).

ifeq ($(ENABLE_PAGING),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/paging.c)
endif

ifeq ($(ENABLE_SLAB),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/slab.c)
endif

ifeq ($(ENABLE_MEMORY_PROTECTION),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_protection.c)
endif

ifeq ($(ENABLE_GUARD_PAGES),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/guard_pages.c)
endif

ifeq ($(ENABLE_ASLR),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/aslr.c)
endif

ifeq ($(ENABLE_COW),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/cow.c)
endif

ifeq ($(ENABLE_SWAP),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/swap.c)
endif

ifeq ($(ENABLE_PAGE_CACHE),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/page_cache.c)
endif

ifeq ($(ENABLE_OOM_KILLER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/oom_killer.c)
endif

ifeq ($(ENABLE_MEMORY_RECLAIM),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_reclaim.c)
endif

ifeq ($(ENABLE_MEMORY_STATS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_stats.c)
endif

ifeq ($(ENABLE_MEMORY_CORRUPTION_DETECTION),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_corruption_detection.c)
endif

ifeq ($(ENABLE_MEMORY_VALIDATION),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_validation.c)
endif

ifeq ($(ENABLE_MEMORY_DEBUG),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/memory_debug.c)
endif

ifeq ($(ENABLE_TLB_SHOOTDOWN),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/tlb_shootdown.c)
endif

ifeq ($(ENABLE_PAE),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/pae.c)
endif

# End of makeconfigs/features/memory.mk
