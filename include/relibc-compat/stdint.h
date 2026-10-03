/* FreeBSD marks the types of stdint.h as declared; Genode's headers
   (e.g. pcg-c/genode_inttypes.h) test those marks */
#include_next <stdint.h>
#ifndef _RELIBC_COMPAT_STDINT_H
#define _RELIBC_COMPAT_STDINT_H
#define _INT8_T_DECLARED
#define _INT16_T_DECLARED
#define _INT32_T_DECLARED
#define _INT64_T_DECLARED
#define _UINT8_T_DECLARED
#define _UINT16_T_DECLARED
#define _UINT32_T_DECLARED
#define _UINT64_T_DECLARED
#define _INTPTR_T_DECLARED
#define _UINTPTR_T_DECLARED
#define _INTMAX_T_DECLARED
#define _UINTMAX_T_DECLARED
#endif
