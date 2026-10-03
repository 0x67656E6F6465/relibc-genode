#
# lighttpd on relibc: Genode's target (repos/ports/src/app/lighttpd), which
# this one takes the place of in a build directory that lists this
# repository first. Genode's mod_webdav.patch removes AT_FDCWD (Genode's
# libc lacks the *at() interface); relibc's stdio.h would still make
# lighttpd use renameat2(AT_FDCWD, ...).
#

LIGHTTPD_GENODE_DIR := $(dir $(call select_from_repositories,src/app/lighttpd/plugin-static.h))

TARGET = lighttpd

include $(LIGHTTPD_GENODE_DIR)target.inc

INC_DIR += $(LIGHTTPD_GENODE_DIR)

LIBS += libc libm pcre

CC_OPT += -DRELIBC_COMPAT_NO_RENAMEAT2

CC_CXX_WARN_STRICT =
