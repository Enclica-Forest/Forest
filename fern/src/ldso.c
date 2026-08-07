#include "include/ldso.h"

#include "include/debuglog.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/util.h"
#include "include/vfs.h"

extern void* kmalloc(size_t size);
extern void kfree(void* ptr);

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static ldso_object_t* g_ldso_objects;
static ldso_object_t g_ldso_object_pool[64];
static bool g_ldso_object_pool_used[64];
static uint32 g_ldso_next_handle = 1;

static const char* g_default_lib_paths[] = {
    "/lib",
    "/usr/lib",
    "/usr/local/lib"
};

typedef struct {
    uint16 vd_version;
    uint16 vd_flags;
    uint16 vd_ndx;
    uint16 vd_cnt;
    uint32 vd_hash;
    uint32 vd_aux;
    uint32 vd_next;
} elf32_verdef_t;

typedef struct {
    uint32 vda_name;
    uint32 vda_next;
} elf32_verdaux_t;

typedef struct {
    uint16 vn_version;
    uint16 vn_cnt;
    uint32 vn_file;
    uint32 vn_aux;
    uint32 vn_next;
} elf32_verneed_t;

typedef struct {
    uint32 vna_hash;
    uint16 vna_flags;
    uint16 vna_other;
    uint32 vna_name;
    uint32 vna_next;
} elf32_vernaux_t;

static bool ldso_range_in_bounds(size_t off, size_t len, size_t total) {
    if (off > total) {
        return false;
    }
    return len <= (total - off);
}

static void ldso_copy_string(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static const elf32_phdr_t* ldso_find_phdr(const ldso_object_t* obj, uint32 type) {
    uint32 i;
    if (!obj || !obj->phdrs) {
        return 0;
    }
    for (i = 0; i < obj->phnum; i++) {
        if (obj->phdrs[i].p_type == type) {
            return &obj->phdrs[i];
        }
    }
    return 0;
}

static bool ldso_vaddr_to_file_offset(const ldso_object_t* obj,
                                      uint32 vaddr,
                                      uint32 access_size,
                                      uint32* out_off) {
    uint32 i;

    if (!obj || !out_off) {
        return false;
    }

    for (i = 0; i < obj->phnum; i++) {
        const elf32_phdr_t* ph = &obj->phdrs[i];
        uint32 seg_start;
        uint32 seg_end;
        uint32 rel;

        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) {
            continue;
        }

        seg_start = ph->p_vaddr;
        seg_end = ph->p_vaddr + ph->p_filesz;
        if (seg_end < seg_start) {
            continue;
        }

        if (vaddr < seg_start || vaddr > seg_end) {
            continue;
        }

        rel = vaddr - seg_start;
        if (access_size > ph->p_filesz || rel > (ph->p_filesz - access_size)) {
            continue;
        }

        *out_off = ph->p_offset + rel;
        return true;
    }

    return false;
}

static bool ldso_vaddr_is_in_mem(const ldso_object_t* obj,
                                 uint32 vaddr,
                                 uint32 access_size,
                                 const elf32_phdr_t** out_seg) {
    uint32 i;

    if (!obj) {
        return false;
    }

    for (i = 0; i < obj->phnum; i++) {
        const elf32_phdr_t* ph = &obj->phdrs[i];
        uint32 seg_start;
        uint32 seg_end;
        uint32 rel;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        seg_start = ph->p_vaddr + obj->load_bias;
        seg_end = seg_start + ph->p_memsz;
        if (seg_end < seg_start) {
            continue;
        }

        if (vaddr < seg_start || vaddr > seg_end) {
            continue;
        }

        rel = vaddr - seg_start;
        if (access_size > ph->p_memsz || rel > (ph->p_memsz - access_size)) {
            continue;
        }

        if (out_seg) {
            *out_seg = ph;
        }
        return true;
    }

    return false;
}

static int ldso_set_dyn_ptr(const ldso_object_t* obj,
                            ldso_ptr_info_t* out,
                            uint32 d_ptr,
                            uint32 min_size) {
    uint32 off;

    if (!obj || !out) {
        return LDSO_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->present = true;
    out->raw_ptr = d_ptr;
    out->runtime_addr = d_ptr + obj->load_bias;

    if (!ldso_vaddr_to_file_offset(obj, d_ptr, min_size, &off)) {
        return LDSO_ERR_BOUNDS;
    }
    if (!ldso_range_in_bounds(off, min_size, obj->elf_size)) {
        return LDSO_ERR_BOUNDS;
    }

    out->file_ptr = (const void*)(obj->elf_image + off);
    return LDSO_OK;
}

static const char* ldso_dyn_string(const ldso_dynamic_info_t* dyn, uint32 off) {
    if (!dyn || !dyn->strtab.present || !dyn->strtab.file_ptr) {
        return 0;
    }
    if (off >= dyn->strsz) {
        return 0;
    }
    return (const char*)dyn->strtab.file_ptr + off;
}

static uint32 ldso_sysv_hash(const char* name) {
    uint32 h = 0;
    const unsigned char* p = (const unsigned char*)name;
    while (*p) {
        uint32 g;
        h = (h << 4) + *p++;
        g = h & 0xF0000000;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

static ldso_object_t* ldso_alloc_object_slot(void) {
    for (uint32 i = 0; i < ARRAY_COUNT(g_ldso_object_pool); i++) {
        if (!g_ldso_object_pool_used[i]) {
            g_ldso_object_pool_used[i] = true;
            memset(&g_ldso_object_pool[i], 0, sizeof(g_ldso_object_pool[i]));
            return &g_ldso_object_pool[i];
        }
    }
    return 0;
}

static ldso_object_t* ldso_find_object_by_handle(uint32 handle) {
    ldso_object_t* it;
    if (handle == 0) {
        return 0;
    }
    for (it = g_ldso_objects; it; it = it->next) {
        if (it->handle == handle) {
            return it;
        }
    }
    return 0;
}

static int ldso_map_object_segments(ldso_object_t* obj) {
    uint32 i;
    uint32 span;
    uint8* mapped;

    if (!obj) {
        return LDSO_ERR_INVALID_ARG;
    }
    if (obj->mapped_image) {
        return LDSO_OK;
    }
    if (obj->max_vaddr <= obj->min_vaddr) {
        return LDSO_ERR_INVALID_ELF;
    }

    span = obj->max_vaddr - obj->min_vaddr;
    span = (span + 0x0FFFu) & ~0x0FFFu;
    mapped = (uint8*)kmalloc(span);
    if (!mapped) {
        return LDSO_ERR_NOMEM;
    }
    memset(mapped, 0, span);

    for (i = 0; i < obj->phnum; i++) {
        const elf32_phdr_t* ph = &obj->phdrs[i];
        uint32 dst_off;
        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) {
            continue;
        }
        if (!ldso_range_in_bounds(ph->p_offset, ph->p_filesz, obj->elf_size)) {
            return LDSO_ERR_BOUNDS;
        }
        if (ph->p_vaddr < obj->min_vaddr) {
            return LDSO_ERR_BOUNDS;
        }

        dst_off = ph->p_vaddr - obj->min_vaddr;
        if (!ldso_range_in_bounds(dst_off, ph->p_filesz, span)) {
            return LDSO_ERR_BOUNDS;
        }

        memcpy(mapped + dst_off, obj->elf_image + ph->p_offset, ph->p_filesz);
    }

    obj->mapped_image = mapped;
    obj->mapped_size = span;
    obj->load_bias = (uint32)(uintptr_t)mapped - obj->min_vaddr;
    return LDSO_OK;
}

static const char* ldso_verdef_name_for_index(const ldso_object_t* obj, uint16 ver_index) {
    const uint8* base;
    const elf32_verdef_t* def;
    uint32 i;

    if (!obj || !obj->dyn.verdef.present || !obj->dyn.verdef.file_ptr || ver_index <= 1) {
        return 0;
    }

    base = (const uint8*)obj->dyn.verdef.file_ptr;
    def = (const elf32_verdef_t*)base;
    for (i = 0; i < obj->dyn.verdefnum; i++) {
        if ((const uint8*)def < base || (uintptr_t)((const uint8*)def - base) >= obj->elf_size) {
            return 0;
        }
        if (def->vd_ndx == ver_index) {
            const elf32_verdaux_t* aux = (const elf32_verdaux_t*)((const uint8*)def + def->vd_aux);
            if (aux && aux->vda_name < obj->dyn.strsz) {
                return ((const char*)obj->dyn.strtab.file_ptr) + aux->vda_name;
            }
            return 0;
        }
        if (def->vd_next == 0) {
            break;
        }
        def = (const elf32_verdef_t*)((const uint8*)def + def->vd_next);
    }
    return 0;
}

static const char* ldso_verneed_name_for_index(const ldso_object_t* obj, uint16 ver_index) {
    const uint8* base;
    const elf32_verneed_t* need;
    uint32 i;

    if (!obj || !obj->dyn.verneed.present || !obj->dyn.verneed.file_ptr || ver_index <= 1) {
        return 0;
    }

    base = (const uint8*)obj->dyn.verneed.file_ptr;
    need = (const elf32_verneed_t*)base;
    for (i = 0; i < obj->dyn.verneednum; i++) {
        uint16 k;
        const elf32_vernaux_t* aux = (const elf32_vernaux_t*)((const uint8*)need + need->vn_aux);
        for (k = 0; k < need->vn_cnt; k++) {
            if (aux->vna_other == ver_index && aux->vna_name < obj->dyn.strsz) {
                return ((const char*)obj->dyn.strtab.file_ptr) + aux->vna_name;
            }
            if (aux->vna_next == 0) {
                break;
            }
            aux = (const elf32_vernaux_t*)((const uint8*)aux + aux->vna_next);
        }
        if (need->vn_next == 0) {
            break;
        }
        need = (const elf32_verneed_t*)((const uint8*)need + need->vn_next);
    }
    return 0;
}

static bool ldso_symbol_version_match(const ldso_object_t* requester,
                                      uint32 requester_sym_index,
                                      const ldso_object_t* owner,
                                      uint32 owner_sym_index) {
    const uint16* req_vs;
    const uint16* own_vs;
    uint16 req_idx;
    uint16 own_idx;
    const char* req_name;
    const char* own_name;

    if (!requester || !owner || requester_sym_index == 0 || owner_sym_index == 0) {
        return true;
    }
    if (!requester->dyn.versym.present || !requester->dyn.versym.file_ptr) {
        return true;
    }
    if (!owner->dyn.versym.present || !owner->dyn.versym.file_ptr) {
        return true;
    }

    req_vs = (const uint16*)requester->dyn.versym.file_ptr;
    own_vs = (const uint16*)owner->dyn.versym.file_ptr;
    req_idx = (uint16)(req_vs[requester_sym_index] & 0x7FFFu);
    own_idx = (uint16)(own_vs[owner_sym_index] & 0x7FFFu);

    if (req_idx <= 1) {
        return true;
    }
    req_name = ldso_verneed_name_for_index(requester, req_idx);
    if (!req_name || !req_name[0]) {
        return true;
    }
    own_name = ldso_verdef_name_for_index(owner, own_idx);
    if (!own_name || !own_name[0]) {
        return false;
    }
    return strcmp(req_name, own_name) == 0;
}

static int ldso_register_needed_objects_recursive(const ldso_object_t* root, uint32 depth) {
    if (!root) {
        return LDSO_ERR_INVALID_ARG;
    }
    if (depth > 8) {
        return LDSO_ERR_UNSUPPORTED;
    }
    if (!root->dyn.dynamic || root->dyn.needed_count == 0) {
        return LDSO_OK;
    }

    for (uint32 i = 0; i < root->dyn.needed_count; i++) {
        const char* soname = root->dyn.needed[i];
        char resolved_path[LDSO_MAX_OBJECT_PATH];
        const uint8* dep_data = 0;
        uint32 dep_size = 0;
        ldso_object_t* dep_obj;
        int rc;

        if (!soname || !soname[0]) {
            continue;
        }
        if (ldso_find_object_by_name(soname)) {
            continue;
        }

        rc = ldso_resolve_library_path(soname, root->dyn.runpath, resolved_path, sizeof(resolved_path));
        if (rc != LDSO_OK && root->dyn.runpath != root->dyn.rpath) {
            rc = ldso_resolve_library_path(soname, root->dyn.rpath, resolved_path, sizeof(resolved_path));
        }
        if (rc != LDSO_OK) {
            rc = ldso_resolve_library_path(soname, 0, resolved_path, sizeof(resolved_path));
        }
        if (rc != LDSO_OK) {
            debuglog(DEBUG_WARN, "[LDSO] missing dependency: %s\n", soname);
            return rc;
        }

        if (!vfs_read_file(resolved_path, &dep_data, &dep_size) ||
            !dep_data || dep_size < sizeof(elf32_ehdr_t)) {
            debuglog(DEBUG_WARN, "[LDSO] failed to load dependency image: %s\n", resolved_path);
            return LDSO_ERR_NOT_FOUND;
        }

        dep_obj = ldso_alloc_object_slot();
        if (!dep_obj) {
            return LDSO_ERR_NOMEM;
        }

        rc = ldso_object_init_from_elf_image(dep_obj,
                                             soname,
                                             resolved_path,
                                             dep_data,
                                             (size_t)dep_size,
                                             root->load_bias);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_parse_dynamic(dep_obj);
        if (rc != LDSO_OK && rc != LDSO_ERR_NO_DYNAMIC) {
            return rc;
        }

        rc = ldso_map_object_segments(dep_obj);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_register_object(dep_obj);
        if (rc != LDSO_OK && rc != LDSO_ERR_DUPLICATE) {
            return rc;
        }

        rc = ldso_register_needed_objects_recursive(dep_obj, depth + 1);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_apply_relocations_i386(dep_obj, 0, 0);
        if (rc != LDSO_OK && rc != LDSO_ERR_UNSUPPORTED) {
            return rc;
        }
    }

    return LDSO_OK;
}

void ldso_init(void) {
    g_ldso_objects = 0;
    memset(g_ldso_object_pool_used, 0, sizeof(g_ldso_object_pool_used));
}

int ldso_object_init_from_elf_image(ldso_object_t* obj,
                                    const char* name,
                                    const char* path,
                                    const uint8* elf_image,
                                    size_t elf_size,
                                    uint32 runtime_base) {
    const elf32_ehdr_t* eh;
    uint32 i;
    uint32 min_vaddr = 0xFFFFFFFFu;
    uint32 max_vaddr = 0;

    if (!obj || !elf_image || elf_size < sizeof(elf32_ehdr_t)) {
        return LDSO_ERR_INVALID_ARG;
    }

    memset(obj, 0, sizeof(*obj));

    eh = (const elf32_ehdr_t*)elf_image;
    if (eh->e_ident[EI_MAG0] != ELF_MAGIC_0 ||
        eh->e_ident[EI_MAG1] != ELF_MAGIC_1 ||
        eh->e_ident[EI_MAG2] != ELF_MAGIC_2 ||
        eh->e_ident[EI_MAG3] != ELF_MAGIC_3) {
        return LDSO_ERR_INVALID_ELF;
    }
    if (eh->e_ident[EI_CLASS] != ELF_CLASS_32 ||
        eh->e_ident[EI_DATA] != ELF_DATA_2LSB ||
        eh->e_machine != ELF_MACHINE_386 ||
        eh->e_phentsize != sizeof(elf32_phdr_t)) {
        return LDSO_ERR_INVALID_ELF;
    }
    if (eh->e_phnum == 0) {
        return LDSO_ERR_INVALID_ELF;
    }

    if (!ldso_range_in_bounds(eh->e_phoff,
                              (size_t)eh->e_phnum * sizeof(elf32_phdr_t),
                              elf_size)) {
        return LDSO_ERR_BOUNDS;
    }

    obj->elf_image = elf_image;
    obj->elf_size = elf_size;
    obj->ehdr = eh;
    obj->phdrs = (const elf32_phdr_t*)(elf_image + eh->e_phoff);
    obj->phnum = eh->e_phnum;

    ldso_copy_string(obj->name, sizeof(obj->name), name ? name : "<unnamed>");
    ldso_copy_string(obj->path, sizeof(obj->path), path ? path : "");

    for (i = 0; i < obj->phnum; i++) {
        const elf32_phdr_t* ph = &obj->phdrs[i];
        uint32 end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        end = ph->p_vaddr + ph->p_memsz;
        if (end < ph->p_vaddr) {
            return LDSO_ERR_INVALID_ELF;
        }

        if (ph->p_vaddr < min_vaddr) {
            min_vaddr = ph->p_vaddr;
        }
        if (end > max_vaddr) {
            max_vaddr = end;
        }

        if (ph->p_filesz > 0 &&
            !ldso_range_in_bounds(ph->p_offset, ph->p_filesz, elf_size)) {
            return LDSO_ERR_BOUNDS;
        }
    }

    if (min_vaddr == 0xFFFFFFFFu) {
        return LDSO_ERR_INVALID_ELF;
    }

    obj->min_vaddr = min_vaddr;
    obj->max_vaddr = max_vaddr;

    if (eh->e_type == ELF_TYPE_DYN) {
        obj->load_bias = runtime_base - min_vaddr;
    } else {
        obj->load_bias = 0;
    }

    return LDSO_OK;
}

int ldso_parse_dynamic(ldso_object_t* obj) {
    const elf32_phdr_t* dyn_ph;
    const elf32_dyn_t* dyn;
    uint32 dyn_count;
    uint32 i;

    if (!obj) {
        return LDSO_ERR_INVALID_ARG;
    }

    memset(&obj->dyn, 0, sizeof(obj->dyn));
    obj->dyn.relent = sizeof(elf32_rel_t);
    obj->dyn.relaent = sizeof(elf32_rela_t);
    obj->dyn.syment = sizeof(elf32_sym_t);

    dyn_ph = ldso_find_phdr(obj, PT_DYNAMIC);
    if (!dyn_ph) {
        return LDSO_ERR_NO_DYNAMIC;
    }

    if (dyn_ph->p_filesz < sizeof(elf32_dyn_t) ||
        !ldso_range_in_bounds(dyn_ph->p_offset, dyn_ph->p_filesz, obj->elf_size)) {
        return LDSO_ERR_BOUNDS;
    }

    dyn = (const elf32_dyn_t*)(obj->elf_image + dyn_ph->p_offset);
    dyn_count = dyn_ph->p_filesz / sizeof(elf32_dyn_t);

    obj->dyn.dynamic = dyn;
    obj->dyn.dynamic_count = dyn_count;

    for (i = 0; i < dyn_count; i++) {
        uint32 tag = (uint32)dyn[i].d_tag;
        uint32 val = dyn[i].d_val;

        if (tag == DT_NULL) {
            break;
        }

        switch (tag) {
            case DT_STRTAB:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.strtab, val, 1) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_STRSZ:
                obj->dyn.strsz = val;
                break;
            case DT_SYMTAB:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.symtab, val, sizeof(elf32_sym_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_SYMENT:
                obj->dyn.syment = val;
                break;
            case DT_HASH:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.hash, val, sizeof(uint32) * 2) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_REL:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.rel, val, sizeof(elf32_rel_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_RELSZ:
                obj->dyn.relsz = val;
                break;
            case DT_RELENT:
                obj->dyn.relent = val;
                break;
            case DT_RELA:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.rela, val, sizeof(elf32_rela_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_RELASZ:
                obj->dyn.relasz = val;
                break;
            case DT_RELAENT:
                obj->dyn.relaent = val;
                break;
            case DT_JMPREL:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.jmprel, val, sizeof(elf32_rel_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_VERSYM:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.versym, val, sizeof(uint16)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_VERDEF:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.verdef, val, sizeof(elf32_verdef_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_VERDEFNUM:
                obj->dyn.verdefnum = val;
                break;
            case DT_VERNEED:
                if (ldso_set_dyn_ptr(obj, &obj->dyn.verneed, val, sizeof(elf32_verneed_t)) != LDSO_OK) {
                    return LDSO_ERR_BOUNDS;
                }
                break;
            case DT_VERNEEDNUM:
                obj->dyn.verneednum = val;
                break;
            case DT_PLTRELSZ:
                obj->dyn.pltrelsz = val;
                break;
            case DT_PLTREL:
                obj->dyn.pltrel_type = val;
                break;
            case DT_SONAME:
                obj->dyn.needed_name_offsets[obj->dyn.needed_count] = val;
                break;
            case DT_RPATH:
                obj->dyn.rpath = (const char*)(uintptr_t)val;
                break;
            case DT_RUNPATH:
                obj->dyn.runpath = (const char*)(uintptr_t)val;
                break;
            case DT_NEEDED:
                if (obj->dyn.needed_count >= LDSO_MAX_NEEDED_LIBS) {
                    return LDSO_ERR_UNSUPPORTED;
                }
                obj->dyn.needed_name_offsets[obj->dyn.needed_count++] = val;
                break;
            case DT_FLAGS:
                obj->dyn.flags = val;
                break;
            case DT_TEXTREL:
                obj->dyn.has_textrel = true;
                break;
            default:
                break;
        }
    }

    if (!obj->dyn.strtab.present || !obj->dyn.symtab.present ||
        obj->dyn.strsz == 0 || obj->dyn.syment < sizeof(elf32_sym_t)) {
        return LDSO_ERR_BAD_STATE;
    }

    if (obj->dyn.hash.present) {
        const uint32* hash_words = (const uint32*)obj->dyn.hash.file_ptr;
        obj->dyn.nbucket = hash_words[0];
        obj->dyn.nchain = hash_words[1];

        if (obj->dyn.nbucket == 0 || obj->dyn.nchain == 0) {
            return LDSO_ERR_BAD_STATE;
        }
    }

    if (obj->dyn.versym.present && obj->dyn.nchain > 0) {
        uint32 versym_bytes = obj->dyn.nchain * sizeof(uint16);
        uint32 versym_off;
        if (!ldso_vaddr_to_file_offset(obj, obj->dyn.versym.raw_ptr, versym_bytes, &versym_off) ||
            !ldso_range_in_bounds(versym_off, versym_bytes, obj->elf_size)) {
            return LDSO_ERR_BOUNDS;
        }
        obj->dyn.versym.file_ptr = obj->elf_image + versym_off;
    }

    if (obj->dyn.rel.present) {
        if (obj->dyn.relent != sizeof(elf32_rel_t) ||
            (obj->dyn.relsz % obj->dyn.relent) != 0) {
            return LDSO_ERR_UNSUPPORTED;
        }

        obj->dyn.rel_count = obj->dyn.relsz / obj->dyn.relent;
        if (obj->dyn.rel_count > 0) {
            uint32 first_rel = obj->dyn.rel.raw_ptr;
            uint32 total_rel = obj->dyn.rel_count * sizeof(elf32_rel_t);
            uint32 rel_off;
            if (!ldso_vaddr_to_file_offset(obj, first_rel, total_rel, &rel_off) ||
                !ldso_range_in_bounds(rel_off, total_rel, obj->elf_size)) {
                return LDSO_ERR_BOUNDS;
            }
            obj->dyn.rel.file_ptr = obj->elf_image + rel_off;
        }
    }

    if (obj->dyn.rela.present) {
        if (obj->dyn.relaent != sizeof(elf32_rela_t) ||
            (obj->dyn.relasz % obj->dyn.relaent) != 0) {
            return LDSO_ERR_UNSUPPORTED;
        }

        obj->dyn.rela_count = obj->dyn.relasz / obj->dyn.relaent;
        if (obj->dyn.rela_count > 0) {
            uint32 first_rela = obj->dyn.rela.raw_ptr;
            uint32 total_rela = obj->dyn.rela_count * sizeof(elf32_rela_t);
            uint32 rela_off;
            if (!ldso_vaddr_to_file_offset(obj, first_rela, total_rela, &rela_off) ||
                !ldso_range_in_bounds(rela_off, total_rela, obj->elf_size)) {
                return LDSO_ERR_BOUNDS;
            }
            obj->dyn.rela.file_ptr = obj->elf_image + rela_off;
        }
    }

    if (obj->dyn.jmprel.present && obj->dyn.pltrelsz > 0) {
        uint32 jmprel_off;
        if (obj->dyn.pltrel_type == DT_REL) {
            if ((obj->dyn.pltrelsz % sizeof(elf32_rel_t)) != 0) {
                return LDSO_ERR_UNSUPPORTED;
            }
            if (!ldso_vaddr_to_file_offset(obj, obj->dyn.jmprel.raw_ptr,
                                           obj->dyn.pltrelsz, &jmprel_off) ||
                !ldso_range_in_bounds(jmprel_off, obj->dyn.pltrelsz, obj->elf_size)) {
                return LDSO_ERR_BOUNDS;
            }
            obj->dyn.jmprel.file_ptr = obj->elf_image + jmprel_off;
        } else if (obj->dyn.pltrel_type == DT_RELA) {
            if ((obj->dyn.pltrelsz % sizeof(elf32_rela_t)) != 0) {
                return LDSO_ERR_UNSUPPORTED;
            }
            if (!ldso_vaddr_to_file_offset(obj, obj->dyn.jmprel.raw_ptr,
                                           obj->dyn.pltrelsz, &jmprel_off) ||
                !ldso_range_in_bounds(jmprel_off, obj->dyn.pltrelsz, obj->elf_size)) {
                return LDSO_ERR_BOUNDS;
            }
            obj->dyn.jmprel.file_ptr = obj->elf_image + jmprel_off;
        } else {
            return LDSO_ERR_UNSUPPORTED;
        }
    }

    if (obj->dyn.flags & DF_TEXTREL) {
        obj->dyn.has_textrel = true;
    }

    for (i = 0; i < obj->dyn.needed_count; i++) {
        obj->dyn.needed[i] = ldso_dyn_string(&obj->dyn, obj->dyn.needed_name_offsets[i]);
        if (!obj->dyn.needed[i]) {
            return LDSO_ERR_BOUNDS;
        }
    }

    if ((uintptr_t)obj->dyn.rpath <= 0xFFFFFFFFu) {
        obj->dyn.rpath = ldso_dyn_string(&obj->dyn, (uint32)(uintptr_t)obj->dyn.rpath);
    }
    if ((uintptr_t)obj->dyn.runpath <= 0xFFFFFFFFu) {
        obj->dyn.runpath = ldso_dyn_string(&obj->dyn, (uint32)(uintptr_t)obj->dyn.runpath);
    }

    if (obj->dyn.soname == 0 && obj->path[0] != '\0') {
        obj->dyn.soname = obj->path;
    }

    return LDSO_OK;
}

int ldso_register_object(ldso_object_t* obj) {
    ldso_object_t* it;

    if (!obj) {
        return LDSO_ERR_INVALID_ARG;
    }

    for (it = g_ldso_objects; it; it = it->next) {
        if (it == obj || (obj->name[0] && strcmp(it->name, obj->name) == 0)) {
            return LDSO_ERR_DUPLICATE;
        }
    }

    obj->next = g_ldso_objects;
    g_ldso_objects = obj;
    return LDSO_OK;
}

void ldso_unregister_object(ldso_object_t* obj) {
    ldso_object_t* it;
    ldso_object_t* prev = 0;

    for (it = g_ldso_objects; it; it = it->next) {
        if (it == obj) {
            if (prev) {
                prev->next = it->next;
            } else {
                g_ldso_objects = it->next;
            }
            it->next = 0;
            return;
        }
        prev = it;
    }
}

const ldso_object_t* ldso_find_object_by_name(const char* name) {
    const ldso_object_t* it;

    if (!name || !name[0]) {
        return 0;
    }

    for (it = g_ldso_objects; it; it = it->next) {
        if ((it->name[0] && strcmp(it->name, name) == 0) ||
            (it->path[0] && strcmp(it->path, name) == 0) ||
            (it->dyn.soname && strcmp(it->dyn.soname, name) == 0)) {
            return it;
        }
    }
    return 0;
}

const ldso_object_t* ldso_get_object_list(void) {
    return g_ldso_objects;
}

static int ldso_lookup_symbol_in_object_ex(const ldso_object_t* obj,
                                           const char* symbol_name,
                                           uint32* out_addr,
                                           const elf32_sym_t** out_sym,
                                           uint32* out_sym_index) {
    const elf32_sym_t* symtab;
    uint32 i;

    if (!obj || !symbol_name || !symbol_name[0]) {
        return LDSO_ERR_INVALID_ARG;
    }
    if (!obj->dyn.symtab.present || !obj->dyn.strtab.present) {
        return LDSO_ERR_BAD_STATE;
    }

    symtab = (const elf32_sym_t*)obj->dyn.symtab.file_ptr;

    if (obj->dyn.hash.present && obj->dyn.nbucket > 0 && obj->dyn.nchain > 0) {
        const uint32* hash = (const uint32*)obj->dyn.hash.file_ptr;
        const uint32* buckets = hash + 2;
        const uint32* chains = buckets + obj->dyn.nbucket;
        uint32 h = ldso_sysv_hash(symbol_name);

        for (i = buckets[h % obj->dyn.nbucket]; i != SHN_UNDEF; i = chains[i]) {
            const char* name;
            const elf32_sym_t* sym;
            if (i >= obj->dyn.nchain) {
                return LDSO_ERR_BOUNDS;
            }
            sym = &symtab[i];
            if (sym->st_name >= obj->dyn.strsz) {
                continue;
            }
            name = ((const char*)obj->dyn.strtab.file_ptr) + sym->st_name;
            if (strcmp(name, symbol_name) == 0) {
                if (sym->st_shndx == SHN_UNDEF) {
                    return LDSO_ERR_NOT_FOUND;
                }
                if (out_addr) {
                    *out_addr = obj->load_bias + sym->st_value;
                }
                if (out_sym) {
                    *out_sym = sym;
                }
                if (out_sym_index) {
                    *out_sym_index = i;
                }
                return LDSO_OK;
            }
        }

        return LDSO_ERR_NOT_FOUND;
    }

    if (obj->dyn.nchain == 0) {
        return LDSO_ERR_UNSUPPORTED;
    }

    for (i = 0; i < obj->dyn.nchain; i++) {
        const elf32_sym_t* sym = &symtab[i];
        const char* name;

        if (sym->st_name >= obj->dyn.strsz) {
            continue;
        }

        name = ((const char*)obj->dyn.strtab.file_ptr) + sym->st_name;
        if (strcmp(name, symbol_name) == 0) {
            if (sym->st_shndx == SHN_UNDEF) {
                return LDSO_ERR_NOT_FOUND;
            }
            if (out_addr) {
                *out_addr = obj->load_bias + sym->st_value;
            }
            if (out_sym) {
                *out_sym = sym;
            }
            if (out_sym_index) {
                *out_sym_index = i;
            }
            return LDSO_OK;
        }
    }

    return LDSO_ERR_NOT_FOUND;
}

int ldso_lookup_symbol_in_object(const ldso_object_t* obj,
                                 const char* symbol_name,
                                 uint32* out_addr,
                                 const elf32_sym_t** out_sym) {
    return ldso_lookup_symbol_in_object_ex(obj, symbol_name, out_addr, out_sym, 0);
}

static int ldso_lookup_symbol_global_ex(const char* symbol_name,
                                        const ldso_object_t* requester,
                                        uint32 requester_sym_index,
                                        uint32* out_addr,
                                        const ldso_object_t** out_owner,
                                        const elf32_sym_t** out_sym) {
    const ldso_object_t* it;

    if (!symbol_name || !symbol_name[0]) {
        return LDSO_ERR_INVALID_ARG;
    }

    for (it = g_ldso_objects; it; it = it->next) {
        uint32 addr = 0;
        const elf32_sym_t* sym = 0;
        uint32 owner_sym_index = 0;
        int rc = ldso_lookup_symbol_in_object_ex(it, symbol_name, &addr, &sym, &owner_sym_index);
        if (rc == LDSO_OK) {
            if (!ldso_symbol_version_match(requester, requester_sym_index, it, owner_sym_index)) {
                continue;
            }
            if (out_addr) {
                *out_addr = addr;
            }
            if (out_owner) {
                *out_owner = it;
            }
            if (out_sym) {
                *out_sym = sym;
            }
            return LDSO_OK;
        }
    }

    return LDSO_ERR_NOT_FOUND;
}

int ldso_lookup_symbol_global(const char* symbol_name,
                              const ldso_object_t* requester,
                              uint32* out_addr,
                              const ldso_object_t** out_owner,
                              const elf32_sym_t** out_sym) {
    return ldso_lookup_symbol_global_ex(symbol_name, requester, 0, out_addr, out_owner, out_sym);
}

static int ldso_walk_rel_array(const ldso_object_t* obj,
                               const elf32_rel_t* rel,
                               uint32 count,
                               bool from_plt,
                               ldso_reloc_walk_cb cb,
                               void* ctx) {
    uint32 i;

    for (i = 0; i < count; i++) {
        ldso_reloc_entry_t entry;
        int rc;

        entry.r_offset = rel[i].r_offset;
        entry.r_info = rel[i].r_info;
        entry.type = ELF32_R_TYPE(entry.r_info);
        entry.sym_index = ELF32_R_SYM(entry.r_info);
        entry.addend = 0;
        entry.is_rela = false;
        entry.from_plt = from_plt;

        rc = cb(obj, &entry, ctx);
        if (rc != LDSO_OK) {
            return rc;
        }
    }

    return LDSO_OK;
}

static int ldso_walk_rela_array(const ldso_object_t* obj,
                                const elf32_rela_t* rela,
                                uint32 count,
                                bool from_plt,
                                ldso_reloc_walk_cb cb,
                                void* ctx) {
    uint32 i;

    for (i = 0; i < count; i++) {
        ldso_reloc_entry_t entry;
        int rc;

        entry.r_offset = rela[i].r_offset;
        entry.r_info = rela[i].r_info;
        entry.type = ELF32_R_TYPE(entry.r_info);
        entry.sym_index = ELF32_R_SYM(entry.r_info);
        entry.addend = rela[i].r_addend;
        entry.is_rela = true;
        entry.from_plt = from_plt;

        rc = cb(obj, &entry, ctx);
        if (rc != LDSO_OK) {
            return rc;
        }
    }

    return LDSO_OK;
}

int ldso_walk_relocations(const ldso_object_t* obj,
                          ldso_reloc_walk_cb cb,
                          void* ctx) {
    int rc;

    if (!obj || !cb) {
        return LDSO_ERR_INVALID_ARG;
    }

    if (obj->dyn.rela_count > 0 && obj->dyn.rela.file_ptr) {
        rc = ldso_walk_rela_array(obj,
                                  (const elf32_rela_t*)obj->dyn.rela.file_ptr,
                                  obj->dyn.rela_count,
                                  false,
                                  cb,
                                  ctx);
        if (rc != LDSO_OK) {
            return rc;
        }
    }

    if (obj->dyn.rel_count > 0 && obj->dyn.rel.file_ptr) {
        rc = ldso_walk_rel_array(obj,
                                 (const elf32_rel_t*)obj->dyn.rel.file_ptr,
                                 obj->dyn.rel_count,
                                 false,
                                 cb,
                                 ctx);
        if (rc != LDSO_OK) {
            return rc;
        }
    }

    if (obj->dyn.jmprel.present && obj->dyn.jmprel.file_ptr && obj->dyn.pltrelsz) {
        if (obj->dyn.pltrel_type == DT_REL) {
            uint32 count = obj->dyn.pltrelsz / sizeof(elf32_rel_t);
            rc = ldso_walk_rel_array(obj,
                                     (const elf32_rel_t*)obj->dyn.jmprel.file_ptr,
                                     count,
                                     true,
                                     cb,
                                     ctx);
        } else if (obj->dyn.pltrel_type == DT_RELA) {
            uint32 count = obj->dyn.pltrelsz / sizeof(elf32_rela_t);
            rc = ldso_walk_rela_array(obj,
                                      (const elf32_rela_t*)obj->dyn.jmprel.file_ptr,
                                      count,
                                      true,
                                      cb,
                                      ctx);
        } else {
            return LDSO_ERR_UNSUPPORTED;
        }
        if (rc != LDSO_OK) {
            return rc;
        }
    }

    return LDSO_OK;
}

typedef struct {
    ldso_symbol_resolver_t resolver;
    void* resolver_ctx;
} ldso_apply_ctx_t;

typedef enum {
    LDSO_RELOC_FILTER_ALL = 0,
    LDSO_RELOC_FILTER_REL = 1,
    LDSO_RELOC_FILTER_RELA = 2
} ldso_reloc_filter_t;

typedef struct {
    ldso_apply_ctx_t apply;
    ldso_reloc_filter_t filter;
} ldso_apply_filter_ctx_t;

static int ldso_apply_one_reloc_filtered(const ldso_object_t* obj,
                                         const ldso_reloc_entry_t* reloc,
                                         void* ctx_ptr);
static int ldso_lookup_symbol_global_ex(const char* symbol_name,
                                        const ldso_object_t* requester,
                                        uint32 requester_sym_index,
                                        uint32* out_addr,
                                        const ldso_object_t** out_owner,
                                        const elf32_sym_t** out_sym);

static int ldso_resolve_symbol_value(const ldso_object_t* obj,
                                     uint32 sym_index,
                                     ldso_symbol_resolver_t resolver,
                                     void* resolver_ctx,
                                     uint32* out_sym_addr) {
    const elf32_sym_t* symtab;
    const elf32_sym_t* sym;
    const char* name;

    if (!obj || !out_sym_addr) {
        return LDSO_ERR_INVALID_ARG;
    }

    if (sym_index == 0) {
        *out_sym_addr = 0;
        return LDSO_OK;
    }

    if (!obj->dyn.symtab.file_ptr || !obj->dyn.strtab.file_ptr || obj->dyn.nchain == 0) {
        return LDSO_ERR_BAD_STATE;
    }

    if (sym_index >= obj->dyn.nchain) {
        return LDSO_ERR_BOUNDS;
    }

    symtab = (const elf32_sym_t*)obj->dyn.symtab.file_ptr;
    sym = &symtab[sym_index];

    if (sym->st_shndx != SHN_UNDEF) {
        *out_sym_addr = obj->load_bias + sym->st_value;
        return LDSO_OK;
    }

    if (sym->st_name >= obj->dyn.strsz) {
        return LDSO_ERR_BOUNDS;
    }

    name = ((const char*)obj->dyn.strtab.file_ptr) + sym->st_name;
    if (!name[0]) {
        return LDSO_ERR_NOT_FOUND;
    }

    if (resolver) {
        return resolver(obj, name, out_sym_addr, resolver_ctx);
    }

    return ldso_lookup_symbol_global_ex(name, obj, sym_index, out_sym_addr, 0, 0);
}

static int ldso_apply_one_reloc(const ldso_object_t* obj,
                                const ldso_reloc_entry_t* reloc,
                                void* ctx_ptr) {
    ldso_apply_ctx_t* ctx = (ldso_apply_ctx_t*)ctx_ptr;
    uint32 place_addr;
    const elf32_phdr_t* seg = 0;
    volatile uint32* place;
    uint32 addend;
    uint32 sym_addr = 0;
    int rc;

    if (!obj || !reloc || !ctx) {
        return LDSO_ERR_INVALID_ARG;
    }

    place_addr = obj->load_bias + reloc->r_offset;
    if (!ldso_vaddr_is_in_mem(obj, place_addr, sizeof(uint32), &seg)) {
        return LDSO_ERR_BOUNDS;
    }

    if (!(seg->p_flags & PF_W) && !obj->dyn.has_textrel) {
        debuglog(DEBUG_WARN,
                 "[LDSO] refusing relocation into non-writable segment without TEXTREL: %s offset=0x%x type=%u\n",
                 obj->name,
                 reloc->r_offset,
                 reloc->type);
        return LDSO_ERR_UNSUPPORTED;
    }

    place = (volatile uint32*)(uintptr_t)place_addr;
    addend = reloc->is_rela ? (uint32)reloc->addend : *place;

    switch (reloc->type) {
        case R_386_NONE:
            return LDSO_OK;

        case R_386_RELATIVE:
            *place = obj->load_bias + addend;
            return LDSO_OK;

        case R_386_32:
            rc = ldso_resolve_symbol_value(obj,
                                           reloc->sym_index,
                                           ctx->resolver,
                                           ctx->resolver_ctx,
                                           &sym_addr);
            if (rc != LDSO_OK) {
                return rc;
            }
            *place = sym_addr + addend;
            return LDSO_OK;

        case R_386_PC32:
            rc = ldso_resolve_symbol_value(obj,
                                           reloc->sym_index,
                                           ctx->resolver,
                                           ctx->resolver_ctx,
                                           &sym_addr);
            if (rc != LDSO_OK) {
                return rc;
            }
            *place = sym_addr + addend - place_addr;
            return LDSO_OK;

        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            rc = ldso_resolve_symbol_value(obj,
                                           reloc->sym_index,
                                           ctx->resolver,
                                           ctx->resolver_ctx,
                                           &sym_addr);
            if (rc != LDSO_OK) {
                return rc;
            }
            *place = sym_addr;
            return LDSO_OK;

        case R_386_COPY:
            return LDSO_ERR_UNSUPPORTED;

        default:
            debuglog(DEBUG_WARN,
                     "[LDSO] unsupported i386 relocation: obj=%s type=%u offset=0x%x\n",
                     obj->name,
                     reloc->type,
                     reloc->r_offset);
            return LDSO_ERR_UNSUPPORTED;
    }
}

int ldso_apply_relocations_i386(ldso_object_t* obj,
                                ldso_symbol_resolver_t resolver,
                                void* resolver_ctx) {
    ldso_apply_filter_ctx_t ctx;

    if (!obj) {
        return LDSO_ERR_INVALID_ARG;
    }

    ctx.apply.resolver = resolver;
    ctx.apply.resolver_ctx = resolver_ctx;
    ctx.filter = LDSO_RELOC_FILTER_ALL;

    return ldso_walk_relocations(obj, ldso_apply_one_reloc_filtered, &ctx);
}

static int ldso_apply_one_reloc_filtered(const ldso_object_t* obj,
                                         const ldso_reloc_entry_t* reloc,
                                         void* ctx_ptr) {
    ldso_apply_filter_ctx_t* ctx = (ldso_apply_filter_ctx_t*)ctx_ptr;

    if (!ctx) {
        return LDSO_ERR_INVALID_ARG;
    }

    if (ctx->filter == LDSO_RELOC_FILTER_REL && reloc->is_rela) {
        return LDSO_OK;
    }
    if (ctx->filter == LDSO_RELOC_FILTER_RELA && !reloc->is_rela) {
        return LDSO_OK;
    }

    return ldso_apply_one_reloc(obj, reloc, &ctx->apply);
}

static int ldso_apply_relocations_i386_filtered(ldso_object_t* obj,
                                                ldso_symbol_resolver_t resolver,
                                                void* resolver_ctx,
                                                ldso_reloc_filter_t filter) {
    ldso_apply_filter_ctx_t ctx;

    if (!obj) {
        return LDSO_ERR_INVALID_ARG;
    }

    ctx.apply.resolver = resolver;
    ctx.apply.resolver_ctx = resolver_ctx;
    ctx.filter = filter;

    return ldso_walk_relocations(obj, ldso_apply_one_reloc_filtered, &ctx);
}

static int ldso_prepare_loaded_exec_object(const uint8* elf_data,
                                           uint32 elf_size,
                                           const elf_load_info_t* load_info,
                                           ldso_object_t* out_obj) {
    int rc;

    if (!elf_data || !load_info || !out_obj || elf_size < sizeof(elf32_ehdr_t)) {
        return LDSO_ERR_INVALID_ARG;
    }
    if (!load_info->valid) {
        return LDSO_ERR_BAD_STATE;
    }

    rc = ldso_object_init_from_elf_image(out_obj,
                                         "<exec>",
                                         "",
                                         elf_data,
                                         (size_t)elf_size,
                                         load_info->base_address);
    if (rc != LDSO_OK) {
        return rc;
    }

    rc = ldso_parse_dynamic(out_obj);
    if (rc == LDSO_ERR_NO_DYNAMIC) {
        return LDSO_OK;
    }
    return rc;
}

static int ldso_apply_in_loaded_address_space(const uint8* elf_data,
                                              uint32 elf_size,
                                              const elf_load_info_t* load_info,
                                              page_directory_t* target_pd,
                                              ldso_reloc_filter_t filter) {
    ldso_object_t obj;
    page_directory_t* prev_pd = 0;
    int rc;

    if (!target_pd) {
        return LDSO_ERR_INVALID_ARG;
    }

    rc = ldso_prepare_loaded_exec_object(elf_data, elf_size, load_info, &obj);
    if (rc != LDSO_OK) {
        return rc;
    }
    if (!obj.dyn.dynamic) {
        return LDSO_OK;
    }

    prev_pd = vmm_get_current_page_directory();
    if (prev_pd != target_pd) {
        vmm_switch_page_directory(target_pd);
    }

    rc = ldso_apply_relocations_i386_filtered(&obj, 0, 0, filter);

    if (prev_pd != target_pd) {
        vmm_switch_page_directory(prev_pd);
    }

    return rc;
}

int ldso_apply_rel_in_loaded_address_space(const uint8* elf_data,
                                           uint32 elf_size,
                                           const elf_load_info_t* load_info,
                                           page_directory_t* target_pd) {
    return ldso_apply_in_loaded_address_space(elf_data,
                                              elf_size,
                                              load_info,
                                              target_pd,
                                              LDSO_RELOC_FILTER_REL);
}

int ldso_apply_rela_in_loaded_address_space(const uint8* elf_data,
                                            uint32 elf_size,
                                            const elf_load_info_t* load_info,
                                            page_directory_t* target_pd) {
    return ldso_apply_in_loaded_address_space(elf_data,
                                              elf_size,
                                              load_info,
                                              target_pd,
                                              LDSO_RELOC_FILTER_RELA);
}

int ldso_exec_handoff(const uint8* elf_data,
                      uint32 elf_size,
                      const char* interp_path,
                      const elf_load_info_t* load_info,
                      uint32* out_entry_point) {
    ldso_object_t main_obj;
    int rc;

    if (!out_entry_point) {
        return LDSO_ERR_INVALID_ARG;
    }
    if (!elf_data || !load_info) {
        return LDSO_ERR_INVALID_ARG;
    }

    *out_entry_point = load_info->entry_point;

    rc = ldso_prepare_loaded_exec_object(elf_data, elf_size, load_info, &main_obj);
    if (rc != LDSO_OK) {
        return rc;
    }

    rc = ldso_register_needed_objects_recursive(&main_obj, 0);
    if (rc != LDSO_OK) {
        return rc;
    }

    if (interp_path && interp_path[0]) {
        const uint8* interp_data = 0;
        uint32 interp_size = 0;
        ldso_object_t* interp_obj;

        if (!vfs_read_file(interp_path, &interp_data, &interp_size)) {
            return LDSO_ERR_NOT_FOUND;
        }
        if (!interp_data || interp_size < sizeof(elf32_ehdr_t)) {
            return LDSO_ERR_INVALID_ELF;
        }

        interp_obj = ldso_alloc_object_slot();
        if (!interp_obj) {
            return LDSO_ERR_NOMEM;
        }

        rc = ldso_object_init_from_elf_image(interp_obj,
                                             "ld.so",
                                             interp_path,
                                             interp_data,
                                             (size_t)interp_size,
                                             load_info->base_address);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_parse_dynamic(interp_obj);
        if (rc != LDSO_OK && rc != LDSO_ERR_NO_DYNAMIC) {
            return rc;
        }

        rc = ldso_map_object_segments(interp_obj);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_register_object(interp_obj);
        if (rc != LDSO_OK && rc != LDSO_ERR_DUPLICATE) {
            return rc;
        }

        rc = ldso_register_needed_objects_recursive(interp_obj, 0);
        if (rc != LDSO_OK) {
            return rc;
        }

        rc = ldso_apply_relocations_i386(interp_obj, 0, 0);
        if (rc != LDSO_OK && rc != LDSO_ERR_UNSUPPORTED) {
            return rc;
        }
    }

    return LDSO_OK;
}

static int ldso_validate_one_reloc(const ldso_object_t* obj,
                                   const ldso_reloc_entry_t* reloc,
                                   void* ctx) {
    uint32 place_addr;

    (void)ctx;

    if (!obj || !reloc) {
        return LDSO_ERR_INVALID_ARG;
    }

    place_addr = obj->load_bias + reloc->r_offset;
    if (!ldso_vaddr_is_in_mem(obj, place_addr, sizeof(uint32), 0)) {
        return LDSO_ERR_BOUNDS;
    }

    switch (reloc->type) {
        case R_386_NONE:
        case R_386_RELATIVE:
        case R_386_32:
        case R_386_PC32:
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT:
            return LDSO_OK;
        default:
            return LDSO_ERR_UNSUPPORTED;
    }
}

int ldso_validate_relocations_i386(const ldso_object_t* obj) {
    return ldso_walk_relocations(obj, ldso_validate_one_reloc, 0);
}

static bool ldso_path_exists(const char* path) {
    return vfs_read_file(path, 0, 0);
}

static bool ldso_append_search_path(ldso_search_path_list_t* out_paths,
                                    const char* path) {
    uint32 i;

    if (!out_paths || !path || !path[0]) {
        return false;
    }

    for (i = 0; i < out_paths->count; i++) {
        if (strcmp(out_paths->entries[i], path) == 0) {
            return true;
        }
    }

    if (out_paths->count >= LDSO_MAX_SEARCH_PATHS) {
        return false;
    }

    ldso_copy_string(out_paths->entries[out_paths->count],
                     sizeof(out_paths->entries[out_paths->count]),
                     path);
    out_paths->count++;
    return true;
}

int ldso_build_search_paths(const char* ld_library_path,
                            ldso_search_path_list_t* out_paths) {
    uint32 i;

    if (!out_paths) {
        return LDSO_ERR_INVALID_ARG;
    }

    memset(out_paths, 0, sizeof(*out_paths));

    if (ld_library_path && ld_library_path[0]) {
        const char* start = ld_library_path;
        const char* p = ld_library_path;

        while (1) {
            if (*p == ':' || *p == '\0') {
                char token[LDSO_MAX_SEARCH_PATH_LEN];
                size_t len = (size_t)(p - start);

                if (len > 0) {
                    if (len >= sizeof(token)) {
                        len = sizeof(token) - 1;
                    }
                    memcpy(token, start, len);
                    token[len] = '\0';
                    if (!ldso_append_search_path(out_paths, token)) {
                        return LDSO_ERR_UNSUPPORTED;
                    }
                }

                if (*p == '\0') {
                    break;
                }
                start = p + 1;
            }
            p++;
        }
    }

    for (i = 0; i < ARRAY_COUNT(g_default_lib_paths); i++) {
        if (!ldso_append_search_path(out_paths, g_default_lib_paths[i])) {
            return LDSO_ERR_UNSUPPORTED;
        }
    }

    return LDSO_OK;
}

int ldso_resolve_library_path(const char* soname,
                              const char* ld_library_path,
                              char* out_path,
                              size_t out_path_size) {
    ldso_search_path_list_t paths;
    uint32 i;

    if (!soname || !soname[0] || !out_path || out_path_size == 0) {
        return LDSO_ERR_INVALID_ARG;
    }

    out_path[0] = '\0';

    if (strchr(soname, '/')) {
        if (!ldso_path_exists(soname)) {
            return LDSO_ERR_NOT_FOUND;
        }
        ldso_copy_string(out_path, out_path_size, soname);
        return LDSO_OK;
    }

    if (ldso_build_search_paths(ld_library_path, &paths) != LDSO_OK) {
        return LDSO_ERR_BAD_STATE;
    }

    for (i = 0; i < paths.count; i++) {
        char candidate[LDSO_MAX_OBJECT_PATH];
        int n;

        n = string_format(candidate, sizeof(candidate), "%s/%s", paths.entries[i], soname);
        if (n <= 0 || (size_t)n >= sizeof(candidate)) {
            continue;
        }

        if (ldso_path_exists(candidate)) {
            ldso_copy_string(out_path, out_path_size, candidate);
            return LDSO_OK;
        }
    }

    return LDSO_ERR_NOT_FOUND;
}

int ldso_dlopen_path(const char* path, int flags, uint32* out_handle) {
    char resolved[LDSO_MAX_OBJECT_PATH];
    const uint8* elf_data = 0;
    uint32 elf_size = 0;
    ldso_object_t* obj;
    int rc;

    (void)flags;

    if (!path || !path[0] || !out_handle) {
        return LDSO_ERR_INVALID_ARG;
    }

    if (ldso_resolve_library_path(path, 0, resolved, sizeof(resolved)) != LDSO_OK) {
        return LDSO_ERR_NOT_FOUND;
    }

    obj = (ldso_object_t*)ldso_find_object_by_name(resolved);
    if (obj) {
        if (obj->handle == 0) {
            obj->handle = g_ldso_next_handle++;
        }
        *out_handle = obj->handle;
        return LDSO_OK;
    }

    if (!vfs_read_file(resolved, &elf_data, &elf_size) ||
        !elf_data || elf_size < sizeof(elf32_ehdr_t)) {
        return LDSO_ERR_NOT_FOUND;
    }

    obj = ldso_alloc_object_slot();
    if (!obj) {
        return LDSO_ERR_NOMEM;
    }

    rc = ldso_object_init_from_elf_image(obj, path, resolved, elf_data, elf_size, 0);
    if (rc != LDSO_OK) {
        return rc;
    }

    rc = ldso_parse_dynamic(obj);
    if (rc != LDSO_OK && rc != LDSO_ERR_NO_DYNAMIC) {
        return rc;
    }

    rc = ldso_map_object_segments(obj);
    if (rc != LDSO_OK) {
        return rc;
    }

    rc = ldso_register_object(obj);
    if (rc != LDSO_OK && rc != LDSO_ERR_DUPLICATE) {
        return rc;
    }

    rc = ldso_register_needed_objects_recursive(obj, 0);
    if (rc != LDSO_OK) {
        return rc;
    }

    rc = ldso_apply_relocations_i386(obj, 0, 0);
    if (rc != LDSO_OK && rc != LDSO_ERR_UNSUPPORTED) {
        return rc;
    }

    obj->handle = g_ldso_next_handle++;
    *out_handle = obj->handle;
    return LDSO_OK;
}

int ldso_dlsym_by_handle(uint32 handle, const char* symbol, uint32* out_addr) {
    ldso_object_t* obj;
    uint32 addr = 0;
    int rc;

    if (!symbol || !symbol[0] || !out_addr) {
        return LDSO_ERR_INVALID_ARG;
    }

    if (handle == 0) {
        return ldso_lookup_symbol_global(symbol, 0, out_addr, 0, 0);
    }

    obj = ldso_find_object_by_handle(handle);
    if (!obj) {
        return LDSO_ERR_NOT_FOUND;
    }

    rc = ldso_lookup_symbol_in_object(obj, symbol, &addr, 0);
    if (rc != LDSO_OK) {
        return rc;
    }
    *out_addr = addr;
    return LDSO_OK;
}

int ldso_dlclose_handle(uint32 handle) {
    ldso_object_t* obj = ldso_find_object_by_handle(handle);
    if (!obj) {
        return LDSO_ERR_NOT_FOUND;
    }
    obj->handle = 0;
    return LDSO_OK;
}
