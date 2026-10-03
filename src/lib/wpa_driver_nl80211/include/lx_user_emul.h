/*
 * \brief  dde_linux's lx_user_emul.h (wpa_driver_nl80211), on relibc
 *
 * relibc defines some of its Linux constants itself, as macros, which
 * would turn the original's enumerators into numbers. With the same
 * values, the original's enumerators replace them.
 */

#ifndef _RELIBC_LX_USER_EMUL_H_
#define _RELIBC_LX_USER_EMUL_H_

#include <sys/socket.h>
#include <sys/ioctl.h>

#undef SO_PRIORITY     /* 12 */
#undef SIOCSIFHWADDR   /* 0x8924 */
#undef SIOCGIFHWADDR   /* 0x8927 */

#include_next <lx_user_emul.h>

#endif /* _RELIBC_LX_USER_EMUL_H_ */
