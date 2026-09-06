#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "crypto.h"

struct net {
	int fd;
	struct noise ns;
	char host[WIRE_MAX_HOST], port[6];
	uint8_t spk[32];                /* server static, pinned from the invite */
	uint8_t sk[32], pk[32];         /* our x25519 half */
};

static int wr_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = write(fd, b, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return NET_EIO;
		b += w;
		n -= (size_t)w;
	}
	return NET_OK;
}

static int rd_all(int fd, void *p, size_t n)
{
	uint8_t *b = p;
	while (n) {
		ssize_t r = read(fd, b, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return NET_EIO;
		b += r;
		n -= (size_t)r;
	}
	return NET_OK;
}

/* one length-prefixed message, plaintext: handshake only */
static int put_raw(int fd, const uint8_t *p, size_t n)
{
	uint8_t hdr[WIRE_HDR];
	wire_encode_len(hdr, n);
	if (wr_all(fd, hdr, sizeof hdr)) return NET_EIO;
	return wr_all(fd, p, n);
}

static int get_raw(int fd, uint8_t *p, size_t n)
{
	uint8_t hdr[WIRE_HDR];
	size_t len;
	if (rd_all(fd, hdr, sizeof hdr)) return NET_EIO;
	if (wire_decode_len(hdr, n, n, &len)) return NET_EHS;
	return rd_all(fd, p, n);
}

static int dial(struct net *n)
{
	struct addrinfo hints = { 0 }, *res, *a;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	n->fd = -1; /* before any early return: net_close would otherwise close fd 0 */
	if (getaddrinfo(n->host, n->port, &hints, &res)) return NET_ECONN;

	for (a = res; a; a = a->ai_next) {
		int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) continue;
		/* bounds connect and every later read: a silently dropped peer costs one NET_TIMEOUT,
		 * not a hung client. linux honours SO_SNDTIMEO for connect(2) */
		struct timeval tv = { NET_TIMEOUT, 0 };
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		if (!connect(fd, a->ai_addr, a->ai_addrlen)) { n->fd = fd; break; }
		close(fd);
	}
	freeaddrinfo(res);
	if (n->fd < 0) return NET_ECONN;
	setsockopt(n->fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));

	uint8_t m1[NOISE_MSG1], m2[NOISE_MSG2];
	int r = noise_client_hello(&n->ns, n->sk, n->pk, n->spk, m1);
	if (!r) r = put_raw(n->fd, m1, sizeof m1);
	if (!r) r = get_raw(n->fd, m2, sizeof m2);
	if (!r) r = noise_client_done(&n->ns, m2) ? NET_EHS : NET_OK;
	if (r) { close(n->fd); n->fd = -1; }
	return r;
}

int net_dial(struct net **out, const char *host, const char *port,
             const uint8_t server_pk[32], const struct identity *me)
{
	*out = NULL;
	struct net *n = calloc(1, sizeof *n);
	if (!n) return NET_EMEM;
	if (strlen(host) >= sizeof n->host || strlen(port) >= sizeof n->port) {
		free(n);
		return NET_ECONN;
	}
	strcpy(n->host, host);
	strcpy(n->port, port);
	memcpy(n->spk, server_pk, 32);
	memcpy(n->sk, me->xsk, 32);
	memcpy(n->pk, me->xpk, 32);

	int r = dial(n);
	if (r) { net_close(n); return r; }
	*out = n;
	return NET_OK;
}

void net_close(struct net *n)
{
	if (!n) return;
	if (n->fd >= 0) close(n->fd);
	wc_wipe(n, sizeof *n);
	free(n);
}

int net_send(struct net *n, const struct wire_frame *f)
{
	if (n->fd < 0) return NET_EIO;
	uint8_t *b = malloc(WIRE_HDR + NET_BUF);
	if (!b) return NET_EMEM;
	size_t len;
	int r = wire_encode_frame(b + 4, NET_MAX, &len, f) ? NET_EWIRE : NET_OK;
	if (!r) {
		noise_encrypt(&n->ns, b + WIRE_HDR, b + WIRE_HDR, len);
		wire_encode_len(b, len + NOISE_TAG);
		r = wr_all(n->fd, b, WIRE_HDR + len + NOISE_TAG);
	}
	free(b);
	return r;
}

int net_recv(struct net *n, struct wire_frame *f, uint8_t *buf, size_t cap)
{
	if (n->fd < 0) return NET_EIO;
	if (cap < NET_BUF) return NET_EMEM;
	uint8_t hdr[WIRE_HDR];
	size_t len;
	if (rd_all(n->fd, hdr, sizeof hdr)) return NET_EIO;
	if (wire_decode_len(hdr, NOISE_TAG, NET_BUF, &len)) return NET_EWIRE;
	if (rd_all(n->fd, buf, len)) return NET_EIO;
	if (noise_decrypt(&n->ns, buf, buf, len)) return NET_EWIRE;
	return wire_decode_frame(buf, len - NOISE_TAG, f) ? NET_EWIRE : NET_OK;
}
