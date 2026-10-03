/* glibc's byte-order macros (__BYTE_ORDER, ...), besides relibc's
   BYTE_ORDER and the hto* functions */
#include_next <endian.h>
#ifndef _RELIBC_COMPAT_ENDIAN_H
#define _RELIBC_COMPAT_ENDIAN_H
#ifndef __BYTE_ORDER
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN    __ORDER_BIG_ENDIAN__
#define __PDP_ENDIAN    __ORDER_PDP_ENDIAN__
#define __BYTE_ORDER    __BYTE_ORDER__
#define __FLOAT_WORD_ORDER __FLOAT_WORD_ORDER__
#endif
#endif
