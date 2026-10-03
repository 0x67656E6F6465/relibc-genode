#
# relibc as Genode's libc (libc.lib.so), for a build directory that lists
# this repository before libports (README: "relibc as the libc")
#
include $(REP_DIR)/lib/mk/relibc_genode.inc

LD_OPT    += --whole-archive $(RELIBC_DIR)/librelibc.a $(RELIBC_DIR)/libopenlibm.a --no-whole-archive

# relibc's writable data apart from the shim's, for fork (relibc_state.ld)
LD_OPT    += -T $(REP_DIR)/src/lib/relibc_genode/relibc_state.ld

# relibc's thread-locals by the shim's emulated TLS (thread.cc), not the
# dynamic linker's, whose state a forked child cannot take over
LD_OPT    += --wrap=__emutls_get_address
SHARED_LIB = yes


# relink after tools/build-relibc
$(LIB).lib.so: $(RELIBC_DIR)/librelibc.a $(RELIBC_DIR)/libopenlibm.a $(REP_DIR)/src/lib/relibc_genode/relibc_state.ld
