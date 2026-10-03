/* As with FreeBSD's headers: intptr_t only if no one declared it before
   (_INTPTR_T_DECLARED, e.g. pcg-c/genode_inttypes.h) */
#ifndef _INTPTR_T_DECLARED
#include_next <bits/intptr-t.h>
#define _INTPTR_T_DECLARED
#endif
