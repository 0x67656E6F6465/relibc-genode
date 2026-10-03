/*
 * \brief  Emulation of the Linux userland API for libnl, on relibc
 *
 * In place of dde_linux's src/lib/libnl/include/libnl_emul.h, which defines
 * what Genode's FreeBSD-based libc lacks. relibc has Linux's socket types
 * and constants (in6_addr, sockaddr_in6, ucred, SO_PASSCRED,
 * SCM_CREDENTIALS) itself.
 */

#ifndef _LIBNL_EMUL_H_
#define _LIBNL_EMUL_H_

#define __LITTLE_ENDIAN_BITFIELD 1

#include <linux/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <bits/ucred.h>   /* struct ucred, which sys/socket.h has with _GNU_SOURCE */

/* socket types should be taken from libc */
#define _BITS_SOCKADDR_H

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif

#endif /* _LIBNL_EMUL_H_ */
