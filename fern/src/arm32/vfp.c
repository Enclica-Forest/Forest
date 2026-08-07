#include "vfp.h"

void vfp_init(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(val));
    val |= (0xF << 20);
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 2" : : "r"(val));
    __asm__ volatile("isb");
}

void vfp_save(vfp_context_t *ctx)
{
    uint32_t tmp;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(tmp));
    ctx->fpscr = tmp;
    __asm__ volatile("vmrs %0, fpexc" : "=r"(tmp));
    ctx->fpexc = tmp;
    __asm__ volatile("vstr s0, [%0, #0]" : : "r"(ctx->s));
    __asm__ volatile("vstr s1, [%0, #4]" : : "r"(ctx->s));
    __asm__ volatile("vstr s2, [%0, #8]" : : "r"(ctx->s));
    __asm__ volatile("vstr s3, [%0, #12]" : : "r"(ctx->s));
    __asm__ volatile("vstr s4, [%0, #16]" : : "r"(ctx->s));
    __asm__ volatile("vstr s5, [%0, #20]" : : "r"(ctx->s));
    __asm__ volatile("vstr s6, [%0, #24]" : : "r"(ctx->s));
    __asm__ volatile("vstr s7, [%0, #28]" : : "r"(ctx->s));
    __asm__ volatile("vstr s8, [%0, #32]" : : "r"(ctx->s));
    __asm__ volatile("vstr s9, [%0, #36]" : : "r"(ctx->s));
    __asm__ volatile("vstr s10, [%0, #40]" : : "r"(ctx->s));
    __asm__ volatile("vstr s11, [%0, #44]" : : "r"(ctx->s));
    __asm__ volatile("vstr s12, [%0, #48]" : : "r"(ctx->s));
    __asm__ volatile("vstr s13, [%0, #52]" : : "r"(ctx->s));
    __asm__ volatile("vstr s14, [%0, #56]" : : "r"(ctx->s));
    __asm__ volatile("vstr s15, [%0, #60]" : : "r"(ctx->s));
    __asm__ volatile("vstr s16, [%0, #64]" : : "r"(ctx->s));
    __asm__ volatile("vstr s17, [%0, #68]" : : "r"(ctx->s));
    __asm__ volatile("vstr s18, [%0, #72]" : : "r"(ctx->s));
    __asm__ volatile("vstr s19, [%0, #76]" : : "r"(ctx->s));
    __asm__ volatile("vstr s20, [%0, #80]" : : "r"(ctx->s));
    __asm__ volatile("vstr s21, [%0, #84]" : : "r"(ctx->s));
    __asm__ volatile("vstr s22, [%0, #88]" : : "r"(ctx->s));
    __asm__ volatile("vstr s23, [%0, #92]" : : "r"(ctx->s));
    __asm__ volatile("vstr s24, [%0, #96]" : : "r"(ctx->s));
    __asm__ volatile("vstr s25, [%0, #100]" : : "r"(ctx->s));
    __asm__ volatile("vstr s26, [%0, #104]" : : "r"(ctx->s));
    __asm__ volatile("vstr s27, [%0, #108]" : : "r"(ctx->s));
    __asm__ volatile("vstr s28, [%0, #112]" : : "r"(ctx->s));
    __asm__ volatile("vstr s29, [%0, #116]" : : "r"(ctx->s));
    __asm__ volatile("vstr s30, [%0, #120]" : : "r"(ctx->s));
    __asm__ volatile("vstr s31, [%0, #124]" : : "r"(ctx->s));
}

void vfp_restore(vfp_context_t *ctx)
{
    uint32_t tmp;
    __asm__ volatile("vldr s0, [%0, #0]" : : "r"(ctx->s));
    __asm__ volatile("vldr s1, [%0, #4]" : : "r"(ctx->s));
    __asm__ volatile("vldr s2, [%0, #8]" : : "r"(ctx->s));
    __asm__ volatile("vldr s3, [%0, #12]" : : "r"(ctx->s));
    __asm__ volatile("vldr s4, [%0, #16]" : : "r"(ctx->s));
    __asm__ volatile("vldr s5, [%0, #20]" : : "r"(ctx->s));
    __asm__ volatile("vldr s6, [%0, #24]" : : "r"(ctx->s));
    __asm__ volatile("vldr s7, [%0, #28]" : : "r"(ctx->s));
    __asm__ volatile("vldr s8, [%0, #32]" : : "r"(ctx->s));
    __asm__ volatile("vldr s9, [%0, #36]" : : "r"(ctx->s));
    __asm__ volatile("vldr s10, [%0, #40]" : : "r"(ctx->s));
    __asm__ volatile("vldr s11, [%0, #44]" : : "r"(ctx->s));
    __asm__ volatile("vldr s12, [%0, #48]" : : "r"(ctx->s));
    __asm__ volatile("vldr s13, [%0, #52]" : : "r"(ctx->s));
    __asm__ volatile("vldr s14, [%0, #56]" : : "r"(ctx->s));
    __asm__ volatile("vldr s15, [%0, #60]" : : "r"(ctx->s));
    __asm__ volatile("vldr s16, [%0, #64]" : : "r"(ctx->s));
    __asm__ volatile("vldr s17, [%0, #68]" : : "r"(ctx->s));
    __asm__ volatile("vldr s18, [%0, #72]" : : "r"(ctx->s));
    __asm__ volatile("vldr s19, [%0, #76]" : : "r"(ctx->s));
    __asm__ volatile("vldr s20, [%0, #80]" : : "r"(ctx->s));
    __asm__ volatile("vldr s21, [%0, #84]" : : "r"(ctx->s));
    __asm__ volatile("vldr s22, [%0, #88]" : : "r"(ctx->s));
    __asm__ volatile("vldr s23, [%0, #92]" : : "r"(ctx->s));
    __asm__ volatile("vldr s24, [%0, #96]" : : "r"(ctx->s));
    __asm__ volatile("vldr s25, [%0, #100]" : : "r"(ctx->s));
    __asm__ volatile("vldr s26, [%0, #104]" : : "r"(ctx->s));
    __asm__ volatile("vldr s27, [%0, #108]" : : "r"(ctx->s));
    __asm__ volatile("vldr s28, [%0, #112]" : : "r"(ctx->s));
    __asm__ volatile("vldr s29, [%0, #116]" : : "r"(ctx->s));
    __asm__ volatile("vldr s30, [%0, #120]" : : "r"(ctx->s));
    __asm__ volatile("vldr s31, [%0, #124]" : : "r"(ctx->s));
    tmp = ctx->fpscr;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(tmp));
    tmp = ctx->fpexc;
    __asm__ volatile("vmsr fpexc, %0" : : "r"(tmp));
}
