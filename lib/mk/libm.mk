#
# libm: the math functions (openlibm, as relibc builds it) are part of
# libc.lib.so; this library only exists for programs that name it
#
SRC_C      = libm.c
SHARED_LIB = yes

vpath libm.c $(REP_DIR)/src/lib/libm
