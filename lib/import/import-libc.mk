#
# Programs and libraries on relibc as the libc: relibc's C headers, which
# select its Linux ABI by __relibc_linux_abi__, and Genode's libc API headers
# (libc/component.h, libc/args.h, ...)
#
INC_DIR += $(call select_from_repositories,include/relibc-compat)
INC_DIR += $(call select_from_repositories,var/relibc/include)
INC_DIR += $(dir $(call select_from_repositories,include/libc))
CC_OPT  += -D__relibc_linux_abi__ -D__GENODE__

# relibc marks legacy functions (bzero, ...) deprecated
CC_OPT  += -Wno-error=deprecated-declarations

# hosted, not freestanding
CC_OPT_FREESTANDING =
