/* whimsyd: the relay. Sees ciphertext blobs, mailbox ids, sizes, timing and which
 * account sent what. Never group state, names or plaintext.
 *
 *     whimsyd serve  <datadir> [port]        default port 7717
 *     whimsyd invite <datadir> <host:port>   prints one single-use invite url
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "crypto.h"
#include "net.h"
#include "noise.h"
#include "wire.h"

#define PATHMAX     512
#define PJ(b, ...)  (snprintf(b, sizeof b, __VA_ARGS__) >= (int)sizeof b)
#define BLOB_TTL    (30 * 86400)
#define INVITE_TTL  (7 * 86400)
#define SWEEP_EVERY 60
#define BACKLOG     32
#define MAX_CONN    64
#define MAX_PER_IP  8
#define CONN_TTL    30          /* seconds a half-read frame or an unfinished handshake may sit */
#define IDLE_TTL    300         /* a live client pings well inside this */
#define BLOB_NAME_LEN 85        /* 20 digit seq + '.' + 64 hex putter account */
#define MAIL_MAX    1024        /* blobs one mailbox may hold before a PUT is refused */
#define MAIL_BYTES  (64u << 20)
#define OUT_MAX     (1u << 18)  /* a fetch stops filling c->out past this; the client refetches */
#define FETCH_BLOBS 256         /* blobs one fetch streams; must stay under FETCH_MAX in
                                 * core/whimsy.c, which drops the socket at it */
#define REQ_BURST   512         /* token bucket per connection */
#define REQ_RATE    256

static const char *datadir;
static uint8_t srv_sk[32], srv_pk[32];

struct conn {
	struct conn *next;      /* live list, walked by the sweep tick */
	time_t seen;
	int fd, hs, known, dead, armed, tok;
	time_t tok_t;
	uint8_t ip[16];
	struct noise ns;
	uint8_t mailbox[32];
	uint8_t hdr[WIRE_HDR];
	size_t hdr_n;
	uint8_t *in;
	size_t in_len, in_n;
	uint8_t *out;
	size_t out_n, out_i, out_cap;
};

static int ep;
static struct conn *conns;
static int nconn;

/* files */

static int write_file_ex(const char *path, const void *p, size_t n, mode_t mode, int excl)
{
	int fd = open(path, O_WRONLY | O_CREAT | (excl ? O_EXCL : O_TRUNC), mode);
	if (fd < 0) return -1;
	const uint8_t *b = p;
	while (n) {
		ssize_t w = write(fd, b, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) { close(fd); unlink(path); return -1; }
		b += w;
		n -= (size_t)w;
	}
	return close(fd) ? -1 : 0;
}

static int write_file(const char *path, const void *p, size_t n, mode_t mode)
{
	return write_file_ex(path, p, n, mode, 0);
}

/* -1 on any error, including a file that is not exactly n bytes */
static int read_exact(const char *path, void *p, size_t n)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return -1;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
	    (size_t)st.st_size != n) { close(fd); return -1; }
	uint8_t *b = p;
	while (n) {
		ssize_t r = read(fd, b, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) { close(fd); return -1; }
		b += r;
		n -= (size_t)r;
	}
	close(fd);
	return 0;
}

static int mkdirp(const char *p)
{
	return mkdir(p, 0700) && errno != EEXIST ? -1 : 0;
}

static int subdir(char *out, size_t cap, const char *leaf)
{
	if (snprintf(out, cap, "%s/%s", datadir, leaf) >= (int)cap) return -1;
	return mkdirp(out);
}

static int key_load(void)
{
	char p[PATHMAX];
	if (mkdirp(datadir) || PJ(p, "%s/key", datadir)) return -1;
	struct stat st;
	int exists = stat(p, &st) == 0;
	if (!exists && errno != ENOENT) {
		fprintf(stderr, "whimsyd: stat %s: %s\n", p, strerror(errno));
		return -1;
	}
	if (exists) {
		if (read_exact(p, srv_sk, 32)) {
			fprintf(stderr, "whimsyd: %s is unreadable or corrupt, refusing to overwrite\n", p);
			return -1;
		}
	} else {
		wc_x25519_keypair(srv_sk, srv_pk);
		if (write_file_ex(p, srv_sk, 32, 0600, 1)) {
			fprintf(stderr, "whimsyd: writing %s: %s\n", p, strerror(errno));
			return -1;
		}
	}
	wc_x25519_pk(srv_pk, srv_sk);
	return 0;
}

/* accounts/<client x25519 hex> holds the eddsa pk, which is also the mailbox id */
static int account_path(char *out, size_t cap, const uint8_t xpk[32])
{
	char hex[65];
	wire_hex(hex, xpk, 32);
	return snprintf(out, cap, "%s/accounts/%s", datadir, hex) >= (int)cap ? -1 : 0;
}

static int account_load(const uint8_t xpk[32], uint8_t mailbox[32])
{
	char p[PATHMAX];
	if (account_path(p, sizeof p, xpk)) return -1;
	return read_exact(p, mailbox, 32);
}

static int mailbox_dir(char *out, size_t cap, const uint8_t mb[32])
{
	char hex[65];
	wire_hex(hex, mb, 32);
	return snprintf(out, cap, "%s/mail/%s", datadir, hex) >= (int)cap ? -1 : 0;
}

/* microseconds, forced strictly increasing: the seq is also the file name */
static uint64_t next_seq(void)
{
	static uint64_t last;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t t = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
	if (t <= last) t = last + 1;
	last = t;
	return t;
}

/* connection i/o */

static int push(struct conn *c, const uint8_t *p, size_t n)
{
	if (c->out_i == c->out_n) c->out_i = c->out_n = 0;
	if (n > OUT_MAX || c->out_n > OUT_MAX + 4 + NET_BUF - n) { c->dead = 1; return -1; }
	if (c->out_n + n > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap : 4096;
		while (cap < c->out_n + n) cap *= 2;
		uint8_t *q = realloc(c->out, cap);
		if (!q) { c->dead = 1; return -1; }
		c->out = q;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_n, p, n);
	c->out_n += n;
	return 0;
}

static void refill(struct conn *c)
{
	time_t now = time(NULL);
	if (now == c->tok_t) return;
	long add = (long)(now - c->tok_t) * REQ_RATE;
	c->tok = add > REQ_BURST - c->tok ? REQ_BURST : c->tok + (int)add;
	c->tok_t = now;
}

/* stops reading while the peer is not draining its replies or is out of request tokens */
static void arm(struct conn *c)
{
	int ev = (c->out_n - c->out_i > OUT_MAX / 2 || c->tok <= 0 ? 0 : EPOLLIN) |
	         (c->out_i < c->out_n ? EPOLLOUT : 0);
	if (ev == c->armed) return;
	c->armed = ev;
	struct epoll_event e = { .events = (uint32_t)ev, .data.ptr = c };
	epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
}

static void flush(struct conn *c)
{
	while (c->out_i < c->out_n) {
		ssize_t w = write(c->fd, c->out + c->out_i, c->out_n - c->out_i);
		if (w < 0 && errno == EINTR) continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
		if (w <= 0) { c->dead = 1; return; }
		c->out_i += (size_t)w;
	}
	if (c->out_i == c->out_n) {
		c->out_i = c->out_n = 0;
		free(c->out);            /* an idle connection must not hold a fetch-sized buffer */
		c->out = NULL;
		c->out_cap = 0;
	}
	arm(c);
}

static void put_raw(struct conn *c, const uint8_t *p, size_t n)
{
	uint8_t hdr[WIRE_HDR];
	wire_encode_len(hdr, n);
	if (!push(c, hdr, sizeof hdr)) push(c, p, n);
}

static void put_frame(struct conn *c, const struct wire_frame *f)
{
	uint8_t *b = malloc(WIRE_HDR + NET_BUF);
	if (!b) { c->dead = 1; return; }
	size_t len;
	if (wire_encode_frame(b + WIRE_HDR, NET_MAX, &len, f)) { c->dead = 1; free(b); return; }
	noise_encrypt(&c->ns, b + WIRE_HDR, b + WIRE_HDR, len);
	wire_encode_len(b, len + NOISE_TAG);
	push(c, b, WIRE_HDR + len + NOISE_TAG);
	free(b);
}

static void put_simple(struct conn *c, uint8_t type)
{
	struct wire_frame f = { .type = type };
	put_frame(c, &f);
}

static void put_putok(struct conn *c, uint64_t seq)
{
	struct wire_frame f = { .type = WIRE_F_PUTOK, .putok = { seq } };
	put_frame(c, &f);
}

static void put_err(struct conn *c, uint8_t code)
{
	struct wire_frame f = { .type = WIRE_F_ERR, .err = { code } };
	put_frame(c, &f);
}

/* requests */

static void do_register(struct conn *c, const struct wire_frame *f)
{
	uint8_t xpk[32], msg[64];
	wc_x25519_from_sign_pk(xpk, f->reg.pk);
	if (!wc_equal(xpk, c->ns.rs, 32)) { put_err(c, WIRE_E_BADREQ); return; }
	memcpy(msg, f->reg.token, 32);
	memcpy(msg + 32, xpk, 32);
	if (!wc_verify(f->reg.sig, f->reg.pk, msg, sizeof msg)) { put_err(c, WIRE_E_BADREQ); return; }

	char hex[65], invp[PATHMAX], p[PATHMAX], mail[PATHMAX];
	wire_hex(hex, f->reg.token, 32);
	if (PJ(invp, "%s/invites/%s", datadir, hex)) { put_err(c, WIRE_E_INTERNAL); return; }
	struct stat st;
	if (stat(invp, &st) || time(NULL) - st.st_mtime > INVITE_TTL) {
		put_err(c, WIRE_E_BADTOKEN);
		return;
	}

	if (account_path(p, sizeof p, xpk) || write_file(p, f->reg.pk, 32, 0600) ||
	    mailbox_dir(mail, sizeof mail, f->reg.pk) || mkdirp(mail)) {
		put_err(c, WIRE_E_INTERNAL);
		return;
	}
	unlink(invp);  /* the account file now exists, so a dropped reply just retries into DONE */

	memcpy(c->mailbox, f->reg.pk, 32);
	c->known = 1;
	put_simple(c, WIRE_F_DONE);
}

/* 0 to accept, -1 to refuse. when the box is at either cap the sender holding the most
 * blobs loses its oldest, so no one sender can hold a mailbox against the others.
 * o(n) per put, and o(n) again per eviction; a fetch drains a mailbox */
static int mail_admit(const char *dir, size_t adding, const char *tag)
{
	for (int round = 0; round < 8; round++) {
		DIR *d = opendir(dir);
		if (!d) return 0;
		struct { char tag[65], oldest[96]; unsigned long n; } t[16] = { { { 0 }, { 0 }, 0 } };
		int nt = 0;
		unsigned long n = 0, bytes = adding;
		struct dirent *e;
		while ((e = readdir(d))) {
			char p[PATHMAX];
			struct stat st;
			if (e->d_name[0] == '.' || PJ(p, "%s/%s", dir, e->d_name) || stat(p, &st)) continue;
			n++;
			bytes += (unsigned long)st.st_size;
			/* names are BLOB_NAME_LEN long, "<20 digit seq>.<putter account hex>",
			 * so they sort oldest first and carry who may revoke */
			const char *who = strlen(e->d_name) == BLOB_NAME_LEN ? e->d_name + 21 : "";
			int i = 0;
			while (i < nt && strcmp(t[i].tag, who)) i++;
			if (i == nt) {
				if (nt == 16) continue;  /* ponytail: a 17th sender goes untallied */
				nt++;
				snprintf(t[i].tag, sizeof t[i].tag, "%s", who);
				snprintf(t[i].oldest, sizeof t[i].oldest, "%s", e->d_name);
			} else if (strcmp(e->d_name, t[i].oldest) < 0) {
				snprintf(t[i].oldest, sizeof t[i].oldest, "%s", e->d_name);
			}
			t[i].n++;
		}
		closedir(d);
		if (n < MAIL_MAX && bytes <= MAIL_BYTES) return 0;
		int top = 0;
		for (int i = 1; i < nt; i++)
			if (t[i].n > t[top].n) top = i;
		char p[PATHMAX];
		if (!nt || !strcmp(t[top].tag, tag) || PJ(p, "%s/%s", dir, t[top].oldest) ||
		    unlink(p))
			return -1;
	}
	return -1;
}

static void do_put(struct conn *c, const struct wire_frame *f)
{
	uint8_t xpk[32], mb[32];
	char dir[PATHMAX], p[PATHMAX];
	wc_x25519_from_sign_pk(xpk, f->put.mailbox);
	/* an unknown mailbox discards, and answers a seq drawn the same way a real put
	 * would: a fixed reply (0, or an error) is an account oracle */
	if (account_load(xpk, mb) || !wc_equal(mb, f->put.mailbox, 32)) {
		put_putok(c, next_seq());
		return;
	}
	if (mailbox_dir(dir, sizeof dir, f->put.mailbox) || mkdirp(dir)) {
		put_err(c, WIRE_E_INTERNAL);
		return;
	}
	char tag[65];
	wire_hex(tag, c->mailbox, 32);
	if (mail_admit(dir, f->put.blob_n, tag)) { put_err(c, WIRE_E_FULL); return; }
	uint64_t seq;
	for (;;) {
		seq = next_seq();
		if (PJ(p, "%s/%020llu.%s", dir, (unsigned long long)seq, tag)) {
			put_err(c, WIRE_E_INTERNAL);
			return;
		}
		if (!write_file_ex(p, f->put.blob, f->put.blob_n, 0600, 1)) break;
		if (errno != EEXIST) { put_err(c, WIRE_E_INTERNAL); return; }
		/* a backward clock jump across a restart can regenerate a live seq name */
	}
	put_putok(c, seq);
}

static int seq_name(const struct dirent *e) { return e->d_name[0] != '.'; }

static void do_fetch(struct conn *c)
{
	char dir[PATHMAX], p[PATHMAX];
	struct dirent **ents;
	if (mailbox_dir(dir, sizeof dir, c->mailbox)) { put_err(c, WIRE_E_INTERNAL); return; }
	/* names are zero padded, so alphasort is seq order */
	int n = scandir(dir, &ents, seq_name, alphasort);
	if (n < 0) { put_simple(c, WIRE_F_DONE); return; }

	int sent = 0;
	for (int i = 0; i < n && sent < FETCH_BLOBS; i++) {
		struct stat st;
		if (PJ(p, "%s/%s", dir, ents[i]->d_name) || stat(p, &st) ||
		    st.st_size <= 0 || st.st_size > WIRE_MAX_PAYLOAD - 8) continue;
		/* the whole encoded blob frame must fit; the rest waits for the next fetch */
		if (c->out_n + WIRE_HDR + WIRE_FRAME_HDR + 8 + (size_t)st.st_size + NOISE_TAG >
		    OUT_MAX) continue;
		uint8_t *b = malloc((size_t)st.st_size);
		if (b && !read_exact(p, b, (size_t)st.st_size)) {
			struct wire_frame f = { .type = WIRE_F_BLOB };
			f.blob.seq = strtoull(ents[i]->d_name, NULL, 10);
			f.blob.blob = b;
			f.blob.blob_n = (size_t)st.st_size;
			put_frame(c, &f);
			sent++;
		}
		free(b);
	}
	for (int i = 0; i < n; i++) free(ents[i]);
	free(ents);
	put_simple(c, WIRE_F_DONE);
}

static void do_ack(struct conn *c, const struct wire_frame *f)
{
	char dir[PATHMAX], p[PATHMAX], pre[24];
	if (mailbox_dir(dir, sizeof dir, c->mailbox) ||
	    PJ(pre, "%020llu.", (unsigned long long)f->ack.seq)) return;
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)))
		if (!strncmp(e->d_name, pre, 21) && !PJ(p, "%s/%s", dir, e->d_name)) unlink(p);
	closedir(d);
}

/* the putter unlinks its own blob from a mailbox it wrote to. the name carries the
 * account that put it, so a restart keeps the check; always DONE, since telling a
 * caller whether a seq exists or is someone else's is an oracle */
static void do_revoke(struct conn *c, const struct wire_frame *f)
{
	char dir[PATHMAX], p[PATHMAX], name[96], tag[65];
	wire_hex(tag, c->mailbox, 32);
	if (!mailbox_dir(dir, sizeof dir, f->revoke.mailbox) &&
	    !PJ(name, "%020llu.%s", (unsigned long long)f->revoke.seq, tag) &&
	    !PJ(p, "%s/%s", dir, name))
		unlink(p);
	put_simple(c, WIRE_F_DONE);
}

static void dispatch(struct conn *c, const struct wire_frame *f)
{
	if (f->type == WIRE_F_REGISTER) {
		if (!c->known) { do_register(c, f); return; }
		/* retry of a registration whose DONE was lost: same account, same claim -> DONE again */
		uint8_t xpk[32];
		wc_x25519_from_sign_pk(xpk, f->reg.pk);
		if (wc_equal(xpk, c->ns.rs, 32) && wc_equal(f->reg.pk, c->mailbox, 32))
			put_simple(c, WIRE_F_DONE);
		else
			put_err(c, WIRE_E_BADREQ);
		return;
	}
	if (!c->known) { put_err(c, WIRE_E_NOACCOUNT); return; }
	switch (f->type) {
	case WIRE_F_PUT:   do_put(c, f); break;
	case WIRE_F_FETCH: do_fetch(c); break;
	case WIRE_F_ACK:   do_ack(c, f); break;
	case WIRE_F_REVOKE: do_revoke(c, f); break;
	case WIRE_F_PING:  put_simple(c, WIRE_F_PONG); break;
	default:           put_err(c, WIRE_E_BADREQ); break;
	}
}

static void handle(struct conn *c)
{
	if (!c->hs) {
		uint8_t m2[NOISE_MSG2];
		if (c->in_len != NOISE_MSG1 ||
		    noise_server_hello(&c->ns, srv_sk, srv_pk, c->in) ||
		    noise_server_done(&c->ns, m2)) {
			c->dead = 1;
			return;
		}
		put_raw(c, m2, sizeof m2);
		c->hs = 1;
		c->known = account_load(c->ns.rs, c->mailbox) == 0;
		return;
	}
	if (noise_decrypt(&c->ns, c->in, c->in, c->in_len)) { c->dead = 1; return; }
	struct wire_frame f;
	c->tok--;                /* a malformed frame costs a token too */
	if (wire_decode_frame(c->in, c->in_len - NOISE_TAG, &f)) put_err(c, WIRE_E_BADREQ);
	else dispatch(c, &f);
}

static void readable(struct conn *c)
{
	if (c->tok <= 0) { arm(c); return; }
	if (c->hdr_n < WIRE_HDR) {
		ssize_t r = read(c->fd, c->hdr + c->hdr_n, WIRE_HDR - c->hdr_n);
		if (r <= 0) { if (r == 0 || (errno != EAGAIN && errno != EINTR)) c->dead = 1; return; }
		c->hdr_n += (size_t)r;
		if (c->hdr_n < WIRE_HDR) return;
		size_t len;
		if (wire_decode_len(c->hdr, c->hs ? NOISE_TAG : NOISE_MSG1,
		                    c->hs ? NET_BUF : NOISE_MSG1, &len)) { c->dead = 1; return; }
		c->in = malloc(len);
		if (!c->in) { c->dead = 1; return; }
		c->in_len = len;
		c->in_n = 0;
	}
	ssize_t r = read(c->fd, c->in + c->in_n, c->in_len - c->in_n);
	if (r <= 0) { if (r == 0 || (errno != EAGAIN && errno != EINTR)) c->dead = 1; return; }
	c->in_n += (size_t)r;
	if (c->in_n < c->in_len) return;

	c->seen = time(NULL);
	handle(c);
	free(c->in);
	c->in = NULL;
	c->hdr_n = 0;
	if (!c->dead) flush(c);
}

static void drop(struct conn *c)
{
	for (struct conn **p = &conns; *p; p = &(*p)->next)
		if (*p == c) { *p = c->next; break; }
	nconn--;
	epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	free(c->in);
	free(c->out);
	noise_wipe(&c->ns);
	free(c);
}

/* ttl sweep */

static void sweep_files(const char *dir, time_t cutoff)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	char p[PATHMAX];
	struct stat st;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (PJ(p, "%s/%s", dir, e->d_name)) continue;
		if (!stat(p, &st) && S_ISREG(st.st_mode) && st.st_mtime < cutoff) unlink(p);
	}
	closedir(d);
}

static void sweep(void)
{
	time_t now = time(NULL);
	char p[PATHMAX];
	if (!PJ(p, "%s/invites", datadir)) sweep_files(p, now - INVITE_TTL);
	if (PJ(p, "%s/mail", datadir)) return;
	DIR *d = opendir(p);
	if (!d) return;
	struct dirent *e;
	char box[PATHMAX];
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (PJ(box, "%s/mail/%s", datadir, e->d_name)) continue;
		sweep_files(box, now - BLOB_TTL);
	}
	closedir(d);
}

static void ip_of(uint8_t out[16], const struct sockaddr_storage *ss)
{
	memset(out, 0, 16);
	if (ss->ss_family == AF_INET6)
		memcpy(out, &((const struct sockaddr_in6 *)ss)->sin6_addr, 16);
	else if (ss->ss_family == AF_INET)
		memcpy(out + 12, &((const struct sockaddr_in *)ss)->sin_addr, 4);
}

static int ip_conns(const uint8_t ip[16])
{
	int n = 0;
	for (struct conn *c = conns; c; c = c->next) n += !memcmp(c->ip, ip, 16);
	return n;
}

/* commands */

static int listen_on(const char *port)
{
	struct addrinfo hints = { 0 }, *res, *a;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(NULL, port, &hints, &res)) return -1;
	int fd = -1;
	for (a = res; a; a = a->ai_next) {
		fd = socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK, a->ai_protocol);
		if (fd < 0) continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int));
		if (a->ai_family == AF_INET6) /* one socket for both families */
			setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &(int){ 0 }, sizeof(int));
		if (!bind(fd, a->ai_addr, a->ai_addrlen) && !listen(fd, BACKLOG)) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int serve(const char *port)
{
	char p[PATHMAX];
	if (subdir(p, sizeof p, "mail") || subdir(p, sizeof p, "invites") ||
	    subdir(p, sizeof p, "accounts")) {
		fprintf(stderr, "whimsyd: cannot create %s\n", p);
		return 1;
	}
	int lfd = listen_on(port);
	if (lfd < 0) {
		fprintf(stderr, "whimsyd: cannot listen on %s\n", port);
		return 1;
	}
	ep = epoll_create1(0);
	struct epoll_event e = { .events = EPOLLIN, .data.ptr = NULL };
	if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &e)) return 1;
	printf("whimsyd: listening on %s\n", port);
	fflush(stdout);

	sweep();
	time_t next = time(NULL) + SWEEP_EVERY;
	for (;;) {
		struct epoll_event ev[64];
		int n = epoll_wait(ep, ev, 64, 1000);
		for (int i = 0; i < n; i++) {
			struct conn *c = ev[i].data.ptr;
			if (!c) {
				struct sockaddr_storage ss;
				socklen_t sl = sizeof ss;
				uint8_t ip[16];
				int fd = accept(lfd, (struct sockaddr *)&ss, &sl);
				if (fd < 0) continue;
				ip_of(ip, &ss);
				if (nconn >= MAX_CONN || ip_conns(ip) >= MAX_PER_IP) {
					close(fd);
					continue;
				}
				fcntl(fd, F_SETFL, O_NONBLOCK);
				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));
				c = calloc(1, sizeof *c);
				if (!c) { close(fd); continue; }
				c->fd = fd;
				c->seen = c->tok_t = time(NULL);
				c->tok = REQ_BURST;
				c->armed = EPOLLIN;
				memcpy(c->ip, ip, 16);
				struct epoll_event ce = { .events = EPOLLIN, .data.ptr = c };
				if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ce)) { close(fd); free(c); continue; }
				c->next = conns;
				conns = c;
				nconn++;
				continue;
			}
			if (ev[i].events & EPOLLOUT) flush(c);
			if (!c->dead && (ev[i].events & EPOLLIN)) readable(c);
			if (!c->dead && (ev[i].events & (EPOLLHUP | EPOLLERR))) c->dead = 1;
			if (c->dead) drop(c);
		}
		time_t now = time(NULL);
		for (struct conn *c = conns, *nx; c; c = nx) {
			nx = c->next;
			refill(c);
			arm(c);
			if (now - c->seen > (!c->hs || c->hdr_n || c->in ? CONN_TTL : IDLE_TTL)) drop(c);
		}
		if (now >= next) {
			sweep();
			next = time(NULL) + SWEEP_EVERY;
		}
	}
}

static int invite(const char *hostport)
{
	struct wire_invite v = { 0 };
	const char *colon = strrchr(hostport, ':');
	if (!colon || colon == hostport || (size_t)(colon - hostport) >= sizeof v.host ||
	    strlen(colon + 1) >= sizeof v.port) {
		fprintf(stderr, "whimsyd: expected host:port\n");
		return 1;
	}
	memcpy(v.host, hostport, (size_t)(colon - hostport));
	strcpy(v.port, colon + 1);
	memcpy(v.spk, srv_pk, 32);
	wc_random(v.token, 32);

	char p[PATHMAX], hex[65], url[WIRE_INVITE_MAX];
	wire_hex(hex, v.token, 32);
	if (subdir(p, sizeof p, "invites") || PJ(p, "%s/invites/%s", datadir, hex) ||
	    write_file(p, "", 0, 0600) || wire_encode_invite(url, sizeof url, &v)) {
		fprintf(stderr, "whimsyd: cannot write the invite\n");
		return 1;
	}
	printf("%s\n", url);
	return 0;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	setrlimit(RLIMIT_CORE, &(struct rlimit){ 0, 0 });
	prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
	if (argc < 3) {
		fprintf(stderr, "usage: whimsyd serve <datadir> [port]\n"
		                "       whimsyd invite <datadir> <host:port>\n");
		return 2;
	}
	datadir = argv[2];
	if (key_load()) {
		fprintf(stderr, "whimsyd: cannot open %s\n", datadir);
		return 1;
	}
	if (!strcmp(argv[1], "serve")) return serve(argc > 3 ? argv[3] : "7717");
	if (!strcmp(argv[1], "invite") && argc > 3) return invite(argv[3]);
	fprintf(stderr, "whimsyd: unknown command %s\n", argv[1]);
	return 2;
}
