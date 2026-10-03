#
# What a program on relibc gets with 'LIBS += relibc_genode': relibc's C
# headers, which select its Linux ABI by __relibc_linux_abi__, relibc itself linked
# statically. tools/build-relibc makes var/relibc/.
#
RELIBC_DIR    = $(call select_from_repositories,var/relibc)
INC_DIR      += $(call select_from_repositories,include/relibc-compat)
INC_DIR      += $(RELIBC_DIR)/include
CC_C_OPT     += -D__relibc_linux_abi__
EXT_OBJECTS  += $(RELIBC_DIR)/librelibc.a $(RELIBC_DIR)/libopenlibm.a
