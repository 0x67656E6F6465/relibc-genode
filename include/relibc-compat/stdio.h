#include_next <stdio.h>

/*
 * For ports whose Genode patches remove the *at() interface (AT_FDCWD), as
 * Genode's libc lacks it: without RENAME_NOREPLACE, they use rename()
 */
#ifdef RELIBC_COMPAT_NO_RENAMEAT2
#undef RENAME_NOREPLACE
#undef RENAME_EXCHANGE
#undef RENAME_WHITEOUT
#endif
