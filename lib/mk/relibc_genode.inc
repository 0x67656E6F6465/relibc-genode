#
# The Genode shim of relibc, shared by the static library (relibc_genode)
# and the drop-in libc (libc.lib.so)
#
SHIM_DIR    = $(REP_DIR)/src/lib/relibc_genode
RELIBC_DIR  = $(REP_DIR)/var/relibc
LIBC_API    = $(call select_from_repositories,include/libc)

SRC_CC     += start.cc fd.cc epoll.cc fs.cc memory.cc time.cc thread.cc libc_component.cc socket.cc signals.cc ep.cc exec.cc fork.cc
SRC_C      += init_array.c
INC_DIR    += $(SHIM_DIR) $(dir $(LIBC_API))
LIBS       += base vfs

vpath %.cc $(SHIM_DIR)
vpath %.c  $(SHIM_DIR)
