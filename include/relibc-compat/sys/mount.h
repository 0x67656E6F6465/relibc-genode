/* FreeBSD's mount interface: Genode has no mounts, so none are listed */
#ifndef _RELIBC_COMPAT_SYS_MOUNT_H
#define _RELIBC_COMPAT_SYS_MOUNT_H

#include <sys/statvfs.h>

#define MNT_WAIT   1
#define MNT_NOWAIT 2
#define MNT_RDONLY 0x1

struct statfs {
	unsigned long f_flags, f_bsize, f_blocks, f_bfree, f_bavail;
	char f_mntonname[88], f_mntfromname[88], f_fstypename[16];
};

static inline int getmntinfo(struct statfs **mp, int flags) { (void)flags; *mp = 0; return 0; }

#endif
