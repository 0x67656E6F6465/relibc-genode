/* AF_MAX as glibc's */
#include_next <sys/socket.h>
#ifndef AF_MAX
#define AF_MAX 46
#define PF_MAX AF_MAX
#endif
