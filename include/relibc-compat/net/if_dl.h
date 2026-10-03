/* BSD's link-level socket address (FreeBSD net/if_dl.h). Genode has no
   network interfaces to enumerate this way; the type is for code that
   names it */
#ifndef _RELIBC_COMPAT_NET_IF_DL_H
#define _RELIBC_COMPAT_NET_IF_DL_H

#include <sys/types.h>

#ifndef AF_LINK
#define AF_LINK 18
#endif

struct sockaddr_dl {
	unsigned char  sdl_len, sdl_family;
	unsigned short sdl_index;
	unsigned char  sdl_type, sdl_nlen, sdl_alen, sdl_slen;
	char           sdl_data[46];
};

#define LLADDR(s) ((char *)((s)->sdl_data + (s)->sdl_nlen))

#endif
