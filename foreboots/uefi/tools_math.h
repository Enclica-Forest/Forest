/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_math.h - "Math" tool category: number theory & integer calculators.
 * =============================================================================
 * Pure compute/draw tools (integer / fixed-point math only, no libc, no float,
 * no heap). Each open() follows template B: it calls wm_open() and returns; the
 * bootx64.c menu loop drives drawing + input. No firmware services are needed,
 * so this category has NO cat_math_init().
 *
 * Exports the category table consumed by uefi/tools_registry.c.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_MATH_H
#define FOREB_UEFI_TOOLS_MATH_H

#include "tools.h"   /* struct forebo_tool */

/* Per-tool template-B openers. */
void tool_math_prime_open(void);    /* prime checker + next prime            */
void tool_math_factorial_open(void);/* n! (64-bit, capped)                    */
void tool_math_fib_open(void);      /* Fibonacci sequence                     */
void tool_math_gcdlcm_open(void);   /* GCD / LCM of two integers              */
void tool_math_quad_open(void);     /* integer quadratic solver (discriminant)*/
void tool_math_factor_open(void);   /* prime factorizer                       */
void tool_math_modpow_open(void);   /* modular power a^b mod m                 */
void tool_math_isqrt_open(void);    /* integer sqrt + perfect-square test      */
void tool_math_pascal_open(void);   /* Pascal's triangle (scroll)             */

/* Category registry exports (defined in uefi/tools_math.c). */
extern const struct forebo_tool cat_math_tools[];
extern const int                cat_math_count;

#endif /* FOREB_UEFI_TOOLS_MATH_H */
