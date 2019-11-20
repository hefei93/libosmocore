#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdbool.h>
#include <talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/select.h>
#include <osmocom/core/logging.h>
#include <osmocom/mslookup/mdns_sock.h>

/* returns 0 on success, -1 on error */
struct osmo_mdns_sock *osmo_mdns_sock_init(void *ctx, const char *ip, unsigned int port,
					   int (*cb)(struct osmo_fd *fd, unsigned int what),
					   void *data, unsigned int priv_nr)
{
	struct osmo_mdns_sock *ret;
	int sock, rc;
	struct addrinfo hints = {0};
	struct ip_mreq multicast_req = {0};
	in_addr_t iface = INADDR_ANY;
	char portbuf[10];
	int y = 1;

	snprintf(portbuf, sizeof(portbuf) -1, "%u", port);
	ret = talloc_zero(ctx, struct osmo_mdns_sock);
	OSMO_ASSERT(ret);

	/* Fill addrinfo */
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = (AI_PASSIVE | AI_NUMERICHOST);
	rc = getaddrinfo(ip, portbuf, &hints, &ret->ai);
	if (rc != 0) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: getaddrinfo: %s\n", gai_strerror(rc));
		ret->ai = NULL;
		goto error;
	}

	/* Open socket */
	sock = socket(ret->ai->ai_family, ret->ai->ai_socktype, 0);
	if (sock == -1) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: socket: %s\n", strerror(errno));
		goto error;
	}

	/* Set multicast options */
	rc = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (char*)&iface, sizeof(iface));
	if (rc == -1) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: setsockopt: %s\n", strerror(errno));
		goto error;
	}
	memcpy(&multicast_req.imr_multiaddr, &((struct sockaddr_in*)(ret->ai->ai_addr))->sin_addr,
	       sizeof(multicast_req.imr_multiaddr));
	multicast_req.imr_interface.s_addr = htonl(INADDR_ANY);
	rc = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&multicast_req, sizeof(multicast_req));
	if (rc == -1) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: setsockopt: %s\n", strerror(errno));
		goto error;
	}

	/* Always allow binding the same IP and port twice. This is needed in OsmoHLR (where the code becomes cleaner by
	 * just using a different socket for server and client code) and in the mslookup_client_mdns_test. Also for
	 * osmo-mslookup-client if it is running multiple times in parallel (i.e. two incoming calls almost at the same
	 * time need to be resolved with the simple dialplan example that just starts new processes). */
	rc = setsockopt(sock,SOL_SOCKET,SO_REUSEADDR, (char *)&y, sizeof(y));
	if (rc == -1) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: setsockopt: %s\n", strerror(errno));
		goto error;
	}

	/* Bind and register osmo_fd callback */
	rc = bind(sock, ret->ai->ai_addr, ret->ai->ai_addrlen);
	if (rc == -1) {
		LOGP(DLMSLOOKUP, LOGL_ERROR, "osmo_mdns_sock_init: bind: %s\n", strerror(errno));
		goto error;
	}
	osmo_fd_setup(&ret->osmo_fd, sock, OSMO_FD_READ, cb, data, priv_nr);
	if (osmo_fd_register(&ret->osmo_fd) != 0)
		goto error;

	return ret;
error:
	if (ret->ai)
		freeaddrinfo(ret->ai);
	talloc_free(ret);
	return NULL;
}

int osmo_mdns_sock_send(const struct osmo_mdns_sock *mdns_sock, struct msgb *msg)
{
	size_t len = msgb_length(msg);
	int rc = sendto(mdns_sock->osmo_fd.fd, msgb_data(msg), len, 0, mdns_sock->ai->ai_addr,
			mdns_sock->ai->ai_addrlen);
	msgb_free(msg);
	return (rc == len) ? 0 : -1;
}

void osmo_mdns_sock_cleanup(struct osmo_mdns_sock *mdns_sock)
{
	osmo_fd_close(&mdns_sock->osmo_fd);
	freeaddrinfo(mdns_sock->ai);
	talloc_free(mdns_sock);
}