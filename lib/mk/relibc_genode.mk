#
# relibc as a static library, for programs that name it (LIBS +=
# relibc_genode, see lib/import/import-relibc_genode.mk): the shim, the
# start of main (posix_main.cc, as Genode's posix construct.cc), and relibc
#
include $(REP_DIR)/lib/mk/relibc_genode.inc

SRC_CC  += posix_main.cc
INC_DIR += $(RELIBC_DIR)/include
CC_OPT_posix_main += -D__relibc_linux_abi__
