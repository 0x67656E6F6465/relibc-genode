#
# Genode's posix library (main, from the config's arguments), on relibc:
# main in a thread of its own (src/lib/relibc_genode/posix_main.cc)
#
SRC_CC     = posix_main.cc
INC_DIR   += $(REP_DIR)/var/relibc/include $(dir $(call select_from_repositories,include/libc))
CC_OPT    += -D__relibc_linux_abi__
LIBS      += libc
SHARED_LIB = yes

vpath posix_main.cc $(REP_DIR)/src/lib/relibc_genode
