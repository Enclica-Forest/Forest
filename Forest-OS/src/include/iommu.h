#ifndef IOMMU_H
#define IOMMU_H

#include "types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ENABLE_IOMMU
#  define ENABLE_IOMMU 0
#endif

typedef enum {
    IOMMU_TYPE_NONE = 0,
    IOMMU_TYPE_VTD  = 1,    /* Intel VT-d */
    IOMMU_TYPE_AMD  = 2     /* AMD-Vi (IVRS) */
} iommu_type_t;

/* Sets up identity mapping for the kernel so device DMA works. Off by
 * default (ENABLE_IOMMU=0); when disabled every call returns success
 * but performs nothing so callers link cleanly. */
int      iommu_init(void);
iommu_type_t iommu_get_type(void);
bool     iommu_is_present(void);

/* Map a contiguous device DMA address range to a kernel physical range.
 * Returns 0 on success, -1 if IOMMU is absent/disabled, errno on error. */
int      iommu_map(uint32 dev_bdf, uint64 dma_addr, uint64 phys, uint64 size);
int      iommu_unmap(uint32 dev_bdf, uint64 dma_addr, uint64 size);

/* Teardown; safe to call when never initialised. */
void     iommu_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IOMMU_H */