/*
 * iommu.c - basic Intel VT-d / AMD-Vi detection and identity mapping stub.
 *
 * Complexity: real DMA remapping requires the full DMAR parser, root/context
 * table allocator, fault handler wiring, and access to the FSB interrupt
 * queue. This baseline records the presence of an IOMMU from the DMAR/IVRS
 * ACPI tables and exposes the map/unmap API so existing callers compile.
 * When ENABLE_IOMMU=0 (the default) every call is a no-op.
 */

#include "include/iommu.h"
#include "include/acpi_enhanced.h"
#include "include/string.h"
#include "include/debuglog.h"

static iommu_type_t g_iommu_type = IOMMU_TYPE_NONE;
static bool         g_iommu_initialized = false;

/* Walk DMAR looking for the "DMAR" signature - we only need to know it
 * exists to enable identity mapping. */
static iommu_type_t detect_iommu(void) {
    const acpi_table_info_t *info;

    info = acpi_find_table_by_signature(ACPI_SIG_DMAR);
    if (info) {
        debuglog(DEBUG_INFO, "IOMMU: DMAR table present, Intel VT-d\n");
        return IOMMU_TYPE_VTD;
    }
    info = acpi_find_table_by_signature("IVRS");
    if (info) {
        debuglog(DEBUG_INFO, "IOMMU: IVRS table present, AMD-Vi\n");
        return IOMMU_TYPE_AMD;
    }
    return IOMMU_TYPE_NONE;
}

int iommu_init(void) {
#if ENABLE_IOMMU
    if (g_iommu_initialized) return 0;
    g_iommu_type = detect_iommu();
    if (g_iommu_type == IOMMU_TYPE_NONE) {
        debuglog(DEBUG_INFO, "IOMMU: no DMAR/IVRS table - disabled\n");
        g_iommu_initialized = true;
        return 0;
    }
    /* Construct an identity mapping by leaving the context tables in their
     * boot-time state (BIOS already maps root, context, and root-table-
     * entries as identity for the kernel). A full reinit without copying
     * the BIOS setup risks dropping DMA on the next device reset, so for
     * now we only record presence and let userspace own table builds. */
    g_iommu_initialized = true;
    debuglog(DEBUG_INFO, "IOMMU: initialised in passthrough mode (%s)\n",
             g_iommu_type == IOMMU_TYPE_VTD ? "Intel VT-d" : "AMD-Vi");
    return 0;
#else
    debuglog(DEBUG_INFO, "IOMMU: support compiled out (ENABLE_IOMMU=0)\n");
    g_iommu_initialized = true;
    return 0;
#endif
}

iommu_type_t iommu_get_type(void) {
    return g_iommu_type;
}

bool iommu_is_present(void) {
#if ENABLE_IOMMU
    if (!g_iommu_initialized) iommu_init();
    return g_iommu_type != IOMMU_TYPE_NONE;
#else
    return false;
#endif
}

int iommu_map(uint32 dev_bdf, uint64 dma_addr, uint64 phys, uint64 size) {
#if ENABLE_IOMMU
    if (g_iommu_type == IOMMU_TYPE_NONE) return -1;
    /* Full implementation would walk the device's context-entry and build a
     * 2nd-level page-table mapping. Passthrough in this baseline. */
    (void)dev_bdf; (void)dma_addr; (void)phys; (void)size;
    return 0;
#else
    (void)dev_bdf; (void)dma_addr; (void)phys; (void)size;
    return 0;     /* pretend success: caller assumes DMA == phys */
#endif
}

int iommu_unmap(uint32 dev_bdf, uint64 dma_addr, uint64 size) {
#if ENABLE_IOMMU
    if (g_iommu_type == IOMMU_TYPE_NONE) return -1;
    (void)dev_bdf; (void)dma_addr; (void)size;
    return 0;
#else
    (void)dev_bdf; (void)dma_addr; (void)size;
    return 0;
#endif
}

void iommu_shutdown(void) {
#if ENABLE_IOMMU
    if (!g_iommu_initialized) return;
    /* On real teardown we'd flush the IOTLB, mask interrupts, and write the
     * disable bit. The host firmware is welcome back to its own state. */
    g_iommu_type = IOMMU_TYPE_NONE;
    g_iommu_initialized = false;
#else
    return;
#endif
}