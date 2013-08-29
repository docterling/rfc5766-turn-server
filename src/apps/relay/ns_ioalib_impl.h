/*
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * IO Abstraction library
 */

#ifndef __IOA_LIBIMPL__
#define __IOA_LIBIMPL__

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#if !defined(TURN_NO_THREADS)
#include <event2/thread.h>
#endif

#include <openssl/ssl.h>

#include "ns_turn_ioalib.h"
#include "turn_ports.h"
#include "ns_turn_maps_rtcp.h"
#include "ns_turn_maps.h"
#include "ns_turn_server.h"

#include "apputils.h"
#include "stun_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

//////////////////////////////////////////////////////

#define TOO_BIG_BAD_TCP_MESSAGE (40000)
#define MAX_BUFFER_QUEUE_SIZE_PER_ENGINE (64)
#define MAX_SOCKET_BUFFER_BACKLOG (16)

#define BUFFEREVENT_HIGH_WATERMARK 65535

typedef struct _stun_buffer_list_elem {
	struct _stun_buffer_list_elem *next;
	struct _stun_buffer_list_elem *prev;
	stun_buffer buf;
} stun_buffer_list_elem;

typedef struct _stun_buffer_list {
	stun_buffer_list_elem *head;
	stun_buffer_list_elem *tail;
	size_t tsz;
} stun_buffer_list;

typedef unsigned long band_limit_t;
#define SECS_PER_JIFFIE (1)

/*
 * New connection callback
 */

enum _MESSAGE_TO_RELAY_TYPE {
	RMT_UNKNOWN,
	RMT_SOCKET,
	RMT_CB_SOCKET
};
typedef enum _MESSAGE_TO_RELAY_TYPE MESSAGE_TO_RELAY_TYPE;

struct message_to_relay {
	MESSAGE_TO_RELAY_TYPE t;
	union {
		struct socket_message sm;
		struct cb_socket_message cb_sm;
	} m;
};

struct relay_server;
typedef struct relay_server *relay_server_handle;

typedef int (*ioa_engine_new_connection_event_handler)(ioa_engine_handle e, struct message_to_relay *sm);
typedef int (*ioa_engine_udp_event_handler)(relay_server_handle rs, struct message_to_relay *sm);

#define TURN_CMSG_SZ (65536)

struct _ioa_engine
{
  struct event_base *event_base;
  ur_addr_map *children_ss; /* map of maps: local addr / remote addr */
  int deallocate_eb;
  s08bits relay_ifname[1025];
  size_t relays_number;
  size_t relay_addr_counter;
  ioa_addr *relay_addrs;
  int verbose;
  turnipports* tp;
  rtcp_map *map_rtcp;
  stun_buffer_list bufs;
  SSL_CTX *tls_ctx_ssl23;
  SSL_CTX *tls_ctx_v1_0;
#if defined(SSL_TXT_TLSV1_1)
  SSL_CTX *tls_ctx_v1_1;
#if defined(SSL_TXT_TLSV1_2)
  SSL_CTX *tls_ctx_v1_2;
#endif
#endif
  SSL_CTX *dtls_ctx;
  turn_time_t jiffie;
  band_limit_t max_bpj;
  ioa_timer_handle timer_ev;
  s08bits cmsg[TURN_CMSG_SZ+1];
};

#define SOCKET_MAGIC (0xABACADEF)

struct _ioa_socket
{
	evutil_socket_t fd;
	struct _ioa_socket *parent_s;
	void *listener_server;
	u32bits magic;
	ur_addr_map *sockets_container; /* relay container for UDP sockets */
	struct bufferevent *bev;
	ioa_network_buffer_handle defer_nbh;
	int family;
	SOCKET_TYPE st;
	SOCKET_APP_TYPE sat;
	SSL* ssl;
	int bound;
	int local_addr_known;
	ioa_addr local_addr;
	int connected;
	ioa_addr remote_addr;
	ioa_engine_handle e;
	struct event *read_event;
	ioa_net_event_handler read_cb;
	void *read_ctx;
	volatile int done;
	void* session;
	int current_df_relay_flag;
	/* RFC6156: if IPv6 is involved, do not use DF: */
	int do_not_use_df;
	int tobeclosed;
	int broken;
	int default_ttl;
	int current_ttl;
	int default_tos;
	int current_tos;
	stun_buffer_list bufs;
	turn_time_t jiffie;
	band_limit_t jiffie_bytes;
	/* RFC 6062 ==>> */
	//Connection session:
	void *sub_session;
	//Connect:
	struct bufferevent *conn_bev;
	connect_cb conn_cb;
	void *conn_arg;
	//Accept:
	struct evconnlistener *list_ev;
	accept_cb acb;
	void *acbarg;
	/* <<== RFC 6062 */
	const char *func;
	const char *file;
	int line;
};

typedef struct _timer_event
{
	struct event *ev;
	ioa_engine_handle e;
	ioa_timer_event_handler cb;
	void *ctx;
	s08bits* txt;
} timer_event;

/* engine handling */

ioa_engine_handle create_ioa_engine(struct event_base *eb, turnipports* tp, 
				    const s08bits* relay_if,
				    size_t relays_number, s08bits **relay_addrs,
				    int verbose, band_limit_t max_bps);

void set_ssl_ctx(ioa_engine_handle e,
		SSL_CTX *tls_ctx_ssl23,
		SSL_CTX *tls_ctx_v1_0,
#if defined(SSL_TXT_TLSV1_1)
		SSL_CTX *tls_ctx_v1_1,
#if defined(SSL_TXT_TLSV1_2)
		SSL_CTX *tls_ctx_v1_2,
#endif
#endif
		SSL_CTX *dtls_ctx);

void ioa_engine_set_rtcp_map(ioa_engine_handle e, rtcp_map *rtcpmap);

ioa_socket_handle create_ioa_socket_from_fd(ioa_engine_handle e, ioa_socket_raw fd, ioa_socket_handle parent_s, SOCKET_TYPE st, SOCKET_APP_TYPE sat, const ioa_addr *remote_addr, const ioa_addr *local_addr);
ioa_socket_handle create_ioa_socket_from_ssl(ioa_engine_handle e, ioa_socket_handle parent_s, SSL* ssl, SOCKET_TYPE st, SOCKET_APP_TYPE sat, const ioa_addr *remote_addr, const ioa_addr *local_addr);

void add_socket_to_parent(ioa_socket_handle parent_s, ioa_socket_handle s);
void delete_socket_from_parent(ioa_socket_handle s);

void add_socket_to_map(ioa_socket_handle s, ur_addr_map *amap);
void delete_socket_from_map(ioa_socket_handle s);

int is_connreset(void);
int would_block(void);
int udp_send(ioa_socket_handle s, const ioa_addr* dest_addr, const s08bits* buffer, int len);
int udp_recvfrom(evutil_socket_t fd, ioa_addr* orig_addr, const ioa_addr *like_addr, s08bits* buffer, int buf_size, int *ttl, int *tos, s08bits *ecmsg, int flags, u32bits *errcode);
int ssl_read(evutil_socket_t fd, SSL* ssl, s08bits* buffer, int buf_size, int verbose, int *read_len);

int set_raw_socket_ttl_options(evutil_socket_t fd, int family);
int set_raw_socket_tos_options(evutil_socket_t fd, int family);

int set_socket_options(ioa_socket_handle s);

/////////////////////////////////////////////////

#ifdef __cplusplus
}
#endif

#endif /* __IOA_LIBIMPL__ */
