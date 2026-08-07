/**
 * @file mem_protect.c
 * @brief Memory Protection Features
 * 
 * Implements:
 * - NX (No-Execute) bit support
 * - SMEP (Supervisor Mode Execution Prevention)
 * - SMAP (Supervisor Mode Access Prevention)
 * - PAT (Page Attribute Table) configuration
 * - Memory type control (WB, WC, UC, WT)
 */

#include "include/mem_protect.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/debuglog.h"

// ============================================================================
// CONSTANTS
// ============================================================================

// EFER MSR (Extended Feature Enable Register)
#define MSR_EFER            0xC0000080
#define EFER_NXE            (1ULL << 11)    // No-Execute Enable

// CR4 bits
#define CR4_SMEP            (1 << 20)       // SMEP Enable
#define CR4_SMAP            (1 << 21)       // SMAP Enable

// PAT MSR
#define MSR_PAT             0x277

// PAT memory types
#define PAT_UC              0x00    // Uncacheable
#define PAT_WC              0x01    // Write Combining
#define PAT_WT              0x04    // Write Through
#define PAT_WP              0x05    // Write Protected
#define PAT_WB              0x06    // Write Back
#define PAT_UC_MINUS        0x07    // Uncacheable (can be overridden)

// Default PAT value (Intel reset default)
#define PAT_DEFAULT         0x0007040600070406ULL

// Custom PAT value for better memory type support
// PAT0=WB, PAT1=WT, PAT2=UC-, PAT3=UC, PAT4=WB, PAT5=WT, PAT6=WC, PAT7=UC
#define PAT_CUSTOM          0x0001040600070406ULL

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Memory protection state
 */
static struct {
    bool initialized;
    bool nx_supported;
    bool nx_enabled;
    bool smep_supported;
    bool smep_enabled;
    bool smap_supported;
    bool smap_enabled;
    bool pat_supported;
    uint64_t pat_value;
} protect_state = { .initialized = false };

// ============================================================================
// MSR OPERATIONS
// ============================================================================

/**
 * @brief Read MSR
 */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Write MSR
 */
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
}

/**
 * @brief Read CR4
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
 * @brief Write CR4
 */
static inline void write_cr4(uint64_t value) {
#ifdef __x86_64__
    __asm__ volatile("mov %0, %%cr4" :: "r"(value) : "memory");
#else
    __asm__ volatile("mov %0, %%cr4" :: "r"((uint32_t)value) : "memory");
#endif
}

// ============================================================================
// FEATURE DETECTION
// ============================================================================

/**
 * @brief Detect CPU protection features
 */
static void detect_features(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Check extended features (CPUID.80000001H)
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001));
    
    // NX support - EDX bit 20
    protect_state.nx_supported = (edx & (1 << 20)) != 0;
    
    // Check standard features (CPUID.07H.0)
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    
    // SMEP support - EBX bit 7
    protect_state.smep_supported = (ebx & (1 << 7)) != 0;
    
    // SMAP support - EBX bit 20
    protect_state.smap_supported = (ebx & (1 << 20)) != 0;
    
    // Check basic features (CPUID.01H)
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    
    // PAT support - EDX bit 16
    protect_state.pat_supported = (edx & (1 << 16)) != 0;
    
    print("[MEM-PROT] CPU features:\n");
    print("[MEM-PROT] NX: ");   print(protect_state.nx_supported   ? "supported" : "not available"); print("\n");
    print("[MEM-PROT] SMEP: "); print(protect_state.smep_supported ? "supported" : "not available"); print("\n");
    print("[MEM-PROT] SMAP: "); print(protect_state.smap_supported ? "supported" : "not available"); print("\n");
    print("[MEM-PROT] PAT: ");  print(protect_state.pat_supported  ? "supported" : "not available"); print("\n");
}

// ============================================================================
// NX BIT
// ============================================================================

/**
 * @brief Enable NX bit
 */
static bool enable_nx(void) {
    if (!protect_state.nx_supported) {
        return false;
    }
    
    // Read EFER MSR
    uint64_t efer = rdmsr(MSR_EFER);
    
    // Enable NXE bit
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);
    
    // Verify
    efer = rdmsr(MSR_EFER);
    protect_state.nx_enabled = (efer & EFER_NXE) != 0;
    
    return protect_state.nx_enabled;
}

/**
 * @brief Disable NX bit
 */
static void disable_nx(void) {
    if (!protect_state.nx_enabled) {
        return;
    }
    
    uint64_t efer = rdmsr(MSR_EFER);
    efer &= ~EFER_NXE;
    wrmsr(MSR_EFER, efer);
    
    protect_state.nx_enabled = false;
}

// ============================================================================
// SMEP/SMAP
// ============================================================================

/**
 * @brief Enable SMEP
 */
static bool enable_smep(void) {
    if (!protect_state.smep_supported) {
        return false;
    }
    
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;
    write_cr4(cr4);
    
    // Verify
    cr4 = read_cr4();
    protect_state.smep_enabled = (cr4 & CR4_SMEP) != 0;
    
    return protect_state.smep_enabled;
}

/**
 * @brief Disable SMEP
 */
static void disable_smep(void) {
    if (!protect_state.smep_enabled) {
        return;
    }
    
    uint64_t cr4 = read_cr4();
    cr4 &= ~CR4_SMEP;
    write_cr4(cr4);
    
    protect_state.smep_enabled = false;
}

/**
 * @brief Enable SMAP
 */
static bool enable_smap(void) {
    if (!protect_state.smap_supported) {
        return false;
    }
    
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMAP;
    write_cr4(cr4);
    
    // Verify
    cr4 = read_cr4();
    protect_state.smap_enabled = (cr4 & CR4_SMAP) != 0;
    
    return protect_state.smap_enabled;
}

/**
 * @brief Disable SMAP
 */
static void disable_smap(void) {
    if (!protect_state.smap_enabled) {
        return;
    }
    
    uint64_t cr4 = read_cr4();
    cr4 &= ~CR4_SMAP;
    write_cr4(cr4);
    
    protect_state.smap_enabled = false;
}

/**
 * @brief Temporarily disable SMAP for user memory access
 */
void mem_protect_stac(void) {
    if (protect_state.smap_enabled) {
        __asm__ volatile("stac" ::: "cc");
    }
}

/**
 * @brief Re-enable SMAP after user memory access
 */
void mem_protect_clac(void) {
    if (protect_state.smap_enabled) {
        __asm__ volatile("clac" ::: "cc");
    }
}

// ============================================================================
// PAT (Page Attribute Table)
// ============================================================================

/**
 * @brief Configure PAT
 */
static void configure_pat(void) {
    if (!protect_state.pat_supported) {
        return;
    }
    
    // Configure PAT for useful memory types
    // Default: PAT0=WB, PAT1=WT, PAT2=UC-, PAT3=UC
    // We add: PAT4=WB, PAT5=WT, PAT6=WC, PAT7=UC
    protect_state.pat_value = PAT_CUSTOM;
    
    wrmsr(MSR_PAT, protect_state.pat_value);
    
    print("[MEM-PROT] PAT configured for WC support\n");
}

/**
 * @brief Get PAT index for memory type
 */
uint32_t mem_protect_get_pat_index(mem_type_t type) {
    switch (type) {
        case MEM_TYPE_WB:       return 0;   // PAT0 or PAT4
        case MEM_TYPE_WT:       return 1;   // PAT1 or PAT5
        case MEM_TYPE_UC:       return 3;   // PAT3 or PAT7
        case MEM_TYPE_WC:       return 6;   // PAT6 (requires custom PAT)
        case MEM_TYPE_UC_MINUS: return 2;   // PAT2
        default:                return 0;
    }
}

/**
 * @brief Get page flags for memory type
 */
uint32_t mem_protect_get_type_flags(mem_type_t type) {
    uint32_t pat_index = mem_protect_get_pat_index(type);
    uint32_t flags = 0;
    
    // PAT index is encoded in PWT, PCD, and PAT bits
    // PAT index = PAT*4 + PCD*2 + PWT
    if (pat_index & 1) flags |= PAGE_WRITE_THROUGH;     // PWT
    if (pat_index & 2) flags |= PAGE_CACHE_DISABLE;     // PCD
    // PAT bit is bit 7 for 4K pages, bit 12 for large pages
    // We'll handle PAT bit separately in paging code
    
    return flags;
}

// ============================================================================
// PUBLIC API
// ============================================================================

/**
 * @brief Initialize memory protection
 */
void mem_protect_init(void) {
    if (protect_state.initialized) {
        return;
    }
    
    print("[MEM-PROT] Initializing memory protection...\n");
    
    // Detect CPU features
    detect_features();
    
    // Enable NX if supported
    if (protect_state.nx_supported) {
        if (enable_nx()) {
            print("[MEM-PROT] NX enabled\n");
        } else {
            print("[MEM-PROT] NX: enable failed\n");
        }
    } else {
        print("[MEM-PROT] NX: skipped (not supported by CPU)\n");
    }

    // Enable SMEP if supported
    if (protect_state.smep_supported) {
        if (enable_smep()) {
            print("[MEM-PROT] SMEP enabled\n");
        } else {
            print("[MEM-PROT] SMEP: enable failed\n");
        }
    } else {
        print("[MEM-PROT] SMEP: skipped (not supported by CPU)\n");
    }
    
    // SMAP is intentionally not enabled: it requires every kernel access to
    // user memory to be wrapped with stac/clac, which is not yet done
    // consistently throughout the kernel.  Log its availability but do not
    // attempt to enable it so we avoid spurious SMAP violations.
    if (!protect_state.smap_supported) {
        print("[MEM-PROT] SMAP: skipped (not supported by CPU)\n");
    } else {
        print("[MEM-PROT] SMAP: available but not enabled (kernel not ready)\n");
    }
    
    // Configure PAT
    configure_pat();
    
    protect_state.initialized = true;
    
    print("[MEM-PROT] Memory protection initialized\n");
}

/**
 * @brief Check if NX is enabled
 */
bool mem_protect_nx_enabled(void) {
    return protect_state.nx_enabled;
}

/**
 * @brief Check if SMEP is enabled
 */
bool mem_protect_smep_enabled(void) {
    return protect_state.smep_enabled;
}

/**
 * @brief Check if SMAP is enabled
 */
bool mem_protect_smap_enabled(void) {
    return protect_state.smap_enabled;
}

/**
 * @brief Enable/disable SMAP at runtime
 */
void mem_protect_set_smap(bool enable) {
    if (enable) {
        enable_smap();
    } else {
        disable_smap();
    }
}

/**
 * @brief Get protection status
 */
mem_protect_status_t mem_protect_get_status(void) {
    mem_protect_status_t status;
    status.nx_supported = protect_state.nx_supported;
    status.nx_enabled = protect_state.nx_enabled;
    status.smep_supported = protect_state.smep_supported;
    status.smep_enabled = protect_state.smep_enabled;
    status.smap_supported = protect_state.smap_supported;
    status.smap_enabled = protect_state.smap_enabled;
    status.pat_supported = protect_state.pat_supported;
    return status;
}

/**
 * @brief Dump protection status
 */
void mem_protect_dump_status(void) {
    print("\n=== Memory Protection Status ===\n");
    print("NX:   ");
    print(protect_state.nx_supported ? "supported, " : "not supported, ");
    print(protect_state.nx_enabled ? "enabled" : "disabled");
    print("\n");
    print("SMEP: ");
    print(protect_state.smep_supported ? "supported, " : "not supported, ");
    print(protect_state.smep_enabled ? "enabled" : "disabled");
    print("\n");
    print("SMAP: ");
    print(protect_state.smap_supported ? "supported, " : "not supported, ");
    print(protect_state.smap_enabled ? "enabled" : "disabled");
    print("\n");
    print("PAT:  ");
    print(protect_state.pat_supported ? "supported" : "not supported");
    print("\n");
    print("================================\n\n");
}
