/* relibc's limits.h defines PAGE_SIZE (as musl does); FreeBSD's does not,
   and Genode's C++ headers use the name themselves */
#include_next <limits.h>
#undef PAGE_SIZE
