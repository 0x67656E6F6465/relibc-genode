/* FreeBSD's (and glibc's) sys/types.h provides fd_set and FD_* */
#include_next <sys/types.h>
#ifndef _RELIBC_COMPAT_SYS_TYPES_SELECT
#define _RELIBC_COMPAT_SYS_TYPES_SELECT
#include <sys/select.h>

/* FreeBSD's sys/types.h defines the device-number macros (makedev, major,
   minor), relibc's sys/sysmacros.h does (as glibc's) */
#include <sys/sysmacros.h>
#endif
