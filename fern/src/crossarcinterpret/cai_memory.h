/*
 * cai_memory.h - Guest memory management for the cross-architecture interpreter
 *
 * Declares a standalone guest address space type (cai_address_space_t) backed
 * by per-region kmalloc() allocations.  This layer sits between the ELF loader
 * and the architecture-specific step functions; it is intentionally decoupled
 * from the flat-pool design inside cai_context_t so that the two models can
 * coexist during a phased migration.
 *
 * Typical lifecycle
 * -----------------
 *   cai_address_space_t *as = cai_as_create();
 *   cai_as_map(as, 0x08048000, 0x10000, CAI_MEM_READ | CAI_MEM_EXEC);
 *   ...
 *   uint8_t *host = cai_as_translate(as, guest_pc, 4);
 *   cai_as_destroy(as);
 */

#ifndef CAI_MEMORY_H
#define CAI_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Protection flags  (match crossarcinterpret.h CAI_MEM_* values)
 * ========================================================================= */

#ifndef CAI_MEM_READ
#define CAI_MEM_READ  0x1
#define CAI_MEM_WRITE 0x2
#define CAI_MEM_EXEC  0x4
#endif

/* =========================================================================
 * Guest memory region – singly-linked list node
 * ========================================================================= */

typedef struct cai_mem_region {
    uint64_t              guest_base; /* First guest virtual address          */
    uint8_t              *host_ptr;   /* Host kernel pointer to backing store */
    size_t                size;       /* Length in bytes                      */
    uint32_t              flags;      /* CAI_MEM_READ | CAI_MEM_WRITE | ...   */
    struct cai_mem_region *next;      /* Next region in list (NULL = end)     */
} cai_mem_region_t;

/* =========================================================================
 * Guest address space
 * ========================================================================= */

typedef struct cai_address_space {
    cai_mem_region_t *regions;    /* Head of the region linked list           */
    size_t            total_size; /* Sum of all region sizes (informational)  */
} cai_address_space_t;

/* =========================================================================
 * Address-space management
 * ========================================================================= */

/*
 * cai_as_create - Allocate an empty guest address space.
 * Returns NULL on allocation failure.
 */
cai_address_space_t *cai_as_create(void);

/*
 * cai_as_destroy - Free all regions and the address-space descriptor itself.
 * Safe to call with NULL.
 */
void cai_as_destroy(cai_address_space_t *as);

/*
 * cai_as_map - Allocate @size bytes of host memory and register it as a guest
 * region starting at @guest_addr with protection @flags.
 *
 * Returns 0 on success, -1 on error (allocation failure / NULL as).
 */
int cai_as_map(cai_address_space_t *as, uint64_t guest_addr,
               size_t size, uint32_t flags);

/*
 * cai_as_translate - Translate a guest virtual address to a host pointer.
 *
 * Checks that the entire [guest_addr, guest_addr+access_size) range falls
 * within a single mapped region.
 *
 * Returns a host pointer on success, NULL if the address is unmapped or the
 * access would cross a region boundary.
 */
void *cai_as_translate(cai_address_space_t *as, uint64_t guest_addr,
                       size_t access_size);

/* =========================================================================
 * Typed read primitives
 *
 * Each returns the value at @addr, or 0 on fault (out-of-bounds access is
 * logged via debuglog).
 * ========================================================================= */

uint8_t  cai_mem_r8 (cai_address_space_t *as, uint64_t addr);
uint16_t cai_mem_r16(cai_address_space_t *as, uint64_t addr);
uint32_t cai_mem_r32(cai_address_space_t *as, uint64_t addr);
uint64_t cai_mem_r64(cai_address_space_t *as, uint64_t addr);

/* =========================================================================
 * Typed write primitives
 *
 * Out-of-bounds writes are silently dropped after a debuglog entry.
 * ========================================================================= */

void cai_mem_w8 (cai_address_space_t *as, uint64_t addr, uint8_t  v);
void cai_mem_w16(cai_address_space_t *as, uint64_t addr, uint16_t v);
void cai_mem_w32(cai_address_space_t *as, uint64_t addr, uint32_t v);
void cai_mem_w64(cai_address_space_t *as, uint64_t addr, uint64_t v);

#endif /* CAI_MEMORY_H */
