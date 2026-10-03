/* The mount table of Linux's libc: Genode has none, so no entries */
#ifndef _RELIBC_COMPAT_MNTENT_H
#define _RELIBC_COMPAT_MNTENT_H

#include <stdio.h>

#define MOUNTED "/etc/mtab"
#define MNTTYPE_SWAP "swap"

struct mntent { char *mnt_fsname, *mnt_dir, *mnt_type, *mnt_opts; int mnt_freq, mnt_passno; };

static inline FILE          *setmntent(char const *f, char const *m) { (void)f; (void)m; return 0; }
static inline struct mntent *getmntent(FILE *f) { (void)f; return 0; }
static inline int            endmntent(FILE *f) { (void)f; return 1; }
static inline char          *hasmntopt(struct mntent const *m, char const *o) { (void)m; (void)o; return 0; }

#endif
