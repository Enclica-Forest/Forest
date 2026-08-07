#ifndef ARM32_VFP_H
#define ARM32_VFP_H

#include <stdint.h>

typedef struct {
    uint32_t fpexc;
    uint32_t fpscr;
    uint32_t s[32];
} vfp_context_t;

void vfp_init(void);
void vfp_save(vfp_context_t *ctx);
void vfp_restore(vfp_context_t *ctx);

#endif
