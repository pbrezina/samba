/*
   Unix SMB/CIFS implementation.
   name query routines
   Copyright (C) Andrew Tridgell 1994-1998
   Copyright (C) Jeremy Allison 2007.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "libsmb/namequery.h"
#include "../lib/util/tevent_ntstatus.h"
#include "libads/sitename_cache.h"
#include "../lib/addns/dnsquery.h"
#include "../libcli/netlogon/netlogon.h"
#include "lib/async_req/async_sock.h"
#include "lib/tsocket/tsocket.h"
#include "libsmb/nmblib.h"
#include "libsmb/unexpected.h"
#include "../libcli/nbt/libnbt.h"
#include "libads/kerberos_proto.h"
#include "lib/gencache.h"
#include "librpc/gen_ndr/dns.h"
#include "lib/util/util_net.h"
#include "lib/util/string_wrappers.h"

/* nmbd.c sets this to True. */
bool global_in_nmbd = False;

/*
 * Utility function that copes only with AF_INET and AF_INET6
 * as that's all we're going to get out of DNS / NetBIOS / WINS
 * name resolution functions.
 */

bool sockaddr_storage_to_samba_sockaddr(struct samba_sockaddr *sa,
					const struct sockaddr_storage *ss)
{
	sa->u.ss = *ss;

	switch (ss->ss_family) {
	case AF_INET:
		sa->sa_socklen = sizeof(struct sockaddr_in);
		break;
#ifdef HAVE_IPV6
	case AF_INET6:
		sa->sa_socklen = sizeof(struct sockaddr_in6);
		break;
#endif
	default:
		return false;
	}
	return true;
}

/*
 * Utility function to convert from a struct ip_service
 * array to a struct samba_sockaddr array. Will go away
 * once ip_service is gone.
 */

static NTSTATUS ip_service_to_samba_sockaddr(TALLOC_CTX *ctx,
				struct samba_sockaddr **sa_out,
				const struct ip_service *iplist_in,
				size_t count)
{
	struct samba_sockaddr *sa = NULL;
	size_t i;
	bool ok;

	if (count == 0) {
		/*
		 * Zero length arrays are returned as NULL.
		 * in the name resolution code.
		 */
		*sa_out = NULL;
		return NT_STATUS_OK;
	}
	sa = talloc_zero_array(ctx,
				struct samba_sockaddr,
				count);
	if (sa == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	for (i = 0; i < count; i++) {
		ok = sockaddr_storage_to_samba_sockaddr(&sa[i],
						&iplist_in[i].ss);
		if (!ok) {
			TALLOC_FREE(sa);
			return NT_STATUS_INVALID_PARAMETER;
		}
	}
	*sa_out = sa;
	return NT_STATUS_OK;
}

/****************************
 * SERVER AFFINITY ROUTINES *
 ****************************/

 /* Server affinity is the concept of preferring the last domain
    controller with whom you had a successful conversation */

/****************************************************************************
****************************************************************************/
#define SAFKEY_FMT	"SAF/DOMAIN/%s"
#define SAF_TTL		900
#define SAFJOINKEY_FMT	"SAFJOIN/DOMAIN/%s"
#define SAFJOIN_TTL	3600

static char *saf_key(TALLOC_CTX *mem_ctx, const char *domain)
{
	return talloc_asprintf_strupper_m(mem_ctx, SAFKEY_FMT, domain);
}

static char *saf_join_key(TALLOC_CTX *mem_ctx, const char *domain)
{
	return talloc_asprintf_strupper_m(mem_ctx, SAFJOINKEY_FMT, domain);
}

/****************************************************************************
****************************************************************************/

bool saf_store( const char *domain, const char *servername )
{
	char *key;
	time_t expire;
	bool ret = False;

	if ( !domain || !servername ) {
		DEBUG(2,("saf_store: "
			"Refusing to store empty domain or servername!\n"));
		return False;
	}

	if ( (strlen(domain) == 0) || (strlen(servername) == 0) ) {
		DEBUG(0,("saf_store: "
			"refusing to store 0 length domain or servername!\n"));
		return False;
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return false;
	}
	expire = time( NULL ) + lp_parm_int(-1, "saf","ttl", SAF_TTL);

	DEBUG(10,("saf_store: domain = [%s], server = [%s], expire = [%u]\n",
		domain, servername, (unsigned int)expire ));

	ret = gencache_set( key, servername, expire );

	TALLOC_FREE( key );

	return ret;
}

bool saf_join_store( const char *domain, const char *servername )
{
	char *key;
	time_t expire;
	bool ret = False;

	if ( !domain || !servername ) {
		DEBUG(2,("saf_join_store: Refusing to store empty domain or servername!\n"));
		return False;
	}

	if ( (strlen(domain) == 0) || (strlen(servername) == 0) ) {
		DEBUG(0,("saf_join_store: refusing to store 0 length domain or servername!\n"));
		return False;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return false;
	}
	expire = time( NULL ) + lp_parm_int(-1, "saf","join ttl", SAFJOIN_TTL);

	DEBUG(10,("saf_join_store: domain = [%s], server = [%s], expire = [%u]\n",
		domain, servername, (unsigned int)expire ));

	ret = gencache_set( key, servername, expire );

	TALLOC_FREE( key );

	return ret;
}

bool saf_delete( const char *domain )
{
	char *key;
	bool ret = False;

	if ( !domain ) {
		DEBUG(2,("saf_delete: Refusing to delete empty domain\n"));
		return False;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return false;
	}
	ret = gencache_del(key);
	TALLOC_FREE(key);

	if (ret) {
		DEBUG(10,("saf_delete[join]: domain = [%s]\n", domain ));
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return false;
	}
	ret = gencache_del(key);
	TALLOC_FREE(key);

	if (ret) {
		DEBUG(10,("saf_delete: domain = [%s]\n", domain ));
	}

	return ret;
}

/****************************************************************************
****************************************************************************/

char *saf_fetch(TALLOC_CTX *mem_ctx, const char *domain )
{
	char *server = NULL;
	time_t timeout;
	bool ret = False;
	char *key = NULL;

	if ( !domain || strlen(domain) == 0) {
		DEBUG(2,("saf_fetch: Empty domain name!\n"));
		return NULL;
	}

	key = saf_join_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_join_key() failed\n"));
		return NULL;
	}

	ret = gencache_get( key, mem_ctx, &server, &timeout );

	TALLOC_FREE( key );

	if ( ret ) {
		DEBUG(5,("saf_fetch[join]: Returning \"%s\" for \"%s\" domain\n",
			server, domain ));
		return server;
	}

	key = saf_key(talloc_tos(), domain);
	if (key == NULL) {
		DEBUG(1, ("saf_key() failed\n"));
		return NULL;
	}

	ret = gencache_get( key, mem_ctx, &server, &timeout );

	TALLOC_FREE( key );

	if ( !ret ) {
		DEBUG(5,("saf_fetch: failed to find server for \"%s\" domain\n",
					domain ));
	} else {
		DEBUG(5,("saf_fetch: Returning \"%s\" for \"%s\" domain\n",
			server, domain ));
	}

	return server;
}

static void set_socket_addr_v4(struct samba_sockaddr *addr)
{
	if (!interpret_string_addr(&addr->u.ss, lp_nbt_client_socket_address(),
				   AI_NUMERICHOST|AI_PASSIVE)) {
		zero_sockaddr(&addr->u.ss);
		/* zero_sockaddr sets family to AF_INET. */
		addr->sa_socklen = sizeof(struct sockaddr_in);
	}
	if (addr->u.ss.ss_family != AF_INET) {
		zero_sockaddr(&addr->u.ss);
		/* zero_sockaddr sets family to AF_INET. */
		addr->sa_socklen = sizeof(struct sockaddr_in);
	}
}

static struct in_addr my_socket_addr_v4(void)
{
	struct samba_sockaddr my_addr = {0};

	set_socket_addr_v4(&my_addr);
	return my_addr.u.in.sin_addr;
}

/****************************************************************************
 Generate a random trn_id.
****************************************************************************/

static int generate_trn_id(void)
{
	uint16_t id;

	generate_random_buffer((uint8_t *)&id, sizeof(id));

	return id % (unsigned)0x7FFF;
}

/****************************************************************************
 Parse a node status response into an array of structures.
****************************************************************************/

static struct node_status *parse_node_status(TALLOC_CTX *mem_ctx, char *p,
				int *num_names,
				struct node_status_extra *extra)
{
	struct node_status *ret;
	int i;

	*num_names = CVAL(p,0);

	if (*num_names == 0)
		return NULL;

	ret = talloc_array(mem_ctx, struct node_status,*num_names);
	if (!ret)
		return NULL;

	p++;
	for (i=0;i< *num_names;i++) {
		strlcpy(ret[i].name,p,16);
		trim_char(ret[i].name,'\0',' ');
		ret[i].type = CVAL(p,15);
		ret[i].flags = p[16];
		p += 18;
		DEBUG(10, ("%s#%02x: flags = 0x%02x\n", ret[i].name,
			   ret[i].type, ret[i].flags));
	}
	/*
	 * Also, pick up the MAC address ...
	 */
	if (extra) {
		memcpy(&extra->mac_addr, p, 6); /* Fill in the mac addr */
	}
	return ret;
}

struct sock_packet_read_state {
	struct tevent_context *ev;
	enum packet_type type;
	int trn_id;

	struct nb_packet_reader *reader;
	struct tevent_req *reader_req;

	struct tdgram_context *sock;
	struct tevent_req *socket_req;
	uint8_t *buf;
	struct tsocket_address *addr;

	bool (*validator)(struct packet_struct *p,
			  void *private_data);
	void *private_data;

	struct packet_struct *packet;
};

static void sock_packet_read_got_packet(struct tevent_req *subreq);
static void sock_packet_read_got_socket(struct tevent_req *subreq);

static struct tevent_req *sock_packet_read_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	struct tdgram_context *sock,
	struct nb_packet_reader *reader,
	enum packet_type type,
	int trn_id,
	bool (*validator)(struct packet_struct *p, void *private_data),
	void *private_data)
{
	struct tevent_req *req;
	struct sock_packet_read_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct sock_packet_read_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->reader = reader;
	state->sock = sock;
	state->type = type;
	state->trn_id = trn_id;
	state->validator = validator;
	state->private_data = private_data;

	if (reader != NULL) {
		state->reader_req = nb_packet_read_send(state, ev, reader);
		if (tevent_req_nomem(state->reader_req, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(
			state->reader_req, sock_packet_read_got_packet, req);
	}

	state->socket_req = tdgram_recvfrom_send(state, ev, state->sock);
	if (tevent_req_nomem(state->socket_req, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(state->socket_req, sock_packet_read_got_socket,
				req);

	return req;
}

static void sock_packet_read_got_packet(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	NTSTATUS status;

	status = nb_packet_read_recv(subreq, state, &state->packet);

	TALLOC_FREE(state->reader_req);

	if (!NT_STATUS_IS_OK(status)) {
		if (state->socket_req != NULL) {
			/*
			 * Still waiting for socket
			 */
			return;
		}
		/*
		 * Both socket and packet reader failed
		 */
		tevent_req_nterror(req, status);
		return;
	}

	if ((state->validator != NULL) &&
	    !state->validator(state->packet, state->private_data)) {
		DEBUG(10, ("validator failed\n"));

		TALLOC_FREE(state->packet);

		state->reader_req = nb_packet_read_send(state, state->ev,
							state->reader);
		if (tevent_req_nomem(state->reader_req, req)) {
			return;
		}
		tevent_req_set_callback(
			state->reader_req, sock_packet_read_got_packet, req);
		return;
	}

	TALLOC_FREE(state->socket_req);
	tevent_req_done(req);
}

static void sock_packet_read_got_socket(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	struct samba_sockaddr addr = {0};
	ssize_t ret;
	ssize_t received;
	int err;
	bool ok;

	received = tdgram_recvfrom_recv(subreq, &err, state,
					&state->buf, &state->addr);

	TALLOC_FREE(state->socket_req);

	if (received == -1) {
		if (state->reader_req != NULL) {
			/*
			 * Still waiting for reader
			 */
			return;
		}
		/*
		 * Both socket and reader failed
		 */
		tevent_req_nterror(req, map_nt_error_from_unix(err));
		return;
	}
	ok = tsocket_address_is_inet(state->addr, "ipv4");
	if (!ok) {
		goto retry;
	}
	ret = tsocket_address_bsd_sockaddr(state->addr,
					&addr.u.sa,
					sizeof(addr.u.in));
	if (ret == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(errno));
		return;
	}

	state->packet = parse_packet_talloc(
		state, (char *)state->buf, received, state->type,
		addr.u.in.sin_addr, addr.u.in.sin_port);
	if (state->packet == NULL) {
		DEBUG(10, ("parse_packet failed\n"));
		goto retry;
	}
	if ((state->trn_id != -1) &&
	    (state->trn_id != packet_trn_id(state->packet))) {
		DEBUG(10, ("Expected transaction id %d, got %d\n",
			   state->trn_id, packet_trn_id(state->packet)));
		goto retry;
	}

	if ((state->validator != NULL) &&
	    !state->validator(state->packet, state->private_data)) {
		DEBUG(10, ("validator failed\n"));
		goto retry;
	}

	tevent_req_done(req);
	return;

retry:
	TALLOC_FREE(state->packet);
	TALLOC_FREE(state->buf);
	TALLOC_FREE(state->addr);

	state->socket_req = tdgram_recvfrom_send(state, state->ev, state->sock);
	if (tevent_req_nomem(state->socket_req, req)) {
		return;
	}
	tevent_req_set_callback(state->socket_req, sock_packet_read_got_socket,
				req);
}

static NTSTATUS sock_packet_read_recv(struct tevent_req *req,
				      TALLOC_CTX *mem_ctx,
				      struct packet_struct **ppacket)
{
	struct sock_packet_read_state *state = tevent_req_data(
		req, struct sock_packet_read_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*ppacket = talloc_move(mem_ctx, &state->packet);
	return NT_STATUS_OK;
}

struct nb_trans_state {
	struct tevent_context *ev;
	struct tdgram_context *sock;
	struct nb_packet_reader *reader;

	struct tsocket_address *src_addr;
	struct tsocket_address *dst_addr;
	uint8_t *buf;
	size_t buflen;
	enum packet_type type;
	int trn_id;

	bool (*validator)(struct packet_struct *p,
			  void *private_data);
	void *private_data;

	struct packet_struct *packet;
};

static void nb_trans_got_reader(struct tevent_req *subreq);
static void nb_trans_done(struct tevent_req *subreq);
static void nb_trans_sent(struct tevent_req *subreq);
static void nb_trans_send_next(struct tevent_req *subreq);

static struct tevent_req *nb_trans_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	const struct samba_sockaddr *_my_addr,
	const struct samba_sockaddr *_dst_addr,
	bool bcast,
	uint8_t *buf, size_t buflen,
	enum packet_type type, int trn_id,
	bool (*validator)(struct packet_struct *p,
			  void *private_data),
	void *private_data)
{
	const struct sockaddr *my_addr = &_my_addr->u.sa;
	size_t my_addr_len = sizeof(_my_addr->u.in); /*We know it's AF_INET.*/
	const struct sockaddr *dst_addr = &_dst_addr->u.sa;
	size_t dst_addr_len = sizeof(_dst_addr->u.in); /*We know it's AF_INET.*/
	struct tevent_req *req, *subreq;
	struct nb_trans_state *state;
	int ret;

	req = tevent_req_create(mem_ctx, &state, struct nb_trans_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->buf = buf;
	state->buflen = buflen;
	state->type = type;
	state->trn_id = trn_id;
	state->validator = validator;
	state->private_data = private_data;

	ret = tsocket_address_bsd_from_sockaddr(state,
						my_addr, my_addr_len,
						&state->src_addr);
	if (ret == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(errno));
		return tevent_req_post(req, ev);
	}

	ret = tsocket_address_bsd_from_sockaddr(state,
						dst_addr, dst_addr_len,
						&state->dst_addr);
	if (ret == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(errno));
		return tevent_req_post(req, ev);
	}

	ret = tdgram_inet_udp_broadcast_socket(state->src_addr, state,
					       &state->sock);
	if (ret == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(errno));
		return tevent_req_post(req, ev);
	}

	subreq = nb_packet_reader_send(state, ev, type, state->trn_id, NULL);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, nb_trans_got_reader, req);
	return req;
}

static void nb_trans_got_reader(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	status = nb_packet_reader_recv(subreq, state, &state->reader);
	TALLOC_FREE(subreq);

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(10, ("nmbd not around\n"));
		state->reader = NULL;
	}

	subreq = sock_packet_read_send(
		state, state->ev, state->sock,
		state->reader, state->type, state->trn_id,
		state->validator, state->private_data);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_done, req);

	subreq = tdgram_sendto_send(state, state->ev,
				    state->sock,
				    state->buf, state->buflen,
				    state->dst_addr);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_sent, req);
}

static void nb_trans_sent(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	ssize_t sent;
	int err;

	sent = tdgram_sendto_recv(subreq, &err);
	TALLOC_FREE(subreq);
	if (sent == -1) {
		DEBUG(10, ("sendto failed: %s\n", strerror(err)));
		tevent_req_nterror(req, map_nt_error_from_unix(err));
		return;
	}
	subreq = tevent_wakeup_send(state, state->ev,
				    timeval_current_ofs(1, 0));
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_send_next, req);
}

static void nb_trans_send_next(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	bool ret;

	ret = tevent_wakeup_recv(subreq);
	TALLOC_FREE(subreq);
	if (!ret) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}
	subreq = tdgram_sendto_send(state, state->ev,
				    state->sock,
				    state->buf, state->buflen,
				    state->dst_addr);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, nb_trans_sent, req);
}

static void nb_trans_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	status = sock_packet_read_recv(subreq, state, &state->packet);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

static NTSTATUS nb_trans_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			      struct packet_struct **ppacket)
{
	struct nb_trans_state *state = tevent_req_data(
		req, struct nb_trans_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*ppacket = talloc_move(mem_ctx, &state->packet);
	return NT_STATUS_OK;
}

/****************************************************************************
 Do a NBT node status query on an open socket and return an array of
 structures holding the returned names or NULL if the query failed.
**************************************************************************/

struct node_status_query_state {
	struct samba_sockaddr my_addr;
	struct samba_sockaddr addr;
	uint8_t buf[1024];
	ssize_t buflen;
	struct packet_struct *packet;
};

static bool node_status_query_validator(struct packet_struct *p,
					void *private_data);
static void node_status_query_done(struct tevent_req *subreq);

struct tevent_req *node_status_query_send(TALLOC_CTX *mem_ctx,
					  struct tevent_context *ev,
					  struct nmb_name *name,
					  const struct sockaddr_storage *addr)
{
	struct tevent_req *req, *subreq;
	struct node_status_query_state *state;
	struct packet_struct p;
	struct nmb_packet *nmb = &p.packet.nmb;
	bool ok;

	req = tevent_req_create(mem_ctx, &state,
				struct node_status_query_state);
	if (req == NULL) {
		return NULL;
	}

	if (addr->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}

	ok = sockaddr_storage_to_samba_sockaddr(&state->addr, addr);
	if (!ok) {
		/* node status must be IPv4 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}
	state->addr.u.in.sin_port = htons(NMB_PORT);

	set_socket_addr_v4(&state->my_addr);

	ZERO_STRUCT(p);
	nmb->header.name_trn_id = generate_trn_id();
	nmb->header.opcode = 0;
	nmb->header.response = false;
	nmb->header.nm_flags.bcast = false;
	nmb->header.nm_flags.recursion_available = false;
	nmb->header.nm_flags.recursion_desired = false;
	nmb->header.nm_flags.trunc = false;
	nmb->header.nm_flags.authoritative = false;
	nmb->header.rcode = 0;
	nmb->header.qdcount = 1;
	nmb->header.ancount = 0;
	nmb->header.nscount = 0;
	nmb->header.arcount = 0;
	nmb->question.question_name = *name;
	nmb->question.question_type = 0x21;
	nmb->question.question_class = 0x1;

	state->buflen = build_packet((char *)state->buf, sizeof(state->buf),
				     &p);
	if (state->buflen == 0) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		DEBUG(10, ("build_packet failed\n"));
		return tevent_req_post(req, ev);
	}

	subreq = nb_trans_send(state,
				ev,
				&state->my_addr,
				&state->addr,
				false,
				state->buf,
				state->buflen,
				NMB_PACKET,
				nmb->header.name_trn_id,
				node_status_query_validator,
				NULL);
	if (tevent_req_nomem(subreq, req)) {
		DEBUG(10, ("nb_trans_send failed\n"));
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(req, ev, timeval_current_ofs(10, 0))) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, node_status_query_done, req);
	return req;
}

static bool node_status_query_validator(struct packet_struct *p,
					void *private_data)
{
	struct nmb_packet *nmb = &p->packet.nmb;
	debug_nmb_packet(p);

	if (nmb->header.opcode != 0 ||
	    nmb->header.nm_flags.bcast ||
	    nmb->header.rcode ||
	    !nmb->header.ancount ||
	    nmb->answers->rr_type != 0x21) {
		/*
		 * XXXX what do we do with this? could be a redirect,
		 * but we'll discard it for the moment
		 */
		return false;
	}
	return true;
}

static void node_status_query_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct node_status_query_state *state = tevent_req_data(
		req, struct node_status_query_state);
	NTSTATUS status;

	status = nb_trans_recv(subreq, state, &state->packet);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS node_status_query_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				struct node_status **pnode_status,
				int *pnum_names,
				struct node_status_extra *extra)
{
	struct node_status_query_state *state = tevent_req_data(
		req, struct node_status_query_state);
	struct node_status *node_status;
	int num_names;
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	node_status = parse_node_status(
		mem_ctx, &state->packet->packet.nmb.answers->rdata[0],
		&num_names, extra);
	if (node_status == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	*pnode_status = node_status;
	*pnum_names = num_names;
	return NT_STATUS_OK;
}

NTSTATUS node_status_query(TALLOC_CTX *mem_ctx, struct nmb_name *name,
			   const struct sockaddr_storage *addr,
			   struct node_status **pnode_status,
			   int *pnum_names,
			   struct node_status_extra *extra)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = node_status_query_send(ev, ev, name, addr);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = node_status_query_recv(req, mem_ctx, pnode_status,
					pnum_names, extra);
 fail:
	TALLOC_FREE(frame);
	return status;
}

static bool name_status_lmhosts(const struct sockaddr_storage *paddr,
				int qname_type, fstring pname)
{
	FILE *f;
	char *name;
	int name_type;
	struct samba_sockaddr addr_in = {0};
	struct samba_sockaddr addr = {0};
	bool ok;

	ok = sockaddr_storage_to_samba_sockaddr(&addr_in, paddr);
	if (!ok) {
		return false;
	}
	if (addr_in.u.ss.ss_family != AF_INET) {
		return false;
	}

	f = startlmhosts(get_dyn_LMHOSTSFILE());
	if (f == NULL) {
		return false;
	}

	while (getlmhostsent(talloc_tos(), f, &name, &name_type, &addr.u.ss)) {
		if (addr.u.ss.ss_family != AF_INET) {
			continue;
		}
		if (name_type != qname_type) {
			continue;
		}
		if (sockaddr_equal(&addr_in.u.sa, &addr.u.sa)) {
			fstrcpy(pname, name);
			endlmhosts(f);
			return true;
		}
	}
	endlmhosts(f);
	return false;
}

/****************************************************************************
 Find the first type XX name in a node status reply - used for finding
 a servers name given its IP. Return the matched name in *name.
**************************************************************************/

bool name_status_find(const char *q_name,
			int q_type,
			int type,
			const struct sockaddr_storage *to_ss,
			fstring name)
{
	char addr[INET6_ADDRSTRLEN];
	struct node_status *addrs = NULL;
	struct nmb_name nname;
	int count = 0, i;
	bool result = false;
	NTSTATUS status;

	if (lp_disable_netbios()) {
		DEBUG(5,("name_status_find(%s#%02x): netbios is disabled\n",
					q_name, q_type));
		return False;
	}

	print_sockaddr(addr, sizeof(addr), to_ss);

	DEBUG(10, ("name_status_find: looking up %s#%02x at %s\n", q_name,
		   q_type, addr));

	/* Check the cache first. */

	if (namecache_status_fetch(q_name, q_type, type, to_ss, name)) {
		return True;
	}

	if (to_ss->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		return false;
	}

	result = name_status_lmhosts(to_ss, type, name);
	if (result) {
		DBG_DEBUG("Found name %s in lmhosts\n", name);
		namecache_status_store(q_name, q_type, type, to_ss, name);
		return true;
	}

	/* W2K PDC's seem not to respond to '*'#0. JRA */
	make_nmb_name(&nname, q_name, q_type);
	status = node_status_query(talloc_tos(), &nname, to_ss,
				   &addrs, &count, NULL);
	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	for (i=0;i<count;i++) {
                /* Find first one of the requested type that's not a GROUP. */
		if (addrs[i].type == type && ! (addrs[i].flags & 0x80))
			break;
	}
	if (i == count)
		goto done;

	pull_ascii_nstring(name, sizeof(fstring), addrs[i].name);

	/* Store the result in the cache. */
	/* but don't store an entry for 0x1c names here.  Here we have
	   a single host and DOMAIN<0x1c> names should be a list of hosts */

	if ( q_type != 0x1c ) {
		namecache_status_store(q_name, q_type, type, to_ss, name);
	}

	result = true;

 done:
	TALLOC_FREE(addrs);

	DEBUG(10, ("name_status_find: name %sfound", result ? "" : "not "));

	if (result)
		DEBUGADD(10, (", name %s ip address is %s", name, addr));

	DEBUG(10, ("\n"));

	return result;
}

/*
  comparison function used by sort_addr_list
*/

static int addr_compare(const struct sockaddr_storage *ss1,
			const struct sockaddr_storage *ss2)
{
	int max_bits1=0, max_bits2=0;
	int num_interfaces = iface_count();
	int i;
	struct samba_sockaddr sa1;
	struct samba_sockaddr sa2;
	bool ok;

	ok = sockaddr_storage_to_samba_sockaddr(&sa1, ss1);
	if (!ok) {
		return 0; /* No change. */
	}

	ok = sockaddr_storage_to_samba_sockaddr(&sa2, ss2);
	if (!ok) {
		return 0; /* No change. */
	}

	/* Sort IPv4 addresses first. */
	if (sa1.u.ss.ss_family != sa2.u.ss.ss_family) {
		if (sa2.u.ss.ss_family == AF_INET) {
			return 1;
		} else {
			return -1;
		}
	}

	/* Here we know both addresses are of the same
	 * family. */

	for (i=0;i<num_interfaces;i++) {
		struct samba_sockaddr sif = {0};
		const unsigned char *p_ss1 = NULL;
		const unsigned char *p_ss2 = NULL;
		const unsigned char *p_if = NULL;
		size_t len = 0;
		int bits1, bits2;

		ok = sockaddr_storage_to_samba_sockaddr(&sif, iface_n_bcast(i));
		if (!ok) {
			return 0; /* No change. */
		}
		if (sif.u.ss.ss_family != sa1.u.ss.ss_family) {
			/* Ignore interfaces of the wrong type. */
			continue;
		}
		if (sif.u.ss.ss_family == AF_INET) {
			p_if = (const unsigned char *)&sif.u.in.sin_addr;
			p_ss1 = (const unsigned char *)&sa1.u.in.sin_addr;
			p_ss2 = (const unsigned char *)&sa2.u.in.sin_addr;
			len = 4;
		}
#if defined(HAVE_IPV6)
		if (sif.u.ss.ss_family == AF_INET6) {
			p_if = (const unsigned char *)&sif.u.in6.sin6_addr;
			p_ss1 = (const unsigned char *)&sa1.u.in6.sin6_addr;
			p_ss2 = (const unsigned char *)&sa2.u.in6.sin6_addr;
			len = 16;
		}
#endif
		if (!p_ss1 || !p_ss2 || !p_if || len == 0) {
			continue;
		}
		bits1 = matching_len_bits(p_ss1, p_if, len);
		bits2 = matching_len_bits(p_ss2, p_if, len);
		max_bits1 = MAX(bits1, max_bits1);
		max_bits2 = MAX(bits2, max_bits2);
	}

	/* Bias towards directly reachable IPs */
	if (iface_local(&sa1.u.sa)) {
		if (sa1.u.ss.ss_family == AF_INET) {
			max_bits1 += 32;
		} else {
			max_bits1 += 128;
		}
	}
	if (iface_local(&sa2.u.sa)) {
		if (sa2.u.ss.ss_family == AF_INET) {
			max_bits2 += 32;
		} else {
			max_bits2 += 128;
		}
	}
	return max_bits2 - max_bits1;
}

/*******************************************************************
 compare 2 ldap IPs by nearness to our interfaces - used in qsort
*******************************************************************/

static int ip_service_compare(struct ip_service *ss1, struct ip_service *ss2)
{
	int result;

	if ((result = addr_compare(&ss1->ss, &ss2->ss)) != 0) {
		return result;
	}

	if (ss1->port > ss2->port) {
		return 1;
	}

	if (ss1->port < ss2->port) {
		return -1;
	}

	return 0;
}

/*
  sort an IP list so that names that are close to one of our interfaces
  are at the top. This prevents the problem where a WINS server returns an IP
  that is not reachable from our subnet as the first match
*/

static void sort_addr_list(struct sockaddr_storage *sslist, int count)
{
	if (count <= 1) {
		return;
	}

	TYPESAFE_QSORT(sslist, count, addr_compare);
}

static void sort_service_list(struct ip_service *servlist, int count)
{
	if (count <= 1) {
		return;
	}

	TYPESAFE_QSORT(servlist, count, ip_service_compare);
}

/**********************************************************************
 Remove any duplicate address/port pairs in the list
 *********************************************************************/

size_t remove_duplicate_addrs2(struct ip_service *iplist, size_t count )
{
	size_t i, j;

	DEBUG(10,("remove_duplicate_addrs2: "
			"looking for duplicate address/port pairs\n"));

	/* One loop to set duplicates to a zero addr. */
	for ( i=0; i<count; i++ ) {
		bool ok;
		struct samba_sockaddr sa_i = {0};

		ok = sockaddr_storage_to_samba_sockaddr(&sa_i, &iplist[i].ss);
		if (!ok) {
			continue;
		}

		if (is_zero_addr(&sa_i.u.ss)) {
			continue;
		}

		for ( j=i+1; j<count; j++ ) {
			struct samba_sockaddr sa_j = {0};

			ok = sockaddr_storage_to_samba_sockaddr(&sa_j,
							&iplist[j].ss);
			if (!ok) {
				continue;
			}

			if (sockaddr_equal(&sa_i.u.sa, &sa_j.u.sa) &&
					iplist[i].port == iplist[j].port) {
				zero_sockaddr(&iplist[j].ss);
			}
		}
	}

	/* Now remove any addresses set to zero above. */
	for (i = 0; i < count; i++) {
		while (i < count &&
				is_zero_addr(&iplist[i].ss)) {
			ARRAY_DEL_ELEMENT(iplist, i, count);
			count--;
		}
	}

	return count;
}

static bool prioritize_ipv4_list(struct ip_service *iplist, int count)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct ip_service *iplist_new = talloc_array(frame, struct ip_service, count);
	int i, j;

	if (iplist_new == NULL) {
		TALLOC_FREE(frame);
		return false;
	}

	j = 0;

	/* Copy IPv4 first. */
	for (i = 0; i < count; i++) {
		if (iplist[i].ss.ss_family == AF_INET) {
			iplist_new[j++] = iplist[i];
		}
	}

	/* Copy IPv6. */
	for (i = 0; i < count; i++) {
		if (iplist[i].ss.ss_family != AF_INET) {
			iplist_new[j++] = iplist[i];
		}
	}

	memcpy(iplist, iplist_new, sizeof(struct ip_service)*count);
	TALLOC_FREE(frame);
	return true;
}

/****************************************************************************
 Do a netbios name query to find someones IP.
 Returns an array of IP addresses or NULL if none.
 *count will be set to the number of addresses returned.
 *timed_out is set if we failed by timing out
****************************************************************************/

struct name_query_state {
	struct samba_sockaddr my_addr;
	struct samba_sockaddr addr;
	bool bcast;
	bool bcast_star_query;


	uint8_t buf[1024];
	ssize_t buflen;

	NTSTATUS validate_error;
	uint8_t flags;

	struct sockaddr_storage *addrs;
	int num_addrs;
};

static bool name_query_validator(struct packet_struct *p, void *private_data);
static void name_query_done(struct tevent_req *subreq);

struct tevent_req *name_query_send(TALLOC_CTX *mem_ctx,
				   struct tevent_context *ev,
				   const char *name, int name_type,
				   bool bcast, bool recurse,
				   const struct sockaddr_storage *addr)
{
	struct tevent_req *req, *subreq;
	struct name_query_state *state;
	struct packet_struct p;
	struct nmb_packet *nmb = &p.packet.nmb;
	bool ok;

	req = tevent_req_create(mem_ctx, &state, struct name_query_state);
	if (req == NULL) {
		return NULL;
	}
	state->bcast = bcast;

	if (addr->ss_family != AF_INET) {
		/* Can't do node status to IPv6 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}

	if (lp_disable_netbios()) {
		DEBUG(5,("name_query(%s#%02x): netbios is disabled\n",
					name, name_type));
		tevent_req_nterror(req, NT_STATUS_NOT_SUPPORTED);
		return tevent_req_post(req, ev);
	}

	ok = sockaddr_storage_to_samba_sockaddr(&state->addr, addr);
	if (!ok) {
		/* Node status must be IPv4 */
		tevent_req_nterror(req, NT_STATUS_INVALID_ADDRESS);
		return tevent_req_post(req, ev);
	}
	state->addr.u.in.sin_port = htons(NMB_PORT);

	set_socket_addr_v4(&state->my_addr);

	ZERO_STRUCT(p);
	nmb->header.name_trn_id = generate_trn_id();
	nmb->header.opcode = 0;
	nmb->header.response = false;
	nmb->header.nm_flags.bcast = bcast;
	nmb->header.nm_flags.recursion_available = false;
	nmb->header.nm_flags.recursion_desired = recurse;
	nmb->header.nm_flags.trunc = false;
	nmb->header.nm_flags.authoritative = false;
	nmb->header.rcode = 0;
	nmb->header.qdcount = 1;
	nmb->header.ancount = 0;
	nmb->header.nscount = 0;
	nmb->header.arcount = 0;

	if (bcast && (strcmp(name, "*")==0)) {
		/*
		 * We're doing a broadcast query for all
		 * names in the area. Remember this so
		 * we will wait for all names within
		 * the timeout period.
		 */
		state->bcast_star_query = true;
	}

	make_nmb_name(&nmb->question.question_name,name,name_type);

	nmb->question.question_type = 0x20;
	nmb->question.question_class = 0x1;

	state->buflen = build_packet((char *)state->buf, sizeof(state->buf),
				     &p);
	if (state->buflen == 0) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		DEBUG(10, ("build_packet failed\n"));
		return tevent_req_post(req, ev);
	}

	subreq = nb_trans_send(state,
				ev,
				&state->my_addr,
				&state->addr,
				bcast,
				state->buf,
				state->buflen,
				NMB_PACKET,
				nmb->header.name_trn_id,
				name_query_validator,
				state);
	if (tevent_req_nomem(subreq, req)) {
		DEBUG(10, ("nb_trans_send failed\n"));
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_query_done, req);
	return req;
}

static bool name_query_validator(struct packet_struct *p, void *private_data)
{
	struct name_query_state *state = talloc_get_type_abort(
		private_data, struct name_query_state);
	struct nmb_packet *nmb = &p->packet.nmb;
	struct sockaddr_storage *tmp_addrs;
	bool got_unique_netbios_name = false;
	int i;

	debug_nmb_packet(p);

	/*
	 * If we get a Negative Name Query Response from a WINS
	 * server, we should report it and give up.
	 */
	if( 0 == nmb->header.opcode	/* A query response   */
	    && !state->bcast		/* from a WINS server */
	    && nmb->header.rcode	/* Error returned     */
		) {

		if( DEBUGLVL( 3 ) ) {
			/* Only executed if DEBUGLEVEL >= 3 */
			dbgtext( "Negative name query "
				 "response, rcode 0x%02x: ",
				 nmb->header.rcode );
			switch( nmb->header.rcode ) {
			case 0x01:
				dbgtext("Request was invalidly formatted.\n");
				break;
			case 0x02:
				dbgtext("Problem with NBNS, cannot process "
					"name.\n");
				break;
			case 0x03:
				dbgtext("The name requested does not "
					"exist.\n");
				break;
			case 0x04:
				dbgtext("Unsupported request error.\n");
				break;
			case 0x05:
				dbgtext("Query refused error.\n");
				break;
			default:
				dbgtext("Unrecognized error code.\n" );
				break;
			}
		}

		/*
		 * We accept this packet as valid, but tell the upper
		 * layers that it's a negative response.
		 */
		state->validate_error = NT_STATUS_NOT_FOUND;
		return true;
	}

	if (nmb->header.opcode != 0 ||
	    nmb->header.nm_flags.bcast ||
	    nmb->header.rcode ||
	    !nmb->header.ancount) {
		/*
		 * XXXX what do we do with this? Could be a redirect,
		 * but we'll discard it for the moment.
		 */
		return false;
	}

	tmp_addrs = talloc_realloc(
		state, state->addrs, struct sockaddr_storage,
		state->num_addrs + nmb->answers->rdlength/6);
	if (tmp_addrs == NULL) {
		state->validate_error = NT_STATUS_NO_MEMORY;
		return true;
	}
	state->addrs = tmp_addrs;

	DEBUG(2,("Got a positive name query response "
		 "from %s ( ", inet_ntoa(p->ip)));

	for (i=0; i<nmb->answers->rdlength/6; i++) {
		uint16_t flags;
		struct in_addr ip;
		struct sockaddr_storage addr;
		struct samba_sockaddr sa = {0};
		bool ok;
		int j;

		flags = RSVAL(&nmb->answers->rdata[i*6], 0);
		got_unique_netbios_name |= ((flags & 0x8000) == 0);

		putip((char *)&ip,&nmb->answers->rdata[2+i*6]);
		in_addr_to_sockaddr_storage(&addr, ip);

		ok = sockaddr_storage_to_samba_sockaddr(&sa, &addr);
		if (!ok) {
			continue;
		}

		if (is_zero_addr(&sa.u.ss)) {
			continue;
		}

		for (j=0; j<state->num_addrs; j++) {
			struct samba_sockaddr sa_j = {0};

			ok = sockaddr_storage_to_samba_sockaddr(&sa_j,
						&state->addrs[j]);
			if (!ok) {
				continue;
			}
			if (sockaddr_equal(&sa.u.sa, &sa_j.u.sa)) {
				break;
			}
		}
		if (j < state->num_addrs) {
			/* Already got it */
			continue;
		}

		DEBUGADD(2,("%s ",inet_ntoa(ip)));

		state->addrs[state->num_addrs] = addr;
		state->num_addrs += 1;
	}
	DEBUGADD(2,(")\n"));

	/* We add the flags back ... */
	if (nmb->header.response)
		state->flags |= NM_FLAGS_RS;
	if (nmb->header.nm_flags.authoritative)
		state->flags |= NM_FLAGS_AA;
	if (nmb->header.nm_flags.trunc)
		state->flags |= NM_FLAGS_TC;
	if (nmb->header.nm_flags.recursion_desired)
		state->flags |= NM_FLAGS_RD;
	if (nmb->header.nm_flags.recursion_available)
		state->flags |= NM_FLAGS_RA;
	if (nmb->header.nm_flags.bcast)
		state->flags |= NM_FLAGS_B;

	if (state->bcast) {
		/*
		 * We have to collect all entries coming in from broadcast
		 * queries. If we got a unique name and we are not querying
		 * all names registered within broadcast area (query
		 * for the name '*', so state->bcast_star_query is set),
		 * we're done.
		 */
		return (got_unique_netbios_name && !state->bcast_star_query);
	}
	/*
	 * WINS responses are accepted when they are received
	 */
	return true;
}

static void name_query_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_query_state *state = tevent_req_data(
		req, struct name_query_state);
	NTSTATUS status;
	struct packet_struct *p = NULL;

	status = nb_trans_recv(subreq, state, &p);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	if (!NT_STATUS_IS_OK(state->validate_error)) {
		tevent_req_nterror(req, state->validate_error);
		return;
	}
	tevent_req_done(req);
}

NTSTATUS name_query_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			 struct sockaddr_storage **addrs, int *num_addrs,
			 uint8_t *flags)
{
	struct name_query_state *state = tevent_req_data(
		req, struct name_query_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		if (state->bcast &&
		    NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
			/*
			 * In the broadcast case we collect replies until the
			 * timeout.
			 */
			status = NT_STATUS_OK;
		}
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}
	if (state->num_addrs == 0) {
		return NT_STATUS_NOT_FOUND;
	}
	*addrs = talloc_move(mem_ctx, &state->addrs);
	sort_addr_list(*addrs, state->num_addrs);
	*num_addrs = state->num_addrs;
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

NTSTATUS name_query(const char *name, int name_type,
		    bool bcast, bool recurse,
		    const struct sockaddr_storage *to_ss,
		    TALLOC_CTX *mem_ctx,
		    struct sockaddr_storage **addrs,
		    int *num_addrs, uint8_t *flags)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct tevent_context *ev;
	struct tevent_req *req;
	struct timeval timeout;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = name_query_send(ev, ev, name, name_type, bcast, recurse, to_ss);
	if (req == NULL) {
		goto fail;
	}
	if (bcast) {
		timeout = timeval_current_ofs(0, 250000);
	} else {
		timeout = timeval_current_ofs(2, 0);
	}
	if (!tevent_req_set_endtime(req, ev, timeout)) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = name_query_recv(req, mem_ctx, addrs, num_addrs, flags);
 fail:
	TALLOC_FREE(frame);
	return status;
}

/********************************************************
 Convert an array if struct sockaddr_storage to struct ip_service
 return false on failure.  Port is set to PORT_NONE;
 orig_count is the length of ss_list on input,
 *count_out is the length of return_iplist on output as we remove any
 zero addresses from ss_list.
*********************************************************/

static bool convert_ss2service(TALLOC_CTX *ctx,
		struct ip_service **return_iplist,
		const struct sockaddr_storage *ss_list,
		size_t orig_count,
		size_t *count_out)
{
	size_t i;
	size_t real_count = 0;
	struct ip_service *iplist = NULL;

	if (orig_count == 0 || ss_list == NULL) {
		return false;
	}

	/* Filter out zero addrs. */
	for (i = 0; i < orig_count; i++ ) {
		if (is_zero_addr(&ss_list[i])) {
			continue;
		}
		real_count++;
	}
	if (real_count == 0) {
		return false;
	}

	/* copy the ip address; port will be PORT_NONE */
	iplist = talloc_zero_array(ctx, struct ip_service, real_count);
	if (iplist == NULL) {
		DBG_ERR("talloc failed for %zu enetries!\n", real_count);
		return false;
	}

	real_count = 0;
	for (i=0; i < orig_count; i++ ) {
		if (is_zero_addr(&ss_list[i])) {
			continue;
		}
		iplist[real_count].ss   = ss_list[i];
		iplist[real_count].port = PORT_NONE;
		real_count++;
	}

	*return_iplist = iplist;
	*count_out = real_count;
	return true;
}

struct name_queries_state {
	struct tevent_context *ev;
	const char *name;
	int name_type;
	bool bcast;
	bool recurse;
	const struct sockaddr_storage *addrs;
	int num_addrs;
	int wait_msec;
	int timeout_msec;

	struct tevent_req **subreqs;
	int num_received;
	int num_sent;

	int received_index;
	struct sockaddr_storage *result_addrs;
	int num_result_addrs;
	uint8_t flags;
};

static void name_queries_done(struct tevent_req *subreq);
static void name_queries_next(struct tevent_req *subreq);

/*
 * Send a name query to multiple destinations with a wait time in between
 */

static struct tevent_req *name_queries_send(
	TALLOC_CTX *mem_ctx, struct tevent_context *ev,
	const char *name, int name_type,
	bool bcast, bool recurse,
	const struct sockaddr_storage *addrs,
	int num_addrs, int wait_msec, int timeout_msec)
{
	struct tevent_req *req, *subreq;
	struct name_queries_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct name_queries_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->name = name;
	state->name_type = name_type;
	state->bcast = bcast;
	state->recurse = recurse;
	state->addrs = addrs;
	state->num_addrs = num_addrs;
	state->wait_msec = wait_msec;
	state->timeout_msec = timeout_msec;

	state->subreqs = talloc_zero_array(
		state, struct tevent_req *, num_addrs);
	if (tevent_req_nomem(state->subreqs, req)) {
		return tevent_req_post(req, ev);
	}
	state->num_sent = 0;

	subreq = name_query_send(
		state->subreqs, state->ev, name, name_type, bcast, recurse,
		&state->addrs[state->num_sent]);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(
		    subreq, state->ev,
		    timeval_current_ofs(0, state->timeout_msec * 1000))) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_queries_done, req);

	state->subreqs[state->num_sent] = subreq;
	state->num_sent += 1;

	if (state->num_sent < state->num_addrs) {
		subreq = tevent_wakeup_send(
			state, state->ev,
			timeval_current_ofs(0, state->wait_msec * 1000));
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, name_queries_next, req);
	}
	return req;
}

static void name_queries_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);
	int i;
	NTSTATUS status;

	status = name_query_recv(subreq, state, &state->result_addrs,
				 &state->num_result_addrs, &state->flags);

	for (i=0; i<state->num_sent; i++) {
		if (state->subreqs[i] == subreq) {
			break;
		}
	}
	if (i == state->num_sent) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}
	TALLOC_FREE(state->subreqs[i]);

	state->num_received += 1;

	if (!NT_STATUS_IS_OK(status)) {

		if (state->num_received >= state->num_addrs) {
			tevent_req_nterror(req, status);
			return;
		}
		/*
		 * Still outstanding requests, just wait
		 */
		return;
	}
	state->received_index = i;
	tevent_req_done(req);
}

static void name_queries_next(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);

	if (!tevent_wakeup_recv(subreq)) {
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}

	subreq = name_query_send(
		state->subreqs, state->ev,
		state->name, state->name_type, state->bcast, state->recurse,
		&state->addrs[state->num_sent]);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, name_queries_done, req);
	if (!tevent_req_set_endtime(
		    subreq, state->ev,
		    timeval_current_ofs(0, state->timeout_msec * 1000))) {
		return;
	}
	state->subreqs[state->num_sent] = subreq;
	state->num_sent += 1;

	if (state->num_sent < state->num_addrs) {
		subreq = tevent_wakeup_send(
			state, state->ev,
			timeval_current_ofs(0, state->wait_msec * 1000));
		if (tevent_req_nomem(subreq, req)) {
			return;
		}
		tevent_req_set_callback(subreq, name_queries_next, req);
	}
}

static NTSTATUS name_queries_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				  struct sockaddr_storage **result_addrs,
				  int *num_result_addrs, uint8_t *flags,
				  int *received_index)
{
	struct name_queries_state *state = tevent_req_data(
		req, struct name_queries_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}

	if (result_addrs != NULL) {
		*result_addrs = talloc_move(mem_ctx, &state->result_addrs);
	}
	if (num_result_addrs != NULL) {
		*num_result_addrs = state->num_result_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	if (received_index != NULL) {
		*received_index = state->received_index;
	}
	return NT_STATUS_OK;
}

/********************************************************
 Resolve via "bcast" method.
*********************************************************/

struct name_resolve_bcast_state {
	struct sockaddr_storage *addrs;
	int num_addrs;
};

static void name_resolve_bcast_done(struct tevent_req *subreq);

struct tevent_req *name_resolve_bcast_send(TALLOC_CTX *mem_ctx,
					   struct tevent_context *ev,
					   const char *name,
					   int name_type)
{
	struct tevent_req *req, *subreq;
	struct name_resolve_bcast_state *state;
	struct sockaddr_storage *bcast_addrs;
	int i, num_addrs, num_bcast_addrs;

	req = tevent_req_create(mem_ctx, &state,
				struct name_resolve_bcast_state);
	if (req == NULL) {
		return NULL;
	}

	if (lp_disable_netbios()) {
		DEBUG(5, ("name_resolve_bcast(%s#%02x): netbios is disabled\n",
			  name, name_type));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}

	/*
	 * "bcast" means do a broadcast lookup on all the local interfaces.
	 */

	DEBUG(3, ("name_resolve_bcast: Attempting broadcast lookup "
		  "for name %s<0x%x>\n", name, name_type));

	num_addrs = iface_count();
	bcast_addrs = talloc_array(state, struct sockaddr_storage, num_addrs);
	if (tevent_req_nomem(bcast_addrs, req)) {
		return tevent_req_post(req, ev);
	}

	/*
	 * Lookup the name on all the interfaces, return on
	 * the first successful match.
	 */
	num_bcast_addrs = 0;

	for (i=0; i<num_addrs; i++) {
		const struct sockaddr_storage *pss = iface_n_bcast(i);

		if (pss->ss_family != AF_INET) {
			continue;
		}
		bcast_addrs[num_bcast_addrs] = *pss;
		num_bcast_addrs += 1;
	}

	subreq = name_queries_send(state, ev, name, name_type, true, true,
				   bcast_addrs, num_bcast_addrs, 0, 250);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, name_resolve_bcast_done, req);
	return req;
}

static void name_resolve_bcast_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct name_resolve_bcast_state *state = tevent_req_data(
		req, struct name_resolve_bcast_state);
	NTSTATUS status;

	status = name_queries_recv(subreq, state,
				   &state->addrs, &state->num_addrs,
				   NULL, NULL);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS name_resolve_bcast_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
				 struct sockaddr_storage **addrs,
				 int *num_addrs)
{
	struct name_resolve_bcast_state *state = tevent_req_data(
		req, struct name_resolve_bcast_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	*addrs = talloc_move(mem_ctx, &state->addrs);
	*num_addrs = state->num_addrs;
	return NT_STATUS_OK;
}

NTSTATUS name_resolve_bcast(TALLOC_CTX *mem_ctx,
			const char *name,
			int name_type,
			struct sockaddr_storage **return_iplist,
			int *return_count)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = name_resolve_bcast_send(frame, ev, name, name_type);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = name_resolve_bcast_recv(req, mem_ctx, return_iplist,
					 return_count);
 fail:
	TALLOC_FREE(frame);
	return status;
}

struct query_wins_list_state {
	struct tevent_context *ev;
	const char *name;
	uint8_t name_type;
	struct in_addr *servers;
	uint32_t num_servers;
	struct sockaddr_storage server;
	uint32_t num_sent;

	struct sockaddr_storage *addrs;
	int num_addrs;
	uint8_t flags;
};

static void query_wins_list_done(struct tevent_req *subreq);

/*
 * Query a list of (replicating) wins servers in sequence, call them
 * dead if they don't reply
 */

static struct tevent_req *query_wins_list_send(
	TALLOC_CTX *mem_ctx, struct tevent_context *ev,
	struct in_addr src_ip, const char *name, uint8_t name_type,
	struct in_addr *servers, int num_servers)
{
	struct tevent_req *req, *subreq;
	struct query_wins_list_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct query_wins_list_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->name = name;
	state->name_type = name_type;
	state->servers = servers;
	state->num_servers = num_servers;

	if (state->num_servers == 0) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		return tevent_req_post(req, ev);
	}

	in_addr_to_sockaddr_storage(
		&state->server, state->servers[state->num_sent]);

	subreq = name_query_send(state, state->ev,
				 state->name, state->name_type,
				 false, true, &state->server);
	state->num_sent += 1;
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	if (!tevent_req_set_endtime(subreq, state->ev,
				    timeval_current_ofs(2, 0))) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, query_wins_list_done, req);
	return req;
}

static void query_wins_list_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct query_wins_list_state *state = tevent_req_data(
		req, struct query_wins_list_state);
	NTSTATUS status;

	status = name_query_recv(subreq, state,
				 &state->addrs, &state->num_addrs,
				 &state->flags);
	TALLOC_FREE(subreq);
	if (NT_STATUS_IS_OK(status)) {
		tevent_req_done(req);
		return;
	}
	if (!NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
		tevent_req_nterror(req, status);
		return;
	}
	wins_srv_died(state->servers[state->num_sent-1],
		      my_socket_addr_v4());

	if (state->num_sent == state->num_servers) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		return;
	}

	in_addr_to_sockaddr_storage(
		&state->server, state->servers[state->num_sent]);

	subreq = name_query_send(state, state->ev,
				 state->name, state->name_type,
				 false, true, &state->server);
	state->num_sent += 1;
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	if (!tevent_req_set_endtime(subreq, state->ev,
				    timeval_current_ofs(2, 0))) {
		return;
	}
	tevent_req_set_callback(subreq, query_wins_list_done, req);
}

static NTSTATUS query_wins_list_recv(struct tevent_req *req,
				     TALLOC_CTX *mem_ctx,
				     struct sockaddr_storage **addrs,
				     int *num_addrs,
				     uint8_t *flags)
{
	struct query_wins_list_state *state = tevent_req_data(
		req, struct query_wins_list_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (addrs != NULL) {
		*addrs = talloc_move(mem_ctx, &state->addrs);
	}
	if (num_addrs != NULL) {
		*num_addrs = state->num_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

struct resolve_wins_state {
	int num_sent;
	int num_received;

	struct sockaddr_storage *addrs;
	int num_addrs;
	uint8_t flags;
};

static void resolve_wins_done(struct tevent_req *subreq);

struct tevent_req *resolve_wins_send(TALLOC_CTX *mem_ctx,
				     struct tevent_context *ev,
				     const char *name,
				     int name_type)
{
	struct tevent_req *req, *subreq;
	struct resolve_wins_state *state;
	char **wins_tags = NULL;
	struct sockaddr_storage src_ss;
	struct samba_sockaddr src_sa = {0};
	struct in_addr src_ip;
	int i, num_wins_tags;
	bool ok;

	req = tevent_req_create(mem_ctx, &state,
				struct resolve_wins_state);
	if (req == NULL) {
		return NULL;
	}

	if (wins_srv_count() < 1) {
		DEBUG(3,("resolve_wins: WINS server resolution selected "
			"and no WINS servers listed.\n"));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	/* the address we will be sending from */
	if (!interpret_string_addr(&src_ss, lp_nbt_client_socket_address(),
				AI_NUMERICHOST|AI_PASSIVE)) {
		zero_sockaddr(&src_ss);
	}

	ok = sockaddr_storage_to_samba_sockaddr(&src_sa, &src_ss);
	if (!ok) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	if (src_sa.u.ss.ss_family != AF_INET) {
		char addr[INET6_ADDRSTRLEN];
		print_sockaddr(addr, sizeof(addr), &src_sa.u.ss);
		DEBUG(3,("resolve_wins: cannot receive WINS replies "
			"on IPv6 address %s\n",
			addr));
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	src_ip = src_sa.u.in.sin_addr;

	wins_tags = wins_srv_tags();
	if (wins_tags == NULL) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		goto fail;
	}

	num_wins_tags = 0;
	while (wins_tags[num_wins_tags] != NULL) {
		num_wins_tags += 1;
	}

	for (i=0; i<num_wins_tags; i++) {
		int num_servers, num_alive;
		struct in_addr *servers, *alive;
		int j;

		if (!wins_server_tag_ips(wins_tags[i], talloc_tos(),
					 &servers, &num_servers)) {
			DEBUG(10, ("wins_server_tag_ips failed for tag %s\n",
				   wins_tags[i]));
			continue;
		}

		alive = talloc_array(state, struct in_addr, num_servers);
		if (tevent_req_nomem(alive, req)) {
			goto fail;
		}

		num_alive = 0;
		for (j=0; j<num_servers; j++) {
			struct in_addr wins_ip = servers[j];

			if (global_in_nmbd && ismyip_v4(wins_ip)) {
				/* yikes! we'll loop forever */
				continue;
			}
			/* skip any that have been unresponsive lately */
			if (wins_srv_is_dead(wins_ip, src_ip)) {
				continue;
			}
			DEBUG(3, ("resolve_wins: using WINS server %s "
				 "and tag '%s'\n",
				  inet_ntoa(wins_ip), wins_tags[i]));
			alive[num_alive] = wins_ip;
			num_alive += 1;
		}
		TALLOC_FREE(servers);

		if (num_alive == 0) {
			continue;
		}

		subreq = query_wins_list_send(
			state, ev, src_ip, name, name_type,
			alive, num_alive);
		if (tevent_req_nomem(subreq, req)) {
			goto fail;
		}
		tevent_req_set_callback(subreq, resolve_wins_done, req);
		state->num_sent += 1;
	}

	if (state->num_sent == 0) {
		tevent_req_nterror(req, NT_STATUS_NOT_FOUND);
		goto fail;
	}

	wins_srv_tags_free(wins_tags);
	return req;
fail:
	wins_srv_tags_free(wins_tags);
	return tevent_req_post(req, ev);
}

static void resolve_wins_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct resolve_wins_state *state = tevent_req_data(
		req, struct resolve_wins_state);
	NTSTATUS status;

	status = query_wins_list_recv(subreq, state, &state->addrs,
				      &state->num_addrs, &state->flags);
	if (NT_STATUS_IS_OK(status)) {
		tevent_req_done(req);
		return;
	}

	state->num_received += 1;

	if (state->num_received < state->num_sent) {
		/*
		 * Wait for the others
		 */
		return;
	}
	tevent_req_nterror(req, status);
}

NTSTATUS resolve_wins_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			   struct sockaddr_storage **addrs,
			   int *num_addrs, uint8_t *flags)
{
	struct resolve_wins_state *state = tevent_req_data(
		req, struct resolve_wins_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (addrs != NULL) {
		*addrs = talloc_move(mem_ctx, &state->addrs);
	}
	if (num_addrs != NULL) {
		*num_addrs = state->num_addrs;
	}
	if (flags != NULL) {
		*flags = state->flags;
	}
	return NT_STATUS_OK;
}

/********************************************************
 Resolve via "wins" method.
*********************************************************/

NTSTATUS resolve_wins(TALLOC_CTX *mem_ctx,
		const char *name,
		int name_type,
		struct sockaddr_storage **return_iplist,
		int *return_count)
{
	struct tevent_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	ev = samba_tevent_context_init(talloc_tos());
	if (ev == NULL) {
		goto fail;
	}
	req = resolve_wins_send(ev, ev, name, name_type);
	if (req == NULL) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = resolve_wins_recv(req, mem_ctx, return_iplist, return_count,
				   NULL);
fail:
	TALLOC_FREE(ev);
	return status;
}

/********************************************************
 Use ads_dns_lookup_[a|aaaa]_send() calls to look up a
 list of names asynchronously.
*********************************************************/

struct dns_query_state {
	/* Used to tell our parent we've completed. */
	struct dns_lookup_list_async_state *p_async_state;
	const char *query_name; /* For debugging only. */
	size_t num_addrs;
	struct samba_sockaddr *addrs;
};

struct dns_lookup_list_async_state {
	bool timed_out;
	size_t num_query_returns;
	struct dns_query_state *queries;
};

/* Called on query timeout. */
static void dns_lookup_send_timeout_handler(struct tevent_context *ev,
					    struct tevent_timer *te,
					    struct timeval t,
					    void *private_data)
{
	bool *timed_out = (bool *)private_data;
	*timed_out = true;
}

static void dns_lookup_list_a_done(struct tevent_req *req);
#if defined(HAVE_IPV6)
static void dns_lookup_list_aaaa_done(struct tevent_req *req);
#endif

NTSTATUS dns_lookup_list_async(TALLOC_CTX *ctx,
			      size_t num_dns_names,
			      const char **dns_lookup_names,
			      size_t *p_num_addrs,
			      struct samba_sockaddr **pp_addrs,
			      char ***pp_dns_names)
{
	struct dns_lookup_list_async_state *state = NULL;
	struct tevent_context *ev = NULL;
	struct tevent_req *req = NULL;
	struct tevent_timer *timer = NULL;
	size_t num_queries_sent = 0;
	size_t queries_size = num_dns_names;
	size_t i;
	size_t num_addrs = 0;
	struct samba_sockaddr *addr_out = NULL;
	char **dns_names_ret = NULL;
	NTSTATUS status = NT_STATUS_NO_MEMORY;

	/* Nothing to do. */
	if (num_dns_names == 0) {
		*p_num_addrs = 0;
		*pp_addrs = NULL;
		if (pp_dns_names != NULL) {
			*pp_dns_names = NULL;
		}
		return NT_STATUS_OK;
	}

	state = talloc_zero(ctx, struct dns_lookup_list_async_state);
	if (state == NULL) {
		goto fail;
	}
	ev = samba_tevent_context_init(ctx);
	if (ev == NULL) {
		goto fail;
	}

#if defined(HAVE_IPV6)
	queries_size = 2 * num_dns_names;
	/* Wrap protection. */
	if (queries_size < num_dns_names) {
		goto fail;
	}
#else
	queries_size = num_dns_names;
#endif

	state->queries = talloc_zero_array(state,
					   struct dns_query_state,
					   queries_size);
	if (state->queries == NULL) {
		goto fail;
	}

	/* Hit all the DNS servers with asnyc lookups for all the names. */
	for (i = 0; i < num_dns_names; i++) {
		DBG_INFO("async DNS lookup A record for %s\n",
			dns_lookup_names[i]);

		/* Setup the query state. */
		state->queries[num_queries_sent].query_name =
					dns_lookup_names[i];
		state->queries[num_queries_sent].p_async_state = state;

		req = ads_dns_lookup_a_send(ev, ev, dns_lookup_names[i]);
		if (req == NULL) {
			goto fail;
		}
		tevent_req_set_callback(req,
				dns_lookup_list_a_done,
				&state->queries[num_queries_sent]);
		num_queries_sent++;

#if defined(HAVE_IPV6)
		/* If we're IPv6 capable ask for that too. */
		state->queries[num_queries_sent].query_name =
					dns_lookup_names[i];
		state->queries[num_queries_sent].p_async_state = state;

		DBG_INFO("async DNS lookup AAAA record for %s\n",
			dns_lookup_names[i]);

		req = ads_dns_lookup_aaaa_send(ev, ev, dns_lookup_names[i]);
		if (req == NULL) {
			goto fail;
		}
		tevent_req_set_callback(req,
					dns_lookup_list_aaaa_done,
					&state->queries[num_queries_sent]);
		num_queries_sent++;
#endif
	}

	/* We must always have a timeout. */
	timer = tevent_add_timer(ev,
				 ev,
				 timeval_current_ofs(lp_get_async_dns_timeout(),
						     0),
				 dns_lookup_send_timeout_handler,
				 &state->timed_out);
	if (timer == NULL) {
		goto fail;
	}

	/* Loop until timed out or got all replies. */
	for(;;) {
		int ret;

		if (state->timed_out) {
			break;
		}
		if (state->num_query_returns == num_queries_sent) {
			break;
		}
		ret = tevent_loop_once(ev);
		if (ret != 0) {
			goto fail;
		}
	}

	/* Count what we got back. */
	for (i = 0; i < num_queries_sent; i++) {
		struct dns_query_state *query = &state->queries[i];

		/* Wrap check. */
		if (num_addrs + query->num_addrs < num_addrs) {
			goto fail;
		}
		num_addrs += query->num_addrs;
	}

	if (state->timed_out) {
		DBG_INFO("async DNS lookup timed out after %zu entries "
			"(not an error)\n",
			num_addrs);
	}

	addr_out = talloc_zero_array(ctx,
				     struct samba_sockaddr,
				     num_addrs);
	if (addr_out == NULL) {
		goto fail;
	}

	/*
	 * Did the caller want an array of names back
	 * that match the IP addresses ? If we provide
	 * this, dsgetdcname() internals can now use this
	 * async lookup code also.
	 */
	if (pp_dns_names != NULL) {
		dns_names_ret = talloc_zero_array(ctx,
						  char *,
						  num_addrs);
		if (dns_names_ret == NULL) {
			goto fail;
		}
	}

	/* Copy what we got back. */
	num_addrs = 0;
	for (i = 0; i < num_queries_sent; i++) {
		size_t j;
		struct dns_query_state *query = &state->queries[i];

		if (query->num_addrs == 0) {
			continue;
		}

		if (dns_names_ret != NULL) {
			/*
			 * If caller wants a name array matched with
			 * the addrs array, copy the same queried name for
			 * each IP address returned.
			 */
			for (j = 0; j < query->num_addrs; j++) {
				dns_names_ret[num_addrs + j] = talloc_strdup(
						ctx,
						query->query_name);
				if (dns_names_ret[num_addrs + j] == NULL) {
					goto fail;
				}
			}
		}

		for (j = 0; j < query->num_addrs; j++) {
			addr_out[num_addrs] = query->addrs[j];
		}
		num_addrs += query->num_addrs;
	}

	*p_num_addrs = num_addrs;
	*pp_addrs = addr_out;
	if (pp_dns_names != NULL) {
		*pp_dns_names = dns_names_ret;
	}

	status = NT_STATUS_OK;

  fail:

	TALLOC_FREE(state);
	TALLOC_FREE(ev);
	return status;
}

/*
 Called when an A record lookup completes.
*/

static void dns_lookup_list_a_done(struct tevent_req *req)
{
	/*
	 * Callback data is an element of a talloc'ed array,
	 * not a talloc object in its own right. So use the
	 * tevent_req_callback_data_void() void * cast function.
	 */
	struct dns_query_state *state = (struct dns_query_state *)
				tevent_req_callback_data_void(req);
	uint8_t rcode = 0;
	size_t i;
	char **hostnames_out = NULL;
	struct samba_sockaddr *addrs = NULL;
	size_t num_addrs = 0;
	NTSTATUS status;

	/* For good or ill, tell the parent we're finished. */
	state->p_async_state->num_query_returns++;

	status = ads_dns_lookup_a_recv(req,
				       state->p_async_state,
				       &rcode,
				       &num_addrs,
				       &hostnames_out,
				       &addrs);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_INFO("async DNS A lookup for %s returned %s\n",
			state->query_name,
			nt_errstr(status));
		return;
	}

	if (rcode != DNS_RCODE_OK) {
		DBG_INFO("async DNS A lookup for %s returned DNS code %u\n",
			state->query_name,
			(unsigned int)rcode);
		return;
	}

	if (num_addrs == 0) {
		DBG_INFO("async DNS A lookup for %s returned 0 addresses.\n",
			state->query_name);
		return;
	}

	/* Copy data out. */
	state->addrs = talloc_zero_array(state->p_async_state,
					 struct samba_sockaddr,
					 num_addrs);
	if (state->addrs == NULL) {
		return;
	}

	for (i = 0; i < num_addrs; i++) {
		char addr[INET6_ADDRSTRLEN];
		DBG_INFO("async DNS A lookup for %s [%zu] got %s -> %s\n",
			state->query_name,
			i,
			hostnames_out[i],
			print_sockaddr(addr,
				sizeof(addr),
				&addrs[i].u.ss));

		state->addrs[i] = addrs[i];
	}
	state->num_addrs = num_addrs;
}

#if defined(HAVE_IPV6)
/*
 Called when an AAAA record lookup completes.
*/

static void dns_lookup_list_aaaa_done(struct tevent_req *req)
{
	/*
	 * Callback data is an element of a talloc'ed array,
	 * not a talloc object in its own right. So use the
	 * tevent_req_callback_data_void() void * cast function.
	 */
	struct dns_query_state *state = (struct dns_query_state *)
				tevent_req_callback_data_void(req);
	uint8_t rcode = 0;
	size_t i;
	char **hostnames_out = NULL;
	struct samba_sockaddr *addrs = NULL;
	size_t num_addrs = 0;
	NTSTATUS status;

	/* For good or ill, tell the parent we're finished. */
	state->p_async_state->num_query_returns++;

	status = ads_dns_lookup_aaaa_recv(req,
					  state->p_async_state,
					  &rcode,
					  &num_addrs,
					  &hostnames_out,
					  &addrs);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_INFO("async DNS AAAA lookup for %s returned %s\n",
			state->query_name,
			nt_errstr(status));
		return;
	}

	if (rcode != DNS_RCODE_OK) {
		DBG_INFO("async DNS AAAA lookup for %s returned DNS code %u\n",
			state->query_name,
			(unsigned int)rcode);
		return;
	}

	if (num_addrs == 0) {
		DBG_INFO("async DNS AAAA lookup for %s returned 0 addresses.\n",
			state->query_name);
		return;
	}

	/* Copy data out. */
	state->addrs = talloc_zero_array(state->p_async_state,
					 struct samba_sockaddr,
					 num_addrs);
	if (state->addrs == NULL) {
		return;
	}

	for (i = 0; i < num_addrs; i++) {
		char addr[INET6_ADDRSTRLEN];
		DBG_INFO("async DNS AAAA lookup for %s [%zu] got %s -> %s\n",
			state->query_name,
			i,
			hostnames_out[i],
			print_sockaddr(addr,
				sizeof(addr),
				&addrs[i].u.ss));
		state->addrs[i] = addrs[i];
	}
	state->num_addrs = num_addrs;
}
#endif

/********************************************************
 Resolve via "hosts" method.
*********************************************************/

static NTSTATUS resolve_hosts(TALLOC_CTX *mem_ctx,
			      const char *name,
			      int name_type,
			      struct sockaddr_storage **return_iplist,
			      int *return_count)
{
	/*
	 * "host" means do a localhost, or dns lookup.
	 */
	struct addrinfo hints;
	struct addrinfo *ailist = NULL;
	struct addrinfo *res = NULL;
	int ret = -1;
	int i = 0;

	if ( name_type != 0x20 && name_type != 0x0) {
		DEBUG(5, ("resolve_hosts: not appropriate "
			"for name type <0x%x>\n",
			name_type));
		return NT_STATUS_INVALID_PARAMETER;
	}

	*return_iplist = NULL;
	*return_count = 0;

	DEBUG(3,("resolve_hosts: Attempting host lookup for name %s<0x%x>\n",
				name, name_type));

	ZERO_STRUCT(hints);
	/* By default make sure it supports TCP. */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;

#if !defined(HAVE_IPV6)
	/* Unless we have IPv6, we really only want IPv4 addresses back. */
	hints.ai_family = AF_INET;
#endif

	ret = getaddrinfo(name,
			NULL,
			&hints,
			&ailist);
	if (ret) {
		DEBUG(3,("resolve_hosts: getaddrinfo failed for name %s [%s]\n",
			name,
			gai_strerror(ret) ));
	}

	for (res = ailist; res; res = res->ai_next) {
		struct sockaddr_storage ss;

		if (!res->ai_addr || res->ai_addrlen == 0) {
			continue;
		}

		ZERO_STRUCT(ss);
		memcpy(&ss, res->ai_addr, res->ai_addrlen);

		if (is_zero_addr(&ss)) {
			continue;
		}

		*return_count += 1;

		*return_iplist = talloc_realloc(
			mem_ctx, *return_iplist, struct sockaddr_storage,
			*return_count);
		if (!*return_iplist) {
			DEBUG(3,("resolve_hosts: malloc fail !\n"));
			freeaddrinfo(ailist);
			return NT_STATUS_NO_MEMORY;
		}
		(*return_iplist)[i] = ss;
		i++;
	}
	if (ailist) {
		freeaddrinfo(ailist);
	}
	if (*return_count) {
		return NT_STATUS_OK;
	}
	return NT_STATUS_UNSUCCESSFUL;
}

/********************************************************
 Resolve via "ADS" method.
*********************************************************/

/* Special name type used to cause a _kerberos DNS lookup. */
#define KDC_NAME_TYPE 0xDCDC

static NTSTATUS resolve_ads(TALLOC_CTX *ctx,
			    const char *name,
			    int name_type,
			    const char *sitename,
			    struct sockaddr_storage **return_addrs,
			    int *return_count)
{
	int 			i;
	NTSTATUS  		status;
	struct dns_rr_srv	*dcs = NULL;
	int			numdcs = 0;
	int			numaddrs = 0;
	size_t num_srv_addrs = 0;
	struct sockaddr_storage *srv_addrs = NULL;
	size_t num_dns_addrs = 0;
	struct samba_sockaddr *dns_addrs = NULL;
	size_t num_dns_names = 0;
	const char **dns_lookup_names = NULL;
	struct sockaddr_storage *ret_addrs = NULL;

	if ((name_type != 0x1c) && (name_type != KDC_NAME_TYPE) &&
	    (name_type != 0x1b)) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	switch (name_type) {
		case 0x1b:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "PDC for %s using DNS\n", name));
			status = ads_dns_query_pdc(ctx,
						   name,
						   &dcs,
						   &numdcs);
			break;

		case 0x1c:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "DCs for %s using DNS\n", name));
			status = ads_dns_query_dcs(ctx,
						   name,
						   sitename,
						   &dcs,
						   &numdcs);
			break;
		case KDC_NAME_TYPE:
			DEBUG(5,("resolve_ads: Attempting to resolve "
				 "KDCs for %s using DNS\n", name));
			status = ads_dns_query_kdcs(ctx,
						    name,
						    sitename,
						    &dcs,
						    &numdcs);
			break;
		default:
			status = NT_STATUS_INVALID_PARAMETER;
			break;
	}

	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(dcs);
		return status;
	}

	if (numdcs == 0) {
		*return_addrs = NULL;
		*return_count = 0;
		TALLOC_FREE(dcs);
		return NT_STATUS_OK;
	}

	/* Paranoia. */
	if (numdcs < 0) {
		TALLOC_FREE(dcs);
		return NT_STATUS_INVALID_PARAMETER;
	}

	/*
	 * Split the returned values into 2 arrays. First one
	 * is a struct sockaddr_storage array that contains results
	 * from the SRV record lookup that contain both hostnames
	 * and IP addresses. We only need to copy out the IP
	 * addresses. This is srv_addrs.
	 *
	 * Second array contains the results from the SRV record
	 * lookup that only contain hostnames - no IP addresses.
	 * We must then call dns_lookup_list() to lookup
	 * hostnames -> IP address. This is dns_addrs.
	 *
	 * Finally we will merge these two arrays to create the
	 * return sockaddr_storage array.
	 */

	/* First count the sizes of each array. */
	for(i = 0; i < numdcs; i++) {
		if (dcs[i].ss_s != NULL) {
			/* IP address returned in SRV record. */
			if (num_srv_addrs + dcs[i].num_ips < num_srv_addrs) {
				/* Wrap check. */
				TALLOC_FREE(dcs);
				return NT_STATUS_INVALID_PARAMETER;
			}
			/* Add in the number of addresses we got. */
			num_srv_addrs += dcs[i].num_ips;
			/*
			 * If we got any IP addresses zero out
			 * the hostname so we know we've already
			 * processed this entry and won't add it
			 * to the dns_lookup_names array we use
			 * to do DNS queries below.
			 */
			dcs[i].hostname = NULL;
		} else {
			/* Ensure we have a hostname to lookup. */
			if (dcs[i].hostname == NULL) {
				continue;
			}
			/* No IP address returned in SRV record. */
			if (num_dns_names + 1 < num_dns_names) {
				/* Wrap check. */
				TALLOC_FREE(dcs);
				return NT_STATUS_INVALID_PARAMETER;
			}
			/* One more name to lookup. */
			num_dns_names += 1;
		}
	}

	/* Allocate the list of IP addresses we already have. */
	srv_addrs = talloc_zero_array(ctx,
				struct sockaddr_storage,
				num_srv_addrs);
	if (srv_addrs == NULL) {
		TALLOC_FREE(dcs);
		return NT_STATUS_NO_MEMORY;
	}

	/* Copy the addresses we already have. */
	num_srv_addrs = 0;
	for(i = 0; i < numdcs; i++) {
		/* Copy all the IP addresses from the SRV response */
		size_t j;
		for (j = 0; j < dcs[i].num_ips; j++) {
			char addr[INET6_ADDRSTRLEN];

			srv_addrs[num_srv_addrs] = dcs[i].ss_s[j];
			if (is_zero_addr(&srv_addrs[num_srv_addrs])) {
				continue;
			}

			DBG_DEBUG("SRV lookup %s got IP[%zu] %s\n",
				name,
				j,
				print_sockaddr(addr,
					sizeof(addr),
					&srv_addrs[num_srv_addrs]));

			num_srv_addrs++;
		}
	}

	/* Allocate the array of hostnames we must look up. */
	dns_lookup_names = talloc_zero_array(ctx,
					const char *,
					num_dns_names);
	if (dns_lookup_names == NULL) {
		TALLOC_FREE(dcs);
		TALLOC_FREE(srv_addrs);
		return NT_STATUS_NO_MEMORY;
	}

	num_dns_names = 0;
	for(i = 0; i < numdcs; i++) {
		if (dcs[i].hostname == NULL) {
			/*
			 * Must have been a SRV return with an IP address.
			 * We don't need to look up this hostname.
			 */
			continue;
                }
		dns_lookup_names[num_dns_names] = dcs[i].hostname;
		num_dns_names++;
	}

	/* Lookup the addresses on the dns_lookup_list. */
	status = dns_lookup_list_async(ctx,
				num_dns_names,
				dns_lookup_names,
				&num_dns_addrs,
				&dns_addrs,
				NULL);

	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(dcs);
		TALLOC_FREE(srv_addrs);
		TALLOC_FREE(dns_lookup_names);
		TALLOC_FREE(dns_addrs);
		return status;
	}

	/*
	 * Combine the two sockaddr_storage arrays into a talloc'ed
	 * struct sockaddr_storage array return.
         */

	numaddrs = num_srv_addrs + num_dns_addrs;
	/* Wrap check + bloody int conversion check :-(. */
	if (numaddrs < num_srv_addrs ||
				numaddrs < 0) {
		TALLOC_FREE(dcs);
		TALLOC_FREE(srv_addrs);
		TALLOC_FREE(dns_addrs);
		TALLOC_FREE(dns_lookup_names);
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (numaddrs == 0) {
		/* Keep the same semantics for zero names. */
		TALLOC_FREE(dcs);
		TALLOC_FREE(srv_addrs);
		TALLOC_FREE(dns_addrs);
		TALLOC_FREE(dns_lookup_names);
		*return_addrs = NULL;
		*return_count = 0;
		return NT_STATUS_OK;
	}

	ret_addrs = talloc_zero_array(ctx,
				struct sockaddr_storage,
				numaddrs);
	if (ret_addrs == NULL) {
		TALLOC_FREE(dcs);
		TALLOC_FREE(srv_addrs);
		TALLOC_FREE(dns_addrs);
		TALLOC_FREE(dns_lookup_names);
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_srv_addrs; i++) {
		ret_addrs[i] = srv_addrs[i];
	}
	for (i = 0; i < num_dns_addrs; i++) {
		ret_addrs[num_srv_addrs+i] = dns_addrs[i].u.ss;
	}

	TALLOC_FREE(dcs);
	TALLOC_FREE(srv_addrs);
	TALLOC_FREE(dns_addrs);
	TALLOC_FREE(dns_lookup_names);

	*return_addrs = ret_addrs;
	*return_count = numaddrs;
	return NT_STATUS_OK;
}

static const char **filter_out_nbt_lookup(TALLOC_CTX *mem_ctx,
					  const char **resolve_order)
{
	size_t i, len, result_idx;
	const char **result;

	len = 0;
	while (resolve_order[len] != NULL) {
		len += 1;
	}

	result = talloc_array(mem_ctx, const char *, len+1);
	if (result == NULL) {
		return NULL;
	}

	result_idx = 0;

	for (i=0; i<len; i++) {
		const char *tok = resolve_order[i];

		if (strequal(tok, "lmhosts") || strequal(tok, "wins") ||
		    strequal(tok, "bcast")) {
			continue;
		}
		result[result_idx++] = tok;
	}
	result[result_idx] = NULL;

	return result;
}

/*******************************************************************
 Samba interface to resolve a name into an IP address.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.

 Added support for ip addr/port to support ADS ldap servers.
 the only place we currently care about the port is in the
 resolve_hosts() when looking up DC's via SRV RR entries in DNS
**********************************************************************/

NTSTATUS internal_resolve_name(TALLOC_CTX *ctx,
				const char *name,
			        int name_type,
				const char *sitename,
				struct ip_service **return_iplist,
				size_t *return_count,
				const char **resolve_order)
{
	const char *tok;
	NTSTATUS status = NT_STATUS_UNSUCCESSFUL;
	size_t i;
	size_t nc_count = 0;
	size_t ret_count = 0;
	/*
	 * Integer count of addresses returned from resolve_XXX()
	 * functions. This will go away when all of them return
	 * size_t.
	*/
	int icount = 0;
	bool ok;
	struct sockaddr_storage *ss_list = NULL;
	struct samba_sockaddr *sa_list = NULL;
	struct ip_service *iplist = NULL;
	TALLOC_CTX *frame = talloc_stackframe();

	*return_iplist = NULL;
	*return_count = 0;

	DBG_DEBUG("looking up %s#%x (sitename %s)\n",
		name, name_type, sitename ? sitename : "(null)");

	if (is_ipaddress(name)) {
		struct sockaddr_storage ss;

		/* if it's in the form of an IP address then get the lib to interpret it */
		ok = interpret_string_addr(&ss, name, AI_NUMERICHOST);
		if (!ok) {
			DBG_WARNING("interpret_string_addr failed on %s\n",
				name);
			TALLOC_FREE(frame);
			return NT_STATUS_INVALID_PARAMETER;
		}
		if (is_zero_addr(&ss)) {
			TALLOC_FREE(frame);
			return NT_STATUS_UNSUCCESSFUL;
		}

		iplist = talloc_zero_array(frame,
					struct ip_service,
					1);
		if (iplist == NULL) {
			TALLOC_FREE(frame);
			return NT_STATUS_NO_MEMORY;
		}

		iplist[0].ss = ss;
		/* ignore the port here */
		iplist[0].port = PORT_NONE;

		*return_iplist = talloc_move(ctx, &iplist);
		*return_count = 1;
		TALLOC_FREE(frame);
		return NT_STATUS_OK;
	}

	/* Check name cache */

	ok = namecache_fetch(frame,
				name,
				name_type,
				&sa_list,
				&nc_count);
	if (ok) {
		/*
		 * Create a struct ip_service list from the
		 * returned samba_sockaddrs.
		 */
		size_t count = 0;

		iplist = talloc_zero_array(frame,
					struct ip_service,
					nc_count);
		if (iplist == NULL) {
			TALLOC_FREE(frame);
			return NT_STATUS_NO_MEMORY;
		}
		count = 0;
		for (i = 0; i < nc_count; i++) {
			if (is_zero_addr(&sa_list[i].u.ss)) {
				continue;
			}
			iplist[count].ss = sa_list[i].u.ss;
			iplist[count].port = 0;
			count++;
		}
		count = remove_duplicate_addrs2(iplist, count);
		if (count == 0) {
			TALLOC_FREE(iplist);
			TALLOC_FREE(frame);
			return NT_STATUS_UNSUCCESSFUL;
		}
		*return_count = count;
		*return_iplist = talloc_move(ctx, &iplist);
		TALLOC_FREE(frame);
		return NT_STATUS_OK;
	}

	/* set the name resolution order */

	if (resolve_order && strcmp(resolve_order[0], "NULL") == 0) {
		DBG_DEBUG("all lookups disabled\n");
		TALLOC_FREE(frame);
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (!resolve_order || !resolve_order[0]) {
		static const char *host_order[] = { "host", NULL };
		resolve_order = host_order;
	}

	if ((strlen(name) > MAX_NETBIOSNAME_LEN - 1) ||
	    (strchr(name, '.') != NULL)) {
		/*
		 * Don't do NBT lookup, the name would not fit anyway
		 */
		resolve_order = filter_out_nbt_lookup(frame, resolve_order);
		if (resolve_order == NULL) {
			TALLOC_FREE(frame);
			return NT_STATUS_NO_MEMORY;
		}
	}

	/* iterate through the name resolution backends */

	for (i=0; resolve_order[i]; i++) {
		tok = resolve_order[i];

		if((strequal(tok, "host") || strequal(tok, "hosts"))) {
			status = resolve_hosts(talloc_tos(),
					       name,
					       name_type,
					       &ss_list,
					       &icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			goto done;
		} else if(strequal( tok, "kdc")) {
			/* deal with KDC_NAME_TYPE names here.
			 * This will result in a SRV record lookup */
			status = resolve_ads(talloc_tos(),
					     name,
					     KDC_NAME_TYPE,
					     sitename,
					     &ss_list,
					     &icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			/* Ensure we don't namecache
			 * this with the KDC port. */
			name_type = KDC_NAME_TYPE;
			goto done;
		} else if(strequal( tok, "ads")) {
			/* deal with 0x1c and 0x1b names here.
			 * This will result in a SRV record lookup */
			status = resolve_ads(talloc_tos(),
					     name,
					     name_type,
					     sitename,
					     &ss_list,
					     &icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			goto done;
		} else if (strequal(tok, "lmhosts")) {
			status = resolve_lmhosts_file_as_sockaddr(
				talloc_tos(),
				get_dyn_LMHOSTSFILE(),
				name,
				name_type,
				&ss_list,
				&icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			goto done;
		} else if (strequal(tok, "wins")) {
			/* don't resolve 1D via WINS */
			if (name_type == 0x1D) {
				continue;
			}
			status = resolve_wins(talloc_tos(),
					      name,
					      name_type,
					      &ss_list,
					      &icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			goto done;
		} else if (strequal(tok, "bcast")) {
			status = name_resolve_bcast(
						talloc_tos(),
						name,
						name_type,
						&ss_list,
						&icount);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			goto done;
		} else {
			DBG_ERR("unknown name switch type %s\n",
				tok);
		}
	}

	/* All of the resolve_* functions above have returned false. */

	TALLOC_FREE(frame);
	*return_count = 0;

	return status;

  done:

	/* Paranoia. */
	if (icount < 0) {
		TALLOC_FREE(frame);
		return NT_STATUS_INVALID_PARAMETER;
	}

	/*
	 * convert_ss2service() leaves the correct
	 * count to return after removing zero addresses
	 * in ret_count.
	 */
	ok = convert_ss2service(frame,
				&iplist,
				ss_list,
				icount,
				&ret_count);
	if (!ok) {
		TALLOC_FREE(iplist);
		TALLOC_FREE(frame);
		return NT_STATUS_NO_MEMORY;
	}

	/* Remove duplicate entries.  Some queries, notably #1c (domain
	controllers) return the PDC in iplist[0] and then all domain
	controllers including the PDC in iplist[1..n].  Iterating over
	the iplist when the PDC is down will cause two sets of timeouts. */

	ret_count = remove_duplicate_addrs2(iplist, ret_count);

	/* Save in name cache */
	if ( DEBUGLEVEL >= 100 ) {
		for (i = 0; i < ret_count && DEBUGLEVEL == 100; i++) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr, sizeof(addr),
					&iplist[i].ss);
			DEBUG(100, ("Storing name %s of type %d (%s:%d)\n",
					name,
					name_type,
					addr,
					iplist[i].port));
		}
	}

	if (ret_count) {
		/*
		 * Convert the ip_service list to a samba_sockaddr array
		 * to store in the namecache. This conversion
		 * will go away once ip_service is gone.
		 */
		struct samba_sockaddr *sa_converted_list = NULL;
		status = ip_service_to_samba_sockaddr(talloc_tos(),
					&sa_converted_list,
					iplist,
					ret_count);
		if (!NT_STATUS_IS_OK(status)) {
			TALLOC_FREE(iplist);
			TALLOC_FREE(frame);
			return status;
		}
		namecache_store(name,
				name_type,
				ret_count,
				sa_converted_list);
		TALLOC_FREE(sa_converted_list);
	}

	/* Display some debugging info */

	if ( DEBUGLEVEL >= 10 ) {
		DBG_DEBUG("returning %zu addresses: ",
				ret_count);

		for (i = 0; i < ret_count; i++) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr, sizeof(addr),
					&iplist[i].ss);
			DEBUGADD(10, ("%s:%d ",
					addr,
					iplist[i].port));
		}
		DEBUG(10, ("\n"));
	}

	*return_count = ret_count;
	*return_iplist = talloc_move(ctx, &iplist);

	TALLOC_FREE(frame);
	return status;
}

/********************************************************
 Internal interface to resolve a name into one IP address.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.
*********************************************************/

bool resolve_name(const char *name,
		struct sockaddr_storage *return_ss,
		int name_type,
		bool prefer_ipv4)
{
	struct ip_service *ss_list = NULL;
	char *sitename = NULL;
	size_t count = 0;
	NTSTATUS status;
	TALLOC_CTX *frame = NULL;

	if (is_ipaddress(name)) {
		return interpret_string_addr(return_ss, name, AI_NUMERICHOST);
	}

	frame = talloc_stackframe();

	sitename = sitename_fetch(frame, lp_realm()); /* wild guess */

	status = internal_resolve_name(frame,
					name,
					name_type,
					sitename,
					&ss_list,
					&count,
					lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		size_t i;

		if (prefer_ipv4) {
			for (i=0; i<count; i++) {
				struct samba_sockaddr sa = {0};
				bool ok;

				ok = sockaddr_storage_to_samba_sockaddr(&sa,
								&ss_list[i].ss);
				if (!ok) {
					TALLOC_FREE(ss_list);
					TALLOC_FREE(frame);
					return false;
				}
				if (!is_broadcast_addr(&sa.u.sa) &&
						(sa.u.ss.ss_family == AF_INET)) {
					*return_ss = ss_list[i].ss;
					TALLOC_FREE(ss_list);
					TALLOC_FREE(frame);
					return True;
				}
			}
		}

		/* only return valid addresses for TCP connections */
		for (i=0; i<count; i++) {
			struct samba_sockaddr sa = {0};
			bool ok;

			ok = sockaddr_storage_to_samba_sockaddr(&sa,
								&ss_list[i].ss);
			if (!ok) {
				TALLOC_FREE(ss_list);
				TALLOC_FREE(frame);
				return false;
			}
			if (!is_broadcast_addr(&sa.u.sa)) {
				*return_ss = ss_list[i].ss;
				TALLOC_FREE(ss_list);
				TALLOC_FREE(frame);
				return True;
			}
		}
	}

	TALLOC_FREE(ss_list);
	TALLOC_FREE(frame);
	return False;
}

/********************************************************
 Internal interface to resolve a name into a list of IP addresses.
 Use this function if the string is either an IP address, DNS
 or host name or NetBIOS name. This uses the name switch in the
 smb.conf to determine the order of name resolution.
*********************************************************/

NTSTATUS resolve_name_list(TALLOC_CTX *ctx,
		const char *name,
		int name_type,
		struct sockaddr_storage **return_ss_arr,
		unsigned int *p_num_entries)
{
	struct ip_service *ss_list = NULL;
	char *sitename = NULL;
	size_t count = 0;
	size_t i;
	unsigned int num_entries = 0;
	struct sockaddr_storage *result_arr = NULL;
	NTSTATUS status;

	if (is_ipaddress(name)) {
		result_arr = talloc(ctx, struct sockaddr_storage);
		if (result_arr == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
		if (!interpret_string_addr(result_arr, name, AI_NUMERICHOST)) {
			TALLOC_FREE(result_arr);
			return NT_STATUS_BAD_NETWORK_NAME;
		}
		*p_num_entries = 1;
		*return_ss_arr = result_arr;
		return NT_STATUS_OK;
	}

	sitename = sitename_fetch(ctx, lp_realm()); /* wild guess */

	status = internal_resolve_name(ctx,
					name,
					name_type,
					sitename,
					&ss_list,
					&count,
					lp_name_resolve_order());
	TALLOC_FREE(sitename);

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* only return valid addresses for TCP connections */
	for (i=0, num_entries = 0; i<count; i++) {
		struct samba_sockaddr sa = {0};
		bool ok;

		ok = sockaddr_storage_to_samba_sockaddr(&sa,
							&ss_list[i].ss);
		if (!ok) {
			continue;
		}
		if (!is_zero_addr(&sa.u.ss) &&
		    !is_broadcast_addr(&sa.u.sa)) {
			num_entries++;
		}
	}
	if (num_entries == 0) {
		status = NT_STATUS_BAD_NETWORK_NAME;
		goto done;
	}

	result_arr = talloc_array(ctx,
				struct sockaddr_storage,
				num_entries);
	if (result_arr == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	for (i=0, num_entries = 0; i<count; i++) {
		struct samba_sockaddr sa = {0};
		bool ok;

		ok = sockaddr_storage_to_samba_sockaddr(&sa,
							&ss_list[i].ss);
		if (!ok) {
			continue;
		}
		if (!is_zero_addr(&sa.u.ss) &&
		    !is_broadcast_addr(&sa.u.sa)) {
			result_arr[num_entries++] = ss_list[i].ss;
		}
	}

	if (num_entries == 0) {
		TALLOC_FREE(result_arr);
		status = NT_STATUS_BAD_NETWORK_NAME;
		goto done;
	}

	status = NT_STATUS_OK;
	*p_num_entries = num_entries;
	*return_ss_arr = result_arr;
done:
	TALLOC_FREE(ss_list);
	return status;
}

/********************************************************
 Find the IP address of the master browser or DMB for a workgroup.
*********************************************************/

bool find_master_ip(const char *group, struct sockaddr_storage *master_ss)
{
	struct ip_service *ip_list = NULL;
	size_t count = 0;
	NTSTATUS status;

	if (lp_disable_netbios()) {
		DEBUG(5,("find_master_ip(%s): netbios is disabled\n", group));
		return false;
	}

	status = internal_resolve_name(talloc_tos(),
					group,
					0x1D,
					NULL,
					&ip_list,
					&count,
					lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		*master_ss = ip_list[0].ss;
		TALLOC_FREE(ip_list);
		return true;
	}

	TALLOC_FREE(ip_list);

	status = internal_resolve_name(talloc_tos(),
					group,
					0x1B,
					NULL,
					&ip_list,
					&count,
					lp_name_resolve_order());
	if (NT_STATUS_IS_OK(status)) {
		*master_ss = ip_list[0].ss;
		TALLOC_FREE(ip_list);
		return true;
	}

	TALLOC_FREE(ip_list);
	return false;
}

/********************************************************
 Get the IP address list of the primary domain controller
 for a domain.
*********************************************************/

bool get_pdc_ip(const char *domain, struct sockaddr_storage *pss)
{
	struct ip_service *ip_list = NULL;
	size_t count = 0;
	NTSTATUS status = NT_STATUS_DOMAIN_CONTROLLER_NOT_FOUND;
	static const char *ads_order[] = { "ads", NULL };
	/* Look up #1B name */

	if (lp_security() == SEC_ADS) {
		status = internal_resolve_name(talloc_tos(),
						domain,
						0x1b,
						NULL,
						&ip_list,
						&count,
						ads_order);
	}

	if (!NT_STATUS_IS_OK(status) || count == 0) {
		TALLOC_FREE(ip_list);
		status = internal_resolve_name(talloc_tos(),
						domain,
						0x1b,
						NULL,
						&ip_list,
						&count,
						lp_name_resolve_order());
		if (!NT_STATUS_IS_OK(status)) {
			TALLOC_FREE(ip_list);
			return false;
		}
	}

	/* if we get more than 1 IP back we have to assume it is a
	   multi-homed PDC and not a mess up */

	if ( count > 1 ) {
		DBG_INFO("PDC has %zu IP addresses!\n", count);
		sort_service_list(ip_list, count);
	}

	*pss = ip_list[0].ss;
	TALLOC_FREE(ip_list);
	return true;
}

/* Private enum type for lookups. */

enum dc_lookup_type { DC_NORMAL_LOOKUP, DC_ADS_ONLY, DC_KDC_ONLY };

/********************************************************
 Get the IP address list of the domain controllers for
 a domain.
*********************************************************/

static NTSTATUS get_dc_list(TALLOC_CTX *ctx,
			const char *domain,
			const char *sitename,
			struct ip_service **ip_list,
			size_t *ret_count,
			enum dc_lookup_type lookup_type,
			bool *ordered)
{
	const char **resolve_order = NULL;
	char *saf_servername = NULL;
	char *pserver = NULL;
	const char *p;
	char *port_str = NULL;
	int port;
	char *name;
	size_t num_addresses = 0;
	size_t local_count = 0;
	size_t i;
	struct ip_service *return_iplist = NULL;
	struct ip_service *auto_ip_list = NULL;
	bool done_auto_lookup = false;
	size_t auto_count = 0;
	NTSTATUS status;
	TALLOC_CTX *frame = talloc_stackframe();
	int auto_name_type = 0x1C;

	*ordered = False;

	/* if we are restricted to solely using DNS for looking
	   up a domain controller, make sure that host lookups
	   are enabled for the 'name resolve order'.  If host lookups
	   are disabled and ads_only is True, then set the string to
	   NULL. */

	resolve_order = lp_name_resolve_order();
	if (!resolve_order) {
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}
	if (lookup_type == DC_ADS_ONLY)  {
		if (str_list_check_ci(resolve_order, "host")) {
			static const char *ads_order[] = { "ads", NULL };
			resolve_order = ads_order;

			/* DNS SRV lookups used by the ads resolver
			   are already sorted by priority and weight */
			*ordered = true;
		} else {
			/* this is quite bizarre! */
			static const char *null_order[] = { "NULL", NULL };
                        resolve_order = null_order;
		}
	} else if (lookup_type == DC_KDC_ONLY) {
		static const char *kdc_order[] = { "kdc", NULL };
		/* DNS SRV lookups used by the ads/kdc resolver
		   are already sorted by priority and weight */
		*ordered = true;
		resolve_order = kdc_order;
		auto_name_type = KDC_NAME_TYPE;
	}

	/* fetch the server we have affinity for.  Add the
	   'password server' list to a search for our domain controllers */

	saf_servername = saf_fetch(frame, domain);

	if (strequal(domain, lp_workgroup()) || strequal(domain, lp_realm())) {
		pserver = talloc_asprintf(frame, "%s, %s",
			saf_servername ? saf_servername : "",
			lp_password_server());
	} else {
		pserver = talloc_asprintf(frame, "%s, *",
			saf_servername ? saf_servername : "");
	}

	TALLOC_FREE(saf_servername);
	if (!pserver) {
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}

	DEBUG(3,("get_dc_list: preferred server list: \"%s\"\n", pserver ));

	/*
	 * if '*' appears in the "password server" list then add
	 * an auto lookup to the list of manually configured
	 * DC's.  If any DC is listed by name, then the list should be
	 * considered to be ordered
	 */

	p = pserver;
	while (next_token_talloc(frame, &p, &name, LIST_SEP)) {
		if (!done_auto_lookup && strequal(name, "*")) {
			done_auto_lookup = true;

			status = internal_resolve_name(frame,
							domain,
							auto_name_type,
							sitename,
							&auto_ip_list,
							&auto_count,
							resolve_order);
			if (!NT_STATUS_IS_OK(status)) {
				continue;
			}
			/* Wrap check. */
			if (num_addresses + auto_count < num_addresses) {
				TALLOC_FREE(auto_ip_list);
				status = NT_STATUS_INVALID_PARAMETER;
				goto out;
			}
			num_addresses += auto_count;
			DBG_DEBUG("Adding %zu DC's from auto lookup\n",
						auto_count);
		} else  {
			/* Wrap check. */
			if (num_addresses + 1 < num_addresses) {
				TALLOC_FREE(auto_ip_list);
				status = NT_STATUS_INVALID_PARAMETER;
				goto out;
			}
			num_addresses++;
		}
	}

	/* if we have no addresses and haven't done the auto lookup, then
	   just return the list of DC's.  Or maybe we just failed. */

	if (num_addresses == 0) {
		struct ip_service *dc_iplist = NULL;
		size_t dc_count = 0;

		if (done_auto_lookup) {
			DEBUG(4,("get_dc_list: no servers found\n"));
			status = NT_STATUS_NO_LOGON_SERVERS;
			goto out;
		}
		/* talloc off frame, only move to ctx on success. */
		status = internal_resolve_name(frame,
						domain,
						auto_name_type,
						sitename,
						&dc_iplist,
						&dc_count,
						resolve_order);
		if (!NT_STATUS_IS_OK(status)) {
			goto out;
		}
		return_iplist = talloc_move(ctx, &dc_iplist);
		local_count = dc_count;
		goto out;
	}

	return_iplist = talloc_zero_array(ctx,
					struct ip_service,
					num_addresses);
	if (return_iplist == NULL) {
		DEBUG(3,("get_dc_list: malloc fail !\n"));
		status = NT_STATUS_NO_MEMORY;
		goto out;
	}

	p = pserver;
	local_count = 0;

	/* fill in the return list now with real IP's */

	while ((local_count<num_addresses) &&
			next_token_talloc(frame, &p, &name, LIST_SEP)) {
		struct samba_sockaddr name_sa = {0};

		/* copy any addresses from the auto lookup */

		if (strequal(name, "*")) {
			size_t j;
			for (j=0; j<auto_count; j++) {
				char addr[INET6_ADDRSTRLEN];
				print_sockaddr(addr,
						sizeof(addr),
						&auto_ip_list[j].ss);
				/* Check for and don't copy any
				 * known bad DC IP's. */
				if(!NT_STATUS_IS_OK(check_negative_conn_cache(
						domain,
						addr))) {
					DEBUG(5,("get_dc_list: "
						"negative entry %s removed "
						"from DC list\n",
						addr));
					continue;
				}
				return_iplist[local_count].ss =
					auto_ip_list[j].ss;
				return_iplist[local_count].port =
					auto_ip_list[j].port;
				local_count++;
			}
			continue;
		}

		/* added support for address:port syntax for ads
		 * (not that I think anyone will ever run the LDAP
		 * server in an AD domain on something other than
		 * port 389
		 * However, the port should not be used for kerberos
		 */

		port = (lookup_type == DC_ADS_ONLY) ? LDAP_PORT :
			((lookup_type == DC_KDC_ONLY) ? DEFAULT_KRB5_PORT :
			 PORT_NONE);
		if ((port_str=strchr(name, ':')) != NULL) {
			*port_str = '\0';
			if (lookup_type != DC_KDC_ONLY) {
				port_str++;
				port = atoi(port_str);
			}
		}

		/* explicit lookup; resolve_name() will
		 * handle names & IP addresses */
		if (resolve_name(name, &name_sa.u.ss, 0x20, true)) {
			char addr[INET6_ADDRSTRLEN];
			bool ok;

			/*
			 * Ensure we set sa_socklen correctly.
			 * Doesn't matter now, but eventually we
			 * will remove ip_service and return samba_sockaddr
			 * arrays directly.
			 */
			ok = sockaddr_storage_to_samba_sockaddr(
					&name_sa,
					&name_sa.u.ss);
			if (!ok) {
				status = NT_STATUS_INVALID_ADDRESS;
				goto out;
			}

			print_sockaddr(addr,
					sizeof(addr),
					&name_sa.u.ss);

			/* Check for and don't copy any known bad DC IP's. */
			if( !NT_STATUS_IS_OK(check_negative_conn_cache(domain,
							addr)) ) {
				DEBUG(5,("get_dc_list: negative entry %s "
					"removed from DC list\n",
					name ));
				continue;
			}

			return_iplist[local_count].ss = name_sa.u.ss;
			return_iplist[local_count].port = port;
			local_count++;
			*ordered = true;
		}
	}

	/* need to remove duplicates in the list if we have any
	   explicit password servers */

	local_count = remove_duplicate_addrs2(return_iplist, local_count );

	/* For DC's we always prioritize IPv4 due to W2K3 not
	 * supporting LDAP, KRB5 or CLDAP over IPv6. */

	if (local_count && return_iplist) {
		prioritize_ipv4_list(return_iplist, local_count);
	}

	if ( DEBUGLEVEL >= 4 ) {
		DEBUG(4,("get_dc_list: returning %zu ip addresses "
				"in an %sordered list\n",
				local_count,
				*ordered ? "":"un"));
		DEBUG(4,("get_dc_list: "));
		for ( i=0; i<local_count; i++ ) {
			char addr[INET6_ADDRSTRLEN];
			print_sockaddr(addr,
					sizeof(addr),
					&return_iplist[i].ss);
			DEBUGADD(4,("%s:%d ", addr, return_iplist[i].port ));
		}
		DEBUGADD(4,("\n"));
	}

	status = (local_count != 0 ? NT_STATUS_OK : NT_STATUS_NO_LOGON_SERVERS);

  out:

	if (NT_STATUS_IS_OK(status)) {
		*ip_list = return_iplist;
		*ret_count = local_count;
	} else {
		TALLOC_FREE(return_iplist);
	}

	TALLOC_FREE(auto_ip_list);
	TALLOC_FREE(frame);
	return status;
}

/*********************************************************************
 Talloc version.
 Small wrapper function to get the DC list and sort it if neccessary.
*********************************************************************/

NTSTATUS get_sorted_dc_list(TALLOC_CTX *ctx,
				const char *domain,
				const char *sitename,
				struct ip_service **ip_list_ret,
				size_t *ret_count,
				bool ads_only)
{
	bool ordered = false;
	NTSTATUS status;
	enum dc_lookup_type lookup_type = DC_NORMAL_LOOKUP;
	struct ip_service *ip_list = NULL;
	size_t count = 0;

	DBG_INFO("attempting lookup for name %s (sitename %s)\n",
		domain,
		sitename ? sitename : "NULL");

	if (ads_only) {
		lookup_type = DC_ADS_ONLY;
	}

	status = get_dc_list(ctx,
			domain,
			sitename,
			&ip_list,
			&count,
			lookup_type,
			&ordered);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_LOGON_SERVERS)
			&& sitename) {
		DBG_NOTICE("no server for name %s available"
			" in site %s, fallback to all servers\n",
			domain,
			sitename);
		status = get_dc_list(ctx,
				domain,
				NULL,
				&ip_list,
				&count,
				lookup_type,
				&ordered);
	}

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* only sort if we don't already have an ordered list */
	if (!ordered) {
		sort_service_list(ip_list, count);
	}

	*ret_count = count;
	*ip_list_ret = ip_list;
	return status;
}

/*********************************************************************
 Talloc version.
 Get the KDC list - re-use all the logic in get_dc_list.
*********************************************************************/

NTSTATUS get_kdc_list(TALLOC_CTX *ctx,
			const char *realm,
			const char *sitename,
			struct ip_service **ip_list_ret,
			size_t *ret_count)
{
	size_t count = 0;
	struct ip_service *ip_list = NULL;
	bool ordered = false;
	NTSTATUS status;

	status = get_dc_list(ctx,
			realm,
			sitename,
			&ip_list,
			&count,
			DC_KDC_ONLY,
			&ordered);

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* only sort if we don't already have an ordered list */
	if (!ordered ) {
		sort_service_list(ip_list, count);
	}

	*ret_count = count;
	*ip_list_ret = ip_list;
	return status;
}
