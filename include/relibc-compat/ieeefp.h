/* FreeBSD's ieeefp.h: finite and finitef, the old names of isfinite
   (relibc has them, openlibm's math.h does not declare them) */
#ifndef _RELIBC_COMPAT_IEEEFP_H
#define _RELIBC_COMPAT_IEEEFP_H
#include <math.h>
#ifdef __cplusplus
extern "C" {
#endif
int finite(double x);
int finitef(float x);
#ifdef __cplusplus
}
#endif
#endif
