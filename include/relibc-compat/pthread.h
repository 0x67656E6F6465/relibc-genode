/* FreeBSD names the type of the mutex kinds 'enum pthread_mutextype';
   relibc's kinds are integer constants */
#include_next <pthread.h>
#ifndef _RELIBC_COMPAT_PTHREAD_H
#define _RELIBC_COMPAT_PTHREAD_H
typedef int pthread_mutextype;
#endif
