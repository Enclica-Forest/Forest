#ifndef LDSO_H
#define LDSO_H

#include <stddef.h>
#include "types.h"
#include "elf.h"
#include "memory.h"

#define LDSO_MAX_OBJECT_NAME       64
#define LDSO_MAX_OBJECT_PATH       256
#define LDSO_MAX_NEEDED_LIBS       32
#define LDSO_MAX_SEARCH_PATHS      32
#define LDSO_MAX_SEARCH_PATH_LEN   128

#define DT_NULL       0
#define DT_NEEDED     1
#define DT_PLTRELSZ   2
#define DT_PLTGOT     3
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ      10
#define DT_SYMENT     11
#define DT_INIT       12
#define DT_FINI       13
#define DT_SONAME     14
#define DT_RPATH      15
#define DT_SYMBOLIC   16
#define DT_REL        17
#define DT_RELSZ      18
#define DT_RELENT     19
#define DT_PLTREL     20
#define DT_DEBUG      21
#define DT_TEXTREL    22
#define DT_JMPREL     23
#define DT_RUNPATH    29
#define DT_FLAGS      30
#define DT_VERSYM     0x6FFFFFF0
#define DT_VERDEF     0x6FFFFFFC
#define DT_VERDEFNUM  0x6FFFFFFD
#define DT_VERNEED    0x6FFFFFFE
#define DT_VERNEEDNUM 0x6FFFFFFF

#define DF_ORIGIN     0x00000001
#define DF_SYMBOLIC   0x00000002
#define DF_TEXTREL    0x00000004
#define DF_BIND_NOW   0x00000008
#define DF_STATIC_TLS 0x00000010

typedef enum {
    LDSO_OK = 0,
    LDSO_ERR_INVALID_ARG = -1,
    LDSO_ERR_INVALID_ELF = -2,
    LDSO_ERR_BOUNDS = -3,
    LDSO_ERR_NO_DYNAMIC = -4,
    LDSO_ERR_UNSUPPORTED = -5,
    LDSO_ERR_NOMEM = -6,
    LDSO_ERR_NOT_FOUND = -7,
    LDSO_ERR_DUPLICATE = -8,
    LDSO_ERR_BAD_STATE = -9
} ldso_status_t;

typedef struct {
    bool present;
    uint32 raw_ptr;
    const void* file_ptr;
    uint32 runtime_addr;
} ldso_ptr_info_t;

typedef struct {
    const elf32_dyn_t* dynamic;
    uint32 dynamic_count;

    ldso_ptr_info_t strtab;
    uint32 strsz;

    ldso_ptr_info_t symtab;
    uint32 syment;

    ldso_ptr_info_t hash;
    uint32 nbucket;
    uint32 nchain;

    ldso_ptr_info_t rel;
    uint32 relsz;
    uint32 relent;
    uint32 rel_count;

    ldso_ptr_info_t rela;
    uint32 relasz;
    uint32 relaent;
    uint32 rela_count;

    ldso_ptr_info_t jmprel;
    uint32 pltrelsz;
    uint32 pltrel_type;
    ldso_ptr_info_t versym;
    ldso_ptr_info_t verdef;
    ldso_ptr_info_t verneed;
    uint32 verdefnum;
    uint32 verneednum;

    uint32 flags;
    bool has_textrel;

    const char* soname;
    const char* rpath;
    const char* runpath;
    uint32 soname_offset;
    uint32 rpath_offset;
    uint32 runpath_offset;

    uint32 needed_name_offsets[LDSO_MAX_NEEDED_LIBS];
    uint32 needed_count;
    const char* needed[LDSO_MAX_NEEDED_LIBS];
} ldso_dynamic_info_t;

typedef struct ldso_object {
    char name[LDSO_MAX_OBJECT_NAME];
    char path[LDSO_MAX_OBJECT_PATH];

    const uint8* elf_image;
    size_t elf_size;

    const elf32_ehdr_t* ehdr;
    const elf32_phdr_t* phdrs;
    uint16 phnum;

    uint32 min_vaddr;
    uint32 max_vaddr;
    uint32 load_bias;
    void* mapped_image;
    uint32 mapped_size;
    uint32 handle;

    ldso_dynamic_info_t dyn;

    struct ldso_object* next;
} ldso_object_t;

typedef struct {
    uint32 r_offset;
    uint32 r_info;
    uint32 type;
    uint32 sym_index;
    int32 addend;
    bool is_rela;
    bool from_plt;
} ldso_reloc_entry_t;

typedef struct {
    char entries[LDSO_MAX_SEARCH_PATHS][LDSO_MAX_SEARCH_PATH_LEN];
    uint32 count;
} ldso_search_path_list_t;

typedef int (*ldso_reloc_walk_cb)(const ldso_object_t* obj,
                                  const ldso_reloc_entry_t* reloc,
                                  void* ctx);

typedef int (*ldso_symbol_resolver_t)(const ldso_object_t* requester,
                                      const char* symbol_name,
                                      uint32* out_addr,
                                      void* ctx);

void ldso_init(void);

int ldso_object_init_from_elf_image(ldso_object_t* obj,
                                    const char* name,
                                    const char* path,
                                    const uint8* elf_image,
                                    size_t elf_size,
                                    uint32 runtime_base);
int ldso_parse_dynamic(ldso_object_t* obj);

int ldso_register_object(ldso_object_t* obj);
void ldso_unregister_object(ldso_object_t* obj);
const ldso_object_t* ldso_find_object_by_name(const char* name);
const ldso_object_t* ldso_get_object_list(void);

int ldso_lookup_symbol_in_object(const ldso_object_t* obj,
                                 const char* symbol_name,
                                 uint32* out_addr,
                                 const elf32_sym_t** out_sym);
int ldso_lookup_symbol_global(const char* symbol_name,
                              const ldso_object_t* requester,
                              uint32* out_addr,
                              const ldso_object_t** out_owner,
                              const elf32_sym_t** out_sym);

int ldso_walk_relocations(const ldso_object_t* obj,
                          ldso_reloc_walk_cb cb,
                          void* ctx);
int ldso_apply_relocations_i386(ldso_object_t* obj,
                                ldso_symbol_resolver_t resolver,
                                void* resolver_ctx);
int ldso_validate_relocations_i386(const ldso_object_t* obj);

int ldso_exec_handoff(const uint8* elf_data,
                      uint32 elf_size,
                      const char* interp_path,
                      const elf_load_info_t* load_info,
                      uint32* out_entry_point);
int ldso_apply_rel_in_loaded_address_space(const uint8* elf_data,
                                           uint32 elf_size,
                                           const elf_load_info_t* load_info,
                                           page_directory_t* target_pd);
int ldso_apply_rela_in_loaded_address_space(const uint8* elf_data,
                                            uint32 elf_size,
                                            const elf_load_info_t* load_info,
                                            page_directory_t* target_pd);

int ldso_build_search_paths(const char* ld_library_path,
                            ldso_search_path_list_t* out_paths);
int ldso_resolve_library_path(const char* soname,
                              const char* ld_library_path,
                              char* out_path,
                              size_t out_path_size);
int ldso_dlopen_path(const char* path, int flags, uint32* out_handle);
int ldso_dlsym_by_handle(uint32 handle, const char* symbol, uint32* out_addr);
int ldso_dlclose_handle(uint32 handle);

#endif
