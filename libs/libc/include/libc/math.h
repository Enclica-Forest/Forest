/*
 * math.h - Mathematical functions
 * 
 * C23 compatible mathematical functions for Fern libc.
 */
#ifndef _MATH_H
#define _MATH_H

#define __STDC_VERSION_MATH_H__ 202311L

#ifdef __cplusplus
extern "C" {
#endif

/* Classification macros */
#define FP_NAN          0
#define FP_INFINITE     1
#define FP_ZERO         2
#define FP_SUBNORMAL    3
#define FP_NORMAL       4

/* Special values */
#define NAN             (__builtin_nanf(""))
#define INFINITY        (__builtin_inff())
#define HUGE_VAL        (__builtin_huge_val())
#define HUGE_VALF       (__builtin_huge_valf())
#define HUGE_VALL       (__builtin_huge_vall())

/* Mathematical constants */
#ifndef M_E
#define M_E             2.71828182845904523536028747135266250   /* e */
#endif
#ifndef M_LOG2E
#define M_LOG2E         1.44269504088896340735992468100189214   /* log2(e) */
#endif
#ifndef M_LOG10E
#define M_LOG10E        0.434294481903251827651128918916605082  /* log10(e) */
#endif
#ifndef M_LN2
#define M_LN2           0.693147180559945309417232121458176568  /* ln(2) */
#endif
#ifndef M_LN10
#define M_LN10          2.30258509299404568401799145468436421   /* ln(10) */
#endif
#ifndef M_PI
#define M_PI            3.14159265358979323846264338327950288   /* pi */
#endif
#ifndef M_PI_2
#define M_PI_2          1.57079632679489661923132169163975144   /* pi/2 */
#endif
#ifndef M_PI_4
#define M_PI_4          0.785398163397448309615660845819875721  /* pi/4 */
#endif
#ifndef M_1_PI
#define M_1_PI          0.318309886183790671537767526745028724  /* 1/pi */
#endif
#ifndef M_2_PI
#define M_2_PI          0.636619772367581343075535053490057448  /* 2/pi */
#endif
#ifndef M_2_SQRTPI
#define M_2_SQRTPI      1.12837916709551257389615890312154517   /* 2/sqrt(pi) */
#endif
#ifndef M_SQRT2
#define M_SQRT2         1.41421356237309504880168872420969808   /* sqrt(2) */
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2       0.707106781186547524400844362104849039  /* 1/sqrt(2) */
#endif

/* Classification functions (implemented as compiler built-ins where possible) */
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isfinite(x)   __builtin_isfinite(x)
#define isinf(x)      __builtin_isinf(x)
#define isnan(x)      __builtin_isnan(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)

/* Trigonometric functions */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);

long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double asinl(long double x);
long double acosl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);

/* Hyperbolic functions */
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);

long double sinhl(long double x);
long double coshl(long double x);
long double tanhl(long double x);
long double asinhl(long double x);
long double acoshl(long double x);
long double atanhl(long double x);

/* Exponential and logarithmic functions */
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log10(double x);
double log2(double x);
double log1p(double x);

float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log10f(float x);
float log2f(float x);
float log1pf(float x);

long double expl(long double x);
long double exp2l(long double x);
long double expm1l(long double x);
long double logl(long double x);
long double log10l(long double x);
long double log2l(long double x);
long double log1pl(long double x);

/* Power functions */
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);

float powf(float x, float y);
float sqrtf(float x);
float cbrtf(float x);
float hypotf(float x, float y);

long double powl(long double x, long double y);
long double sqrtl(long double x);
long double cbrtl(long double x);
long double hypotl(long double x, long double y);

/* Error and gamma functions */
double erf(double x);
double erfc(double x);
double lgamma(double x);
double tgamma(double x);

float erff(float x);
float erfcf(float x);
float lgammaf(float x);
float tgammaf(float x);

long double erfl(long double x);
long double erfcl(long double x);
long double lgammal(long double x);
long double tgammal(long double x);

/* Nearest integer functions */
double ceil(double x);
double floor(double x);
double trunc(double x);
double round(double x);
long lround(double x);
long long llround(double x);
double nearbyint(double x);
double rint(double x);
long lrint(double x);
long long llrint(double x);

float ceilf(float x);
float floorf(float x);
float truncf(float x);
float roundf(float x);
long lroundf(float x);
long long llroundf(float x);
float nearbyintf(float x);
float rintf(float x);
long lrintf(float x);
long long llrintf(float x);

long double ceill(long double x);
long double floorl(long double x);
long double truncl(long double x);
long double roundl(long double x);
long lroundl(long double x);
long long llroundl(long double x);
long double nearbyintl(long double x);
long double rintl(long double x);
long lrintl(long double x);
long long llrintl(long double x);

/* Remainder functions */
double fmod(double x, double y);
double remainder(double x, double y);
double remquo(double x, double y, int *quo);

float fmodf(float x, float y);
float remainderf(float x, float y);
float remquof(float x, float y, int *quo);

long double fmodl(long double x, long double y);
long double remainderl(long double x, long double y);
long double remquol(long double x, long double y, int *quo);

/* Manipulation functions */
double copysign(double x, double y);
double nan(const char *tagp);
double nextafter(double x, double y);
double nexttoward(double x, long double y);

float copysignf(float x, float y);
float nanf(const char *tagp);
float nextafterf(float x, float y);
float nexttowardf(float x, long double y);

long double copysignl(long double x, long double y);
long double nanl(const char *tagp);
long double nextafterl(long double x, long double y);
long double nexttowardl(long double x, long double y);

/* Maximum, minimum, and positive difference functions */
double fdim(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);

float fdimf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);

long double fdiml(long double x, long double y);
long double fmaxl(long double x, long double y);
long double fminl(long double x, long double y);

/* Fused multiply-add */
double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
long double fmal(long double x, long double y, long double z);

/* Absolute value functions */
double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);

/* Decomposition functions */
double frexp(double value, int *exp);
double ldexp(double x, int exp);
double modf(double value, double *iptr);
double scalbn(double x, int n);
double scalbln(double x, long n);
int ilogb(double x);
double logb(double x);

float frexpf(float value, int *exp);
float ldexpf(float x, int exp);
float modff(float value, float *iptr);
float scalbnf(float x, int n);
float scalblnf(float x, long n);
int ilogbf(float x);
float logbf(float x);

long double frexpl(long double value, int *exp);
long double ldexpl(long double x, int exp);
long double modfl(long double value, long double *iptr);
long double scalbnl(long double x, int n);
long double scalblnl(long double x, long n);
int ilogbl(long double x);
long double logbl(long double x);

/* Special constants for ilogb */
#define FP_ILOGB0       (-2147483647 - 1)
#define FP_ILOGBNAN     (-2147483647 - 1)

/* Comparison macros */
#define isgreater(x, y)         __builtin_isgreater(x, y)
#define isgreaterequal(x, y)    __builtin_isgreaterequal(x, y)
#define isless(x, y)            __builtin_isless(x, y)
#define islessequal(x, y)       __builtin_islessequal(x, y)
#define islessgreater(x, y)     __builtin_islessgreater(x, y)
#define isunordered(x, y)       __builtin_isunordered(x, y)

/* Bessel functions (POSIX XSI extension) */
double j0(double x);
double j1(double x);
double jn(int n, double x);
double y0(double x);
double y1(double x);
double yn(int n, double x);

/* Gamma sign (POSIX) */
extern int signgam;

#ifdef __cplusplus
}
#endif

#endif /* _MATH_H */
