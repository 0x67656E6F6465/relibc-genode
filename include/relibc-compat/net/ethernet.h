/*
 * net/ethernet.h (glibc, FreeBSD), which relibc lacks: Ethernet addresses
 * and headers, with Linux's constants (linux/if_ether.h) as glibc's
 */
#ifndef _RELIBC_COMPAT_NET_ETHERNET_H
#define _RELIBC_COMPAT_NET_ETHERNET_H

#include <stdint.h>
#include <sys/types.h>

#ifndef ETH_ALEN
#define ETH_ALEN      6      /* octets in one address */
#define ETH_TLEN      2      /* octets in the type */
#define ETH_HLEN      14     /* octets in the header */
#define ETH_ZLEN      60     /* minimum frame, without FCS */
#define ETH_DATA_LEN  1500   /* maximum payload */
#define ETH_FRAME_LEN 1514   /* maximum frame, without FCS */
#define ETH_FCS_LEN   4
#endif

#ifndef ETH_P_ALL
#define ETH_P_ALL     0x0003
#define ETH_P_IP      0x0800
#define ETH_P_ARP     0x0806
#define ETH_P_8021Q   0x8100
#define ETH_P_IPV6    0x86DD
#define ETH_P_PAE     0x888E
#endif

struct ether_addr
{
	uint8_t ether_addr_octet[ETH_ALEN];
} __attribute__((__packed__));

struct ether_header
{
	uint8_t  ether_dhost[ETH_ALEN];
	uint8_t  ether_shost[ETH_ALEN];
	uint16_t ether_type;            /* network byte order */
} __attribute__((__packed__));

#define ETHERTYPE_PUP      0x0200
#define ETHERTYPE_SPRITE   0x0500
#define ETHERTYPE_IP       0x0800
#define ETHERTYPE_ARP      0x0806
#define ETHERTYPE_REVARP   0x8035
#define ETHERTYPE_AT       0x809B
#define ETHERTYPE_AARP     0x80F3
#define ETHERTYPE_VLAN     0x8100
#define ETHERTYPE_IPX      0x8137
#define ETHERTYPE_IPV6     0x86DD
#define ETHERTYPE_LOOPBACK 0x9000

#define ETHER_ADDR_LEN  ETH_ALEN
#define ETHER_TYPE_LEN  2
#define ETHER_CRC_LEN   4
#define ETHER_HDR_LEN   ETH_HLEN
#define ETHER_MIN_LEN   (ETH_ZLEN + ETHER_CRC_LEN)
#define ETHER_MAX_LEN   (ETH_FRAME_LEN + ETHER_CRC_LEN)

#endif
