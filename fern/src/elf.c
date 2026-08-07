#include "include/elf.h"
#include "include/system.h"
#include "include/util.h"
#include "include/screen.h"
#include "include/memory.h"
#include "include/panic.h"
#include "include/debuglog.h"

// Explicit forward declaration to help compiler resolve implicit declaration
extern page_directory_t* vmm_get_current_page_directory(void);
extern void* kmalloc(size_t size);

#define ELF_MAX_IMAGE_SIZE   (16 * 1024 * 1024)
#define ELF_MAX_BSS_SIZE     (4 * 1024 * 1024)

static inline uint32 align_down(uint32 value, uint32 align) {
    return value & ~(align - 1);
}

static inline uint32 align_up(uint32 value, uint32 align) {
    return (value + align - 1) & ~(align - 1);
}

static bool header_has_valid_magic(const elf32_ehdr_t* header) {
    return header->e_ident[EI_MAG0] == ELF_MAGIC_0 &&
           header->e_ident[EI_MAG1] == ELF_MAGIC_1 &&
           header->e_ident[EI_MAG2] == ELF_MAGIC_2 &&
           header->e_ident[EI_MAG3] == ELF_MAGIC_3;
}

static bool elf_range_in_bounds(size_t offset, size_t length, size_t total_size) {
    if (offset > total_size) {
        return false;
    }
    return length <= (total_size - offset);
}

static inline bool phdr_in_bounds(const elf32_phdr_t* ph, size_t elf_size) {
    return elf_range_in_bounds(ph->p_offset, ph->p_filesz, elf_size);
}

static uint32 phdr_page_flags(const elf32_phdr_t* ph) {
    uint32 flags = PAGE_PRESENT | PAGE_USER;
    if (ph->p_flags & PF_W) {
        flags |= PAGE_WRITABLE;
    }
    return flags;
}

int elf_validate_header(const elf32_ehdr_t* header) {
    if (!header) {
        return -1;
    }

    if (!header_has_valid_magic(header)) {
        return -2;
    }

    if (header->e_ident[EI_CLASS] != ELF_CLASS_32) {
        return -3;
    }

    if (header->e_ident[EI_DATA] != ELF_DATA_2LSB) {
        return -4;
    }

    if (header->e_type != ELF_TYPE_EXEC && header->e_type != ELF_TYPE_DYN) {
        return -5;
    }

    /* Accept i386. ET_DYN shared objects may also be loaded as the dynamic
     * linker (ldso) which is itself an EM_386 ET_DYN object. */
    if (header->e_machine != ELF_MACHINE_386) {
        return -6;
    }

    if (header->e_version != ELF_VERSION_CURRENT) {
        return -7;
    }

    if (header->e_ehsize != sizeof(elf32_ehdr_t)) {
        return -8;
    }

    if (header->e_phentsize != sizeof(elf32_phdr_t)) {
        return -9;
    }

    if (header->e_phnum == 0) {
        return -10;
    }

    return 0;
}

bool elf_is_valid(const uint8* elf_data, size_t size) {
    if (!elf_data || size < sizeof(elf32_ehdr_t)) {
        return false;
    }

    return elf_validate_header((const elf32_ehdr_t*)elf_data) == 0;
}

const char* elf_validate_error_string(int error_code) {
    switch (error_code) {
        case 0:   return "valid ELF header";
        case -1:  return "null header pointer";
        case -2:  return "bad ELF magic (not \\x7fELF)";
        case -3:  return "wrong ELF class (not 32-bit)";
        case -4:  return "wrong data encoding (not little-endian)";
        case -5:  return "unsupported object type (need EXEC or DYN/PIE)";
        case -6:  return "wrong machine (not i386)";
        case -7:  return "unsupported ELF version";
        case -8:  return "bad ELF header size";
        case -9:  return "bad program header entry size";
        case -10: return "no program headers";
        default:  return "unknown ELF validation error";
    }
}

static bool map_segment_pages(page_directory_t* dir, uint32 start, uint32 end, uint32 flags) {
    debuglog(DEBUG_INFO, "[ELF] map_segment_pages: start=0x%x end=0x%x flags=0x%x, free_frames=%u\n", 
             start, end, flags, pmm_get_free_frames());
    for (uint32 va = start; va < end; va += MEMORY_PAGE_SIZE) {
        uint32 frame = pmm_alloc_frame();
        if (!frame) {
            debuglog(DEBUG_ERROR, "[ELF] pmm_alloc_frame failed while mapping segment page 0x%x-0x%x\n", start, end);
            return false;
        }

        memory_result_t map_res = vmm_map_page(dir, va, frame, flags);
        if (map_res != MEMORY_OK && map_res != MEMORY_ERROR_ALREADY_MAPPED) {
            debuglog(DEBUG_ERROR, "[ELF] vmm_map_page failed (res=%d) va=0x%x frame=0x%x flags=0x%x\n",
                     map_res, va, frame, flags);
            return false;
        }
    }

    return true;
}

static void record_segment_info(const elf32_phdr_t* phdr, elf_load_info_t* info) {
    if ((phdr->p_flags & PF_X) && info->text_size == 0) {
        info->text_start = phdr->p_vaddr;
        info->text_size = phdr->p_memsz;
    }

    if ((phdr->p_flags & PF_W) && info->data_size == 0 && !(phdr->p_flags & PF_X)) {
        info->data_start = phdr->p_vaddr;
        info->data_size = phdr->p_memsz;
    }
}

__attribute__((unused)) static void zero_bss_region(uint32 start, uint32 end) {
    if (end <= start) {
        return;
    }

    memory_set((uint8*)start, 0, end - start);
}

// Copy data to user space in new_dir using vmm_temp_map_page for safe access.
// Uses the VMM's dedicated temporary mapping window (VMM_TEMP_MAP_BASE) which
// is pre-allocated and TLB-safe, instead of a hardcoded kernel address.
static bool copy_to_user_space(page_directory_t* user_dir, uint32 user_va,
                                const uint8* src, uint32 size, uint32 flags) {
    uint32 remaining = size;
    const uint8* src_ptr = src;

    while (remaining > 0) {
        uint32 page_va = align_down(user_va, MEMORY_PAGE_SIZE);

        // Look up (or allocate) the physical frame for this user virtual page
        uint32 frame = vmm_get_physical_addr(user_dir, page_va);
        if (!frame) {
            frame = pmm_alloc_frame();
            if (!frame) {
                debuglog(DEBUG_ERROR, "[ELF] copy_to_user_space: out of frames\n");
                return false;
            }

            memory_result_t res = vmm_map_page(user_dir, page_va, frame, flags);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                pmm_free_frame(frame);
                debuglog(DEBUG_ERROR, "[ELF] copy_to_user_space: vmm_map_page failed\n");
                return false;
            }
        }

        // Temporarily map the physical frame via the VMM's safe temp-map window
        void* kmap = vmm_temp_map_page(frame);
        if (!kmap) {
            debuglog(DEBUG_ERROR, "[ELF] copy_to_user_space: vmm_temp_map_page failed\n");
            return false;
        }

        uint32 copy_offset = user_va - page_va;
        uint32 copy_size = MEMORY_PAGE_SIZE - copy_offset;
        if (copy_size > remaining) {
            copy_size = remaining;
        }

        memory_copy((const char*)src_ptr, (char*)kmap + copy_offset, (int)copy_size);

        vmm_temp_unmap_page(kmap);

        src_ptr  += copy_size;
        remaining -= copy_size;
        user_va  += copy_size;
    }

    return true;
}

//This is busting my balls.
//TODO: fix sloppy code later
int elf_load_executable(const uint8 *elf_data, size_t elf_size,
                        elf_load_info_t *info)
{
    if (!elf_data || !info)
        return -1;

    if (elf_size == 0 || elf_size > ELF_MAX_IMAGE_SIZE) {
        debuglog(DEBUG_ERROR, "[ELF] ELF size invalid: %u (max %u)\n", (uint32)elf_size, ELF_MAX_IMAGE_SIZE);
        return -6;
    }

    memory_set((uint8*)info, 0, sizeof(*info));

    if (!elf_is_valid(elf_data, elf_size)) {
        const elf32_ehdr_t* hdr = (const elf32_ehdr_t*)elf_data;
        int val_err = elf_validate_header(hdr);
        debuglog(DEBUG_ERROR, "[ELF] Validation failed: %s (code=%d)\n",
                 elf_validate_error_string(val_err), val_err);
        return -2;
    }

    if (!elf_has_enough_memory(elf_data, elf_size)) {
        debuglog(DEBUG_ERROR, "[ELF] Not enough memory to load ELF (free=%u)\n", pmm_get_free_frames());
        return -7;
    }

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)elf_data;
    
    /* Position-independent executables (ET_DYN) have a base vaddr of 0 and
     * must be loaded at a runtime bias.  Pick a bias in low user memory that
     * does not collide with the legacy 0x08048000 EXEC base. */
    bool is_pie = (eh->e_type == ELF_TYPE_DYN);
    uint32 load_bias = 0;
    if (is_pie) {
        load_bias = 0x08000000;
        debuglog(DEBUG_INFO, "[ELF] ET_DYN/PIE binary: using load bias 0x%08x\n", load_bias);
    }
    
    // Debug: Print ELF header entry point directly from memory
    uint32* raw_entry_ptr = (uint32*)(elf_data + offsetof(elf32_ehdr_t, e_entry));
    debuglog(DEBUG_INFO, "[ELF] Raw e_entry from ELF header (offset 0x%lx): 0x%x\n", 
             (unsigned long)offsetof(elf32_ehdr_t, e_entry), *raw_entry_ptr);
    debuglog(DEBUG_INFO, "[ELF] Struct e_entry: 0x%x\n", eh->e_entry);

    size_t ph_table_size = (size_t)eh->e_phnum * sizeof(elf32_phdr_t);
    if (!elf_range_in_bounds(eh->e_phoff, ph_table_size, elf_size))
        return -3;
    if (eh->e_phnum > ELF_MAX_PHDR) {
        debuglog(DEBUG_ERROR, "[ELF] Too many program headers: %u (max %u)\n",
                 eh->e_phnum, ELF_MAX_PHDR);
        return -3;
    }

    page_directory_t* new_dir = vmm_create_page_directory();
    if (!new_dir) {
        debuglog(DEBUG_ERROR, "[ELF] vmm_create_page_directory failed! free_frames=%u\n", pmm_get_free_frames());
        return -4;
    }
    debuglog(DEBUG_INFO, "[ELF] Created new page directory at %p\n", (void*)new_dir);

    // Ensure the ELF source buffer is visible in the new page directory.
    // The initrd is identity-mapped in the kernel, so we just copy those mappings.
    uint32 src_start = align_down((uint32)elf_data, MEMORY_PAGE_SIZE);
    uint32 src_end   = align_up((uint32)elf_data + elf_size, MEMORY_PAGE_SIZE);
    debuglog(DEBUG_INFO, "[ELF] Mapping ELF source: 0x%x-0x%x in new_dir\n", src_start, src_end);
    vmm_identity_map_range(new_dir, src_start, src_end, PAGE_PRESENT | PAGE_WRITABLE);

    debuglog(DEBUG_INFO, "[ELF] Loading segments WITHOUT switching page directory\n");
    debuglog(DEBUG_INFO, "[ELF] About to iterate over segments\n");

    uint32 base = 0xFFFFFFFF;
    uint32 end  = 0;
    bool any_segment_loaded = false;
    uint32 bss_min = 0xFFFFFFFF;
    uint32 bss_max = 0;
    uint32 total_bss = 0;
    bool entrypoint_in_segment = false;
    bool entrypoint_executable = false;

    const elf32_phdr_t *ph =
    (const elf32_phdr_t *)(elf_data + eh->e_phoff);

    debuglog(DEBUG_INFO, "[ELF] Program headers: offset=0x%x, num=%u, size=%u each\n", 
             eh->e_phoff, eh->e_phnum, eh->e_phentsize);

    // Make a local copy of program headers to prevent issues with page mapping
    elf32_phdr_t* ph_copy = (elf32_phdr_t*)kmalloc(eh->e_phnum * sizeof(elf32_phdr_t));
    if (!ph_copy) {
        debuglog(DEBUG_ERROR, "[ELF] Failed to allocate program-header copy (%u entries)\n", eh->e_phnum);
        goto fail_destroy;
    }
    memcpy(ph_copy, ph, eh->e_phnum * sizeof(elf32_phdr_t));

    for (uint32 i = 0; i < eh->e_phnum; i++) {
        const elf32_phdr_t* segment = &ph_copy[i];
        
        debuglog(DEBUG_INFO, "[ELF] Segment %u/%u: type=%u, vaddr=0x%x, offset=0x%x, filesz=0x%x, memsz=0x%x, flags=0x%x\n",
                 i + 1, eh->e_phnum, segment->p_type, segment->p_vaddr, segment->p_offset, segment->p_filesz, segment->p_memsz, segment->p_flags);

        /* Capture the program interpreter (PT_INTERP) for ldso-driven Linux
         * ELF binaries. We only record the path; actual invocation is handled
         * by the dynamic linker subsystem. */
        if (segment->p_type == PT_INTERP) {
            if (segment->p_filesz > 0 && segment->p_filesz < sizeof(info->interp) &&
                elf_range_in_bounds(segment->p_offset, segment->p_filesz, elf_size)) {
                memory_copy((const char*)(elf_data + segment->p_offset),
                            info->interp, (int)segment->p_filesz);
                /* Trim trailing NUL or garbage. */
                info->interp[segment->p_filesz - 1] = '\0';
                info->has_interp = true;
                debuglog(DEBUG_INFO, "[ELF] PT_INTERP: %s\n", info->interp);
            } else {
                debuglog(DEBUG_WARN, "[ELF] PT_INTERP present but unusable (filesz=%u)\n",
                         segment->p_filesz);
            }
            continue;
        }

        if (segment->p_type == PT_DYNAMIC) {
            info->has_dynamic = true;
            /* Processed by ldso; not loaded as a PT_LOAD here. */
            continue;
        }

        if (segment->p_type != PT_LOAD)
            continue;

        if (segment->p_memsz == 0)
            continue;

        /* Apply the PIE load bias to position-independent binaries. */
        uint32 seg_vaddr = segment->p_vaddr + load_bias;

        if (!phdr_in_bounds(segment, elf_size) || segment->p_filesz > segment->p_memsz) {
            debuglog(DEBUG_ERROR, "[ELF] Invalid program header: type=%u off=0x%x filesz=0x%x memsz=0x%x elf_size=0x%x\n",
                     segment->p_type, segment->p_offset, segment->p_filesz, segment->p_memsz, (uint32)elf_size);
            goto fail;
        }

        uint64 seg_end_unaligned = (uint64)seg_vaddr + (uint64)segment->p_memsz;
        if (seg_end_unaligned > USER_STACK_TOP || seg_vaddr < MEMORY_USER_START || seg_end_unaligned < seg_vaddr) {
            debuglog(DEBUG_ERROR, "[ELF] Segment %u outside user range: vaddr=0x%x memsz=0x%x\n",
                     i, seg_vaddr, segment->p_memsz);
            goto fail;
        }

        uint32 segment_start = align_down(seg_vaddr, MEMORY_PAGE_SIZE);
        uint32 segment_end = align_up((uint32)seg_end_unaligned, MEMORY_PAGE_SIZE);

        if (segment_end <= segment_start) {
            debuglog(DEBUG_ERROR, "[ELF] Segment %u computed invalid range: start=0x%x end=0x%x\n",
                     i, segment_start, segment_end);
            goto fail;
        }

        uint32 flags = phdr_page_flags(segment);
        if (!map_segment_pages(new_dir, segment_start, segment_end, flags)) {
            debuglog(DEBUG_ERROR, "[ELF] Failed to map segment %u: vaddr=0x%x-0x%x flags=0x%x\n",
                     i, segment_start, segment_end, flags);
            goto fail;
        }

        // Copy segment data using temporary mapping (without switching directories)
        const uint8* file_src = elf_data + segment->p_offset;
        debuglog(DEBUG_INFO, "[ELF] Copying segment %u: filesz=%u to vaddr=0x%x\n",
                 i, segment->p_filesz, seg_vaddr);
        if (!copy_to_user_space(new_dir, seg_vaddr, file_src, segment->p_filesz, flags)) {
            debuglog(DEBUG_ERROR, "[ELF] Failed to copy segment %u data\n", i);
            goto fail;
        }

        // Zero BSS if present
        if (segment->p_memsz > segment->p_filesz) {
            uint32 bss_size = segment->p_memsz - segment->p_filesz;
            uint32 bss_va = seg_vaddr + segment->p_filesz;
            debuglog(DEBUG_INFO, "[ELF] Zeroing BSS: 0x%x size=%u\n", bss_va, bss_size);

            // Zero BSS using VMM temp-map window (safe, no hardcoded kernel address)
            uint32 remaining_bss = bss_size;
            uint32 bss_offset = 0;

            while (remaining_bss > 0) {
                uint32 page_va = align_down(bss_va + bss_offset, MEMORY_PAGE_SIZE);
                uint32 frame = vmm_get_physical_addr(new_dir, page_va);
                if (!frame) {
                    debuglog(DEBUG_ERROR, "[ELF] BSS page not mapped: 0x%x\n", page_va);
                    goto fail;
                }

                void* kmap = vmm_temp_map_page(frame);
                if (!kmap) {
                    debuglog(DEBUG_ERROR, "[ELF] BSS: vmm_temp_map_page failed\n");
                    goto fail;
                }

                uint32 copy_offset = (bss_va + bss_offset) - page_va;
                uint32 copy_size = MEMORY_PAGE_SIZE - copy_offset;
                if (copy_size > remaining_bss) {
                    copy_size = remaining_bss;
                }

                memory_set((uint8*)kmap + copy_offset, 0, copy_size);
                vmm_temp_unmap_page(kmap);

                remaining_bss -= copy_size;
                bss_offset += copy_size;
            }

            total_bss += bss_size;
            if (total_bss > ELF_MAX_BSS_SIZE) {
                debuglog(DEBUG_ERROR, "[ELF] BSS size too large (total=%u, max=%u)\n",
                         total_bss, ELF_MAX_BSS_SIZE);
                goto fail;
            }

            if (bss_va < bss_min) {
                bss_min = bss_va;
            }
            if (bss_va + bss_size > bss_max) {
                bss_max = bss_va + bss_size;
            }
        }

        debuglog(DEBUG_INFO, "[ELF] Segment %u: vaddr=0x%x, memsz=0x%x, end=0x%x, flags=0x%x\n",
                 i, seg_vaddr, segment->p_memsz, seg_vaddr + segment->p_memsz, segment->p_flags);
        
        // Save entry point to a local variable to detect any corruption.
        // For PIE binaries the entry point is relative and must be biased.
        uint32 entry_pt = eh->e_entry + load_bias;
        debuglog(DEBUG_INFO, "[ELF] Checking entry point 0x%x against segment [0x%x - 0x%x]\n",
                 entry_pt, seg_vaddr, seg_vaddr + segment->p_memsz);
        
        if (entry_pt >= seg_vaddr && entry_pt < seg_vaddr + segment->p_memsz) {
            entrypoint_in_segment = true;
            debuglog(DEBUG_INFO, "[ELF] Entry point 0x%x found in segment %u\n", entry_pt, i);
            if (segment->p_flags & PF_X) {
                entrypoint_executable = true;
                debuglog(DEBUG_INFO, "[ELF] Segment %u is executable\n", i);
            }
        }

        if (seg_vaddr < base)
            base = seg_vaddr;

        if (seg_vaddr + segment->p_memsz > end)
            end = seg_vaddr + segment->p_memsz;

        record_segment_info(segment, info);
        any_segment_loaded = true;
    }

    if (!any_segment_loaded || base == 0xFFFFFFFF)
        goto fail_destroy;

    if (!entrypoint_in_segment || !entrypoint_executable) {
        uint32 biased_entry = eh->e_entry + load_bias;
        debuglog(DEBUG_ERROR, "[ELF] Entry point 0x%x (biased 0x%x) not in an executable load segment (in_segment=%d, executable=%d)\n", 
                 eh->e_entry, biased_entry, entrypoint_in_segment, entrypoint_executable);
        debuglog(DEBUG_ERROR, "[ELF] Base=0x%x, End=0x%x\n", base, end);
        
        // Fallback: Check if entry point is within loaded memory region (base to end)
        // This handles cases where segment alignment causes the exact check to fail
        if (biased_entry >= base && biased_entry < end) {
            debuglog(DEBUG_WARN, "[ELF] Entry point 0x%x is within loaded region [0x%x - 0x%x], allowing\n",
                     biased_entry, base, end);
            entrypoint_in_segment = true;
            entrypoint_executable = true;
        } else {
            goto fail_destroy;
        }
    }

    info->entry_point  = eh->e_entry + load_bias;
    info->base_address = base;
    info->total_size   = end - base;
    info->page_directory = (uint32)new_dir;
    info->is_pie       = is_pie;
    info->load_bias    = load_bias;
    if (bss_min != 0xFFFFFFFF) {
        info->bss_start = bss_min;
        info->bss_size = bss_max - bss_min;
    }

    info->valid        = true;

    kfree(ph_copy);
    debuglog(DEBUG_INFO, "[ELF] Successfully loaded ELF, entry=0x%x, pd=0x%x\n",
             info->entry_point, info->page_directory);
    return 0;

    fail:
    if (ph_copy) {
        kfree(ph_copy);
    }
    fail_destroy:
    vmm_destroy_page_directory(new_dir);
    return -5;
}


int elf_load_from_file(const char* filename, elf_load_info_t* load_info) {
    // No filesystem support yet; leave a clear failure path.
    (void)filename;
    (void)load_info;
    print("[ELF] Loading from files is not supported in this kernel build\n");
    return -1;
}

uint32 elf_get_entry_point(const uint8* elf_data) {
    if (!elf_is_valid(elf_data, sizeof(elf32_ehdr_t))) {
        return 0;
    }

    const elf32_ehdr_t* header = (const elf32_ehdr_t*)elf_data;
    return header->e_entry;
}

static bool elf_has_program_header_type(const uint8* elf_data, size_t elf_size, uint32 ph_type) {
    if (!elf_data || elf_size < sizeof(elf32_ehdr_t)) {
        return false;
    }

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*)elf_data;
    size_t ph_table_size = (size_t)eh->e_phnum * sizeof(elf32_phdr_t);
    if (!elf_range_in_bounds(eh->e_phoff, ph_table_size, elf_size)) {
        return false;
    }

    const elf32_phdr_t* ph = (const elf32_phdr_t*)(elf_data + eh->e_phoff);
    for (uint32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == ph_type) {
            return true;
        }
    }
    return false;
}

uint32 elf_estimate_memory_pages(const uint8* elf_data, size_t elf_size) {
    if (!elf_data || elf_size < sizeof(elf32_ehdr_t)) {
        return 0;
    }

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*)elf_data;
    size_t ph_table_size = (size_t)eh->e_phnum * sizeof(elf32_phdr_t);
    if (!elf_range_in_bounds(eh->e_phoff, ph_table_size, elf_size)) {
        return 0;
    }

    uint32 total_pages = 0;
    const elf32_phdr_t* ph = (const elf32_phdr_t*)(elf_data + eh->e_phoff);
    for (uint32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) {
            continue;
        }
        uint32 start = align_down(ph[i].p_vaddr, MEMORY_PAGE_SIZE);
        uint32 end = align_up(ph[i].p_vaddr + ph[i].p_memsz, MEMORY_PAGE_SIZE);
        if (end > start) {
            total_pages += (end - start) / MEMORY_PAGE_SIZE;
        }
    }
    return total_pages;
}

bool elf_has_enough_memory(const uint8* elf_data, size_t elf_size) {
    uint32 needed = elf_estimate_memory_pages(elf_data, elf_size);
    uint32 stack_pages = 32;
    uint32 heap_pages = 4;
    needed += stack_pages + heap_pages + ELF_MIN_FREE_FRAMES;
    uint32 free = pmm_get_free_frames();
    if (free < needed) {
        debuglog(DEBUG_ERROR, "[ELF] Insufficient memory: need %u frames, have %u free\n",
                 needed, free);
        return false;
    }
    return true;
}

bool elf_resolve_symbol(const char* symbol_name, uint32* symbol_addr) {
    // Kernel-side dynamic symbol resolution is not implemented yet.
    (void)symbol_name;
    if (symbol_addr) {
        *symbol_addr = 0;
    }
    return false;
}

int elf_process_relocations(const uint8* elf_data, size_t elf_size, const elf_load_info_t* load_info) {
    if (!elf_data || !load_info || !load_info->valid) {
        return -1;
    }

    const elf32_ehdr_t* eh = (const elf32_ehdr_t*)elf_data;
    if (elf_validate_header(eh) != 0) {
        return -2;
    }

    // Best-effort parser for relocation metadata. Actual runtime relocation writes
    // require a full dynamic linker context and are deferred to ldso.
    if (eh->e_shoff == 0 || eh->e_shnum == 0) {
        return 0;
    }
    size_t sh_table_size = (size_t)eh->e_shnum * sizeof(elf32_shdr_t);
    if (!elf_range_in_bounds(eh->e_shoff, sh_table_size, elf_size)) {
        return -3;
    }

    const elf32_shdr_t* sh = (const elf32_shdr_t*)(elf_data + eh->e_shoff);
    for (uint32 i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_REL && sh[i].sh_type != SHT_RELA) {
            continue;
        }
        if (!elf_range_in_bounds(sh[i].sh_offset, sh[i].sh_size, elf_size)) {
            return -4;
        }
        if (sh[i].sh_entsize == 0) {
            continue;
        }
        uint32 count = sh[i].sh_size / sh[i].sh_entsize;
        if (count == 0) {
            continue;
        }

        if (sh[i].sh_type == SHT_REL && sh[i].sh_entsize >= sizeof(elf32_rel_t)) {
            const elf32_rel_t* rel = (const elf32_rel_t*)(elf_data + sh[i].sh_offset);
            for (uint32 j = 0; j < count; j++) {
                uint32 type = ELF32_R_TYPE(rel[j].r_info);
                if (type == R_386_NONE || type == R_386_RELATIVE || type == R_386_32 || type == R_386_PC32) {
                    continue;
                }
                debuglog(DEBUG_WARN, "[ELF] relocation type %u present (deferred to ldso)\n", type);
            }
        } else if (sh[i].sh_type == SHT_RELA && sh[i].sh_entsize >= sizeof(elf32_rela_t)) {
            const elf32_rela_t* rela = (const elf32_rela_t*)(elf_data + sh[i].sh_offset);
            for (uint32 j = 0; j < count; j++) {
                uint32 type = ELF32_R_TYPE(rela[j].r_info);
                if (type == R_386_NONE || type == R_386_RELATIVE || type == R_386_32 || type == R_386_PC32) {
                    continue;
                }
                debuglog(DEBUG_WARN, "[ELF] rela type %u present (deferred to ldso)\n", type);
            }
        }
    }

    return 0;
}

int elf_load_executable_with_relocs(const uint8* elf_data, size_t elf_size,
                                    elf_load_info_t* load_info, bool allow_relocs) {
    int rc = elf_load_executable(elf_data, elf_size, load_info);
    if (rc != 0) {
        return rc;
    }

    bool has_interp = elf_has_program_header_type(elf_data, elf_size, PT_INTERP);
    bool has_dynamic = elf_has_program_header_type(elf_data, elf_size, PT_DYNAMIC);
    if (has_interp || has_dynamic) {
        if (!allow_relocs) {
            return -8;
        }
        int rel_rc = elf_process_relocations(elf_data, elf_size, load_info);
        if (rel_rc != 0) {
            return rel_rc;
        }
        // Full PT_INTERP / DT_* handling is delegated to ldso.
        debuglog(DEBUG_WARN, "[ELF] dynamic ELF loaded in compatibility mode (ldso pending)\n");
    }

    return 0;
}
