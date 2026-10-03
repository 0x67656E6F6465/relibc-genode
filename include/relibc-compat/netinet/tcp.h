/* struct tcp_info (Linux, as glibc's netinet/tcp.h), which relibc lacks;
   no socket of Genode's reports it (getsockopt TCP_INFO) */
#include_next <netinet/tcp.h>
#ifndef _RELIBC_COMPAT_NETINET_TCP_H
#define _RELIBC_COMPAT_NETINET_TCP_H
#include <stdint.h>

struct tcp_info
{
	uint8_t  tcpi_state;
	uint8_t  tcpi_ca_state;
	uint8_t  tcpi_retransmits;
	uint8_t  tcpi_probes;
	uint8_t  tcpi_backoff;
	uint8_t  tcpi_options;
	uint8_t  tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;

	uint32_t tcpi_rto;
	uint32_t tcpi_ato;
	uint32_t tcpi_snd_mss;
	uint32_t tcpi_rcv_mss;

	uint32_t tcpi_unacked;
	uint32_t tcpi_sacked;
	uint32_t tcpi_lost;
	uint32_t tcpi_retrans;
	uint32_t tcpi_fackets;

	uint32_t tcpi_last_data_sent;
	uint32_t tcpi_last_ack_sent;
	uint32_t tcpi_last_data_recv;
	uint32_t tcpi_last_ack_recv;

	uint32_t tcpi_pmtu;
	uint32_t tcpi_rcv_ssthresh;
	uint32_t tcpi_rtt;
	uint32_t tcpi_rttvar;
	uint32_t tcpi_snd_ssthresh;
	uint32_t tcpi_snd_cwnd;
	uint32_t tcpi_advmss;
	uint32_t tcpi_reordering;

	uint32_t tcpi_rcv_rtt;
	uint32_t tcpi_rcv_space;

	uint32_t tcpi_total_retrans;
};

#endif
