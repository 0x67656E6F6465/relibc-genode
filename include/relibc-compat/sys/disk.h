/* FreeBSD's disk ioctls; the shim answers DIOCGMEDIASIZE (the size of a
   file or block device of the VFS) and DIOCGSECTORSIZE */
#ifndef _RELIBC_COMPAT_SYS_DISK_H
#define _RELIBC_COMPAT_SYS_DISK_H

#include <sys/ioctl.h>  /* FreeBSD: through sys/ioccom.h */

#define DIOCGSECTORSIZE 0x40046480u  /* _IOR('d', 128, u_int) */
#define DIOCGMEDIASIZE  0x40086481u  /* _IOR('d', 129, off_t) */

#endif
