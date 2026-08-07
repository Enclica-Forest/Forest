#ifndef SESSION_DM_H
#define SESSION_DM_H

#include <stdint.h>
#include <stdbool.h>

/* Flags for SYS_DM_REGISTER */
#define DM_FLAG_EXCLUSIVE_FB    (1u << 0)   /* DM wants exclusive framebuffer access */
#define DM_FLAG_MANAGE_INPUT    (1u << 1)   /* DM handles all input (suppress TTY echo) */

/* Maximum length for DM name string */
#define DM_NAME_MAX 32

/* Passed to SYS_DM_REGISTER */
typedef struct {
    char     name[DM_NAME_MAX]; /* DM identifier string e.g. "ForestDM" */
    uint32_t flags;             /* DM_FLAG_* bitmask */
    uint32_t version;           /* DM API version (currently 1) */
} dm_register_info_t;

/* Returned by SYS_DM_REGISTER on success (token) */
typedef int32_t dm_token_t;
#define DM_TOKEN_INVALID (-1)

/* Passed to SYS_DM_AUTH_REPORT — authenticated user info */
typedef struct {
    char     username[16];
    uint32_t uid;
    uint32_t gid;
    uint32_t groups_mask;
    uint32_t flags;
} dm_auth_result_t;

/* Returned by SYS_DM_GET_SESSION */
typedef struct {
    uint32_t session_id;
    uint32_t uid;
    uint32_t gid;
    bool     dm_active;        /* A DM is currently registered */
    char     dm_name[DM_NAME_MAX];
} dm_session_info_t;

/* Error codes returned by DM syscalls */
#define DM_OK            0
#define DM_ERR_BUSY     (-1)  /* Another DM already registered */
#define DM_ERR_PERM     (-2)  /* Caller not allowed to register DM */
#define DM_ERR_INVAL    (-3)  /* Bad arguments */
#define DM_ERR_TOKEN    (-4)  /* Invalid or expired token */

#endif /* SESSION_DM_H */
