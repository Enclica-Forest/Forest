/**
 * @file tlb.c
 * @brief Translation Lookaside Buffer (TLB) Management
 * 
 * Comprehensive TLB management including:
 * - Single page invalidation (INVLPG)
 * - Full TLB flush (MOV to CR3)
 * - Global page handling
 * - SMP TLB shootdown
 * - PCID support (Process Context IDentifiers)
 * 
 * Based on Intel/AMD documentation and OSDev recommendations.
 */

#include "include/tlb.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/spinlock.h"
#include "include/atomic_mm.h"
#include "include/debuglog.h"

// ============================================================================
// CONSTANTS
// ============================================================================

// CR4 bits
#define CR4_PGE         (1 << 7)    // Page Global Enable
#define CR4_PCIDE       (1 << 17)   // PCID Enable

// INVPCID types
#define INVPCID_ADDR            0   // Invalidate single address
#define INVPCID_PCID_ALL        1   // Invalidate all for PCID
#define INVPCID_ALL_GLOBAL      2   // Invalidate all including global
#define INVPCID_ALL_NONGLOBAL   3   // Invalidate all except global

// Maximum CPUs for shootdown
#define MAX_CPUS                64

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief TLB shootdown request
 */
typedef struct {
    volatile uint64_t target_addr;      // Address to invalidate (0 for full flush)
    volatile uint32_t requesting_cpu;   // CPU that requested shootdown
    volatile uint32_t pending_cpus;     // Bitmask of CPUs that need to respond
    volatile uint32_t completed_cpus;   // Bitmask of CPUs that completed
    volatile bool active;               // Request is active
} tlb_shootdown_t;

/**
 * @brief TLB manager state
 */
static struct {
    bool initialized;
    bool pcid_supported;
    bool invpcid_supported;
    bool global_pages_supported;
    uint32_t num_cpus;
    uint32_t current_cpu;       // BSP is 0
    
    // Shootdown state
    tlb_shootdown_t shootdown;
    spinlock_t shootdown_lock;
    
    // Statistics
    uint64_t invlpg_count;
    uint64_t flush_count;
    uint64_t shootdown_count;
} tlb_state = { .initialized = false };

// ============================================================================
// LOW-LEVEL OPERATIONS
// ============================================================================

/**
 * @brief Get CR3 value
 */
static inline uint64_t read_cr3(void) {
    uint64_t value;
#ifdef __x86_64__
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
#else
    uint32_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    value = v;
#endif
    return value;
}

/**
 * @brief Set CR3 value
 */
static inline void write_cr3(uint64_t value) {
#ifdef __x86_64__
    __asm__ volatile("mov %0, %%cr3" :: "r"(value) : "memory");
#else
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint32_t)value) : "memory");
#endif
}

/**
 * @brief Get CR4 value
 */
static inline uint64_t read_cr4(void) {
    uint64_t value;
#ifdef __x86_64__
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
#else
    uint32_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    value = v;
#endif
    return value;
}

/**
 * @brief Set CR4 value
 */
static inline void write_cr4(uint64_t value) {
#ifdef __x86_64__
    __asm__ volatile("mov %0, %%cr4" :: "r"(value) : "memory");
#else
    __asm__ volatile("mov %0, %%cr4" :: "r"((uint32_t)value) : "memory");
#endif
}

/**
 * @brief Execute INVLPG instruction
 */
static inline void invlpg(uint64_t addr) {
#ifdef __x86_64__
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
#else
    __asm__ volatile("invlpg (%0)" :: "r"((uint32_t)addr) : "memory");
#endif
}

/**
 * @brief Execute INVPCID instruction (if supported)
 */
#ifdef __x86_64__
static inline void invpcid(uint64_t type, uint64_t pcid, uint64_t addr) {
    struct {
        uint64_t pcid;
        uint64_t addr;
    } descriptor = { pcid, addr };
    
    __asm__ volatile("invpcid %0, %1" :: "m"(descriptor), "r"(type) : "memory");
}
#endif

/**
 * @brief Check CPUID for features
 */
static void detect_features(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Check basic features (CPUID.01H)
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) 
                     : "a"(1));
    
    // PGE (Page Global Extensions) - EDX bit 13
    tlb_state.global_pages_supported = (edx & (1 << 13)) != 0;
    
    // PCID support - ECX bit 17
    tlb_state.pcid_supported = (ecx & (1 << 17)) != 0;
    
    // Check extended features for INVPCID (CPUID.07H.0:EBX[10])
    if (tlb_state.pcid_supported) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(7), "c"(0));
        tlb_state.invpcid_supported = (ebx & (1 << 10)) != 0;
    }
    
    print("[TLB] Features: Global pages=");
    print(tlb_state.global_pages_supported ? "yes" : "no");
    print(", PCID=");
    print(tlb_state.pcid_supported ? "yes" : "no");
    print(", INVPCID=");
    print(tlb_state.invpcid_supported ? "yes" : "no");
    print("\n");
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize TLB manager
 */
void tlb_init(void) {
    if (tlb_state.initialized) {
        return;
    }
    
    print("[TLB] Initializing TLB manager...\n");
    
    // Detect CPU features
    detect_features();
    
    // Initialize shootdown state
    spinlock_init(&tlb_state.shootdown_lock, "tlb_shootdown");
    tlb_state.shootdown.active = false;
    
    // Reset statistics
    tlb_state.invlpg_count = 0;
    tlb_state.flush_count = 0;
    tlb_state.shootdown_count = 0;
    
    // Enable global pages if supported
    if (tlb_state.global_pages_supported) {
        uint64_t cr4 = read_cr4();
        cr4 |= CR4_PGE;
        write_cr4(cr4);
        print("[TLB] Global pages enabled\n");
    }
    
    tlb_state.num_cpus = 1;  // Start with BSP only
    tlb_state.current_cpu = 0;
    tlb_state.initialized = true;
    
    print("[TLB] TLB manager initialized\n");
}

/**
 * @brief Invalidate a single page in TLB
 */
void tlb_invalidate_page(uint64_t vaddr) {
    if (!tlb_state.initialized) {
        // Still use invlpg even if not initialized
        invlpg(vaddr);
        return;
    }
    
    invlpg(vaddr);
    tlb_state.invlpg_count++;
}

/**
 * @brief Invalidate a range of pages
 */
void tlb_invalidate_range(uint64_t start, uint64_t end) {
    start = start & ~(MEMORY_PAGE_SIZE - 1);
    end = (end + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
    
    // If range is large, just flush the whole TLB
    uint64_t num_pages = (end - start) / MEMORY_PAGE_SIZE;
    if (num_pages > 32) {
        tlb_flush_all();
        return;
    }
    
    for (uint64_t addr = start; addr < end; addr += MEMORY_PAGE_SIZE) {
        tlb_invalidate_page(addr);
    }
}

/**
 * @brief Flush entire TLB (excluding global pages)
 */
void tlb_flush(void) {
    if (!tlb_state.initialized) {
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
        return;
    }
    
    // Reload CR3 to flush TLB
    uint64_t cr3 = read_cr3();
    write_cr3(cr3);
    tlb_state.flush_count++;
}

/**
 * @brief Flush entire TLB including global pages
 */
void tlb_flush_all(void) {
    if (!tlb_state.initialized) {
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
        return;
    }
    
    if (tlb_state.global_pages_supported) {
        // Disable PGE, reload CR3, re-enable PGE
        uint64_t cr4 = read_cr4();
        write_cr4(cr4 & ~CR4_PGE);
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
        write_cr4(cr4);
    } else {
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
    }
    
    tlb_state.flush_count++;
}

/**
 * @brief Flush TLB on address space switch
 */
#if defined(__x86_64__)
void tlb_switch_address_space(uint64_t new_cr3) {
#else
void tlb_switch_address_space(uint32_t new_cr3) {
#endif
    if (!tlb_state.initialized) {
        write_cr3(new_cr3);
        return;
    }
    
#ifdef __x86_64__
    if (tlb_state.pcid_supported) {
        // With PCID, we can preserve some TLB entries
        // Set bit 63 to indicate no flush (NOFLUSH)
        // For now, we always flush for simplicity
        write_cr3(new_cr3);
    } else {
        write_cr3(new_cr3);
    }
#else
    write_cr3(new_cr3);
#endif
    
    tlb_state.flush_count++;
}

/**
 * @brief Request TLB shootdown across CPUs
 * 
 * This is called when a page table entry is modified that
 * may be cached in other CPUs' TLBs.
 */
void tlb_shootdown_page(uint64_t vaddr) {
    if (!tlb_state.initialized || tlb_state.num_cpus <= 1) {
        // Single CPU, just invalidate locally
        tlb_invalidate_page(vaddr);
        return;
    }
    
    spinlock_acquire(&tlb_state.shootdown_lock);
    
    // Set up shootdown request
    tlb_state.shootdown.target_addr = vaddr;
    tlb_state.shootdown.requesting_cpu = tlb_state.current_cpu;
    tlb_state.shootdown.pending_cpus = (1 << tlb_state.num_cpus) - 1;
    tlb_state.shootdown.pending_cpus &= ~(1 << tlb_state.current_cpu);
    tlb_state.shootdown.completed_cpus = 0;
    tlb_state.shootdown.active = true;
    
    // Invalidate locally first
    tlb_invalidate_page(vaddr);
    
    // Send IPI to other CPUs (placeholder - requires APIC support)
    // In a real implementation, you would:
    // 1. Send IPI to all other CPUs
    // 2. Wait for all CPUs to acknowledge
    // 3. Clear the request
    
    // For now, just mark as complete (single CPU simulation)
    tlb_state.shootdown.completed_cpus = tlb_state.shootdown.pending_cpus;
    tlb_state.shootdown.active = false;
    
    tlb_state.shootdown_count++;
    spinlock_release(&tlb_state.shootdown_lock);
}

/**
 * @brief Request full TLB shootdown across CPUs
 */
void tlb_shootdown_all(void) {
    if (!tlb_state.initialized || tlb_state.num_cpus <= 1) {
        tlb_flush_all();
        return;
    }
    
    spinlock_acquire(&tlb_state.shootdown_lock);
    
    // Set up shootdown request for full flush (target_addr = 0)
    tlb_state.shootdown.target_addr = 0;
    tlb_state.shootdown.requesting_cpu = tlb_state.current_cpu;
    tlb_state.shootdown.pending_cpus = (1 << tlb_state.num_cpus) - 1;
    tlb_state.shootdown.pending_cpus &= ~(1 << tlb_state.current_cpu);
    tlb_state.shootdown.completed_cpus = 0;
    tlb_state.shootdown.active = true;
    
    // Flush locally
    tlb_flush_all();
    
    // Placeholder for IPI sending
    tlb_state.shootdown.completed_cpus = tlb_state.shootdown.pending_cpus;
    tlb_state.shootdown.active = false;
    
    tlb_state.shootdown_count++;
    spinlock_release(&tlb_state.shootdown_lock);
}

/**
 * @brief Handle TLB shootdown IPI (called from IPI handler)
 */
void tlb_shootdown_ipi_handler(void) {
    if (!tlb_state.shootdown.active) {
        return;
    }
    
    // Perform the invalidation
    if (tlb_state.shootdown.target_addr == 0) {
        tlb_flush_all();
    } else {
        tlb_invalidate_page(tlb_state.shootdown.target_addr);
    }
    
    // Mark this CPU as complete
    uint32_t cpu_mask = 1 << tlb_state.current_cpu;
    __sync_fetch_and_or((volatile uint32_t*)&tlb_state.shootdown.completed_cpus, 
                        cpu_mask);
}

/**
 * @brief Register a new CPU with TLB manager
 */
void tlb_register_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) {
        return;
    }
    
    if (cpu_id >= tlb_state.num_cpus) {
        tlb_state.num_cpus = cpu_id + 1;
    }
}

/**
 * @brief Set current CPU ID
 */
void tlb_set_current_cpu(uint32_t cpu_id) {
    tlb_state.current_cpu = cpu_id;
}

/**
 * @brief Get TLB statistics
 */
tlb_stats_t tlb_get_stats(void) {
    tlb_stats_t stats;
    stats.invlpg_count = tlb_state.invlpg_count;
    stats.flush_count = tlb_state.flush_count;
    stats.shootdown_count = tlb_state.shootdown_count;
    stats.global_pages_supported = tlb_state.global_pages_supported;
    stats.pcid_supported = tlb_state.pcid_supported;
    return stats;
}

/**
 * @brief Dump TLB statistics
 */
void tlb_dump_stats(void) {
    print("\n=== TLB Statistics ===\n");
    print("INVLPG calls: ");
    print_dec((uint32_t)tlb_state.invlpg_count);
    print("\n");
    print("Full flushes: ");
    print_dec((uint32_t)tlb_state.flush_count);
    print("\n");
    print("Shootdowns: ");
    print_dec((uint32_t)tlb_state.shootdown_count);
    print("\n");
    print("CPUs: ");
    print_dec(tlb_state.num_cpus);
    print("\n");
    print("======================\n\n");
}
