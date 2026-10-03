#
# The test program, the shim, and relibc itself: the library and C headers
# that tools/build-relibc made (the depot build has no Rust toolchain)
#
DIRS  = src/test/relibc_hello src/lib/relibc_genode var/relibc/include
FILES = lib/mk/relibc_genode.mk lib/import/import-relibc_genode.mk var/relibc/librelibc.a

content: $(DIRS) $(FILES) LICENSE.relibc

$(DIRS):
	mkdir -p $@
	cp -r $(REP_DIR)/$@/* $@/

$(FILES) LICENSE.relibc:
	mkdir -p $(dir $@)
	cp $(REP_DIR)/$@ $@
