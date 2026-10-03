#
# wpa_driver_nl80211 on relibc: dde_linux's, built from there (REP_DIR), with glibc's
# byte-order headers, which wpa_supplicant includes for Linux only (and
# sys/endian.h for FreeBSD, as which Genode's libc identifies itself)
#

RELIBC_GENODE_DIR := $(REP_DIR)
override REP_DIR  := $(firstword $(filter-out $(RELIBC_GENODE_DIR),\
                       $(patsubst %/lib/mk/wpa_driver_nl80211.mk,%,\
                         $(wildcard $(addsuffix /lib/mk/wpa_driver_nl80211.mk,$(REPOSITORIES))))))

include $(REP_DIR)/lib/mk/wpa_driver_nl80211.mk

CC_OPT += -D__LITTLE_ENDIAN=__ORDER_LITTLE_ENDIAN__ -D__BIG_ENDIAN=__ORDER_BIG_ENDIAN__ \
          -D__BYTE_ORDER=__BYTE_ORDER__
CC_OPT += -include $(RELIBC_GENODE_DIR)/include/relibc-compat/byteswap.h

# lx_user_emul.h, ahead of dde_linux's
INC_DIR := $(RELIBC_GENODE_DIR)/src/lib/wpa_driver_nl80211/include $(INC_DIR)
