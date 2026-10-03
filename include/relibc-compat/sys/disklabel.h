/* FreeBSD's disk labels: no DIOCGDINFO, so code falls back to sys/disk.h */
#ifndef _RELIBC_COMPAT_SYS_DISKLABEL_H
#define _RELIBC_COMPAT_SYS_DISKLABEL_H
#include <sys/disk.h>
struct disklabel { int d_unused; };
#endif
