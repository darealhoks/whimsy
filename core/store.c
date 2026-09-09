#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypto.h"

#define RECHDR (4 + WC_NONCE)
#define MAGIC  "whimsy"
#define MAGIC_N 6

/* header: magic[6] ver mode blocks[4] passes[4] lanes[4] salt[16] count[8] nonce[24] mac[16]
 * the mac seals an empty plaintext over the 44 bytes before it, so a wrong key
 * is a header failure and not a first-record failure. count is the sealed record
 * total: it is what makes a truncated tail detectable, and it is rewritten after
 * every append, so a store found with fewer records than this is corrupt. the nonce
 * is redrawn on every header write: the key is the same across them all */
#define HDR_COUNT 36
#define HDR_NONCE 44
#define HDR_MAC   68

enum { MODE_PASS = 1, MODE_KEYFILE = 2 };

struct srec { uint8_t *p; size_t n; uint8_t kind; };

struct store {
	int fd;
	uint8_t key[32];
	struct srec *rec;
	size_t nrec, cap;
	off_t cut;      /* >= 0: a tail that did not open, dropped before the next append */
	char *dir;      /* kept for store_rekey: the keyfile and the temp file live there */
	uint8_t hdr[STORE_HDR];
};

static uint32_t ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void st32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void st64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t ld64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static int path_of(char *out, size_t cap, const char *dir, const char *leaf)
{
	int n = snprintf(out, cap, "%s/%s", dir, leaf);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* reads the file whole; -1 on any error, *out NULL for a missing file */
static int read_all(const char *path, uint8_t **out, size_t *out_n, mode_t *mode)
{
	*out = NULL;
	*out_n = 0;
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return errno == ENOENT ? 0 : -1;
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) { close(fd); return -1; }
	if (mode) *mode = st.st_mode;
	size_t n = (size_t)st.st_size;
	uint8_t *b = malloc(n ? n : 1);
	if (!b) { close(fd); return -1; }
	for (size_t got = 0; got < n; ) {
		ssize_t r = read(fd, b + got, n - got);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) { free(b); close(fd); return -1; }
		got += (size_t)r;
	}
	close(fd);
	*out = b;
	*out_n = n;
	return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t w = write(fd, b, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return -1;
		b += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

static int keyfile(uint8_t key[32], const char *dir)
{
	char path[4096];
	if (path_of(path, sizeof path, dir, "key")) return STORE_EIO;
	uint8_t *b;
	size_t n;
	mode_t mode = 0;
	if (read_all(path, &b, &n, &mode)) return STORE_EIO;
	if (b) {
		int e = n == 32 && !(mode & 077) ? (memcpy(key, b, 32), STORE_OK) : STORE_EKEY;
		wc_wipe(b, n);
		free(b);
		return e;
	}
	wc_random(key, 32);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return STORE_EIO;
	int e = write_all(fd, key, 32) || fsync(fd) ? STORE_EIO : STORE_OK;
	close(fd);
	return e;
}

static int derive(uint8_t key[32], const uint8_t hdr[STORE_HDR], const char *dir, const char *pass)
{
	if (hdr[7] == MODE_KEYFILE) return pass ? STORE_EKEY : keyfile(key, dir);
	if (hdr[7] != MODE_PASS) return STORE_EFORMAT;
	if (!pass) return STORE_EKEY;
	uint32_t blocks = ld32(hdr + 8), passes = ld32(hdr + 12), lanes = ld32(hdr + 16);
	if (blocks < STORE_ARGON2_BLOCKS || passes < STORE_ARGON2_PASSES || lanes < STORE_ARGON2_LANES)
		return STORE_EFORMAT;
	if (blocks > STORE_ARGON2_MAX_BLOCKS || passes > STORE_ARGON2_MAX_PASSES ||
	    lanes > STORE_ARGON2_MAX_LANES || blocks < 8 * lanes)
		return STORE_EFORMAT; /* monocypher floors segment_size to 0 below 8 * lanes */
	return wc_argon2(key, pass, strlen(pass), hdr + 20, blocks, passes, lanes)
	       ? STORE_ENOMEM : STORE_OK;
}

static void hdr_mac(uint8_t mac[16], const uint8_t key[32], const uint8_t hdr[STORE_HDR])
{
	wc_seal(mac, key, hdr + HDR_NONCE, hdr, HDR_NONCE, NULL, 0);
}

/* a rename or a create is only durable once the directory entry is */
static void sync_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;
	fsync(fd);
	close(fd);
}

/* the record is durable before this runs, so a header left behind is survivable:
 * store_open takes count as a lower bound. losing it the other way is not */
static void sync_hdr(struct store *s)
{
	st64(s->hdr + HDR_COUNT, s->nrec);
	wc_random(s->hdr + HDR_NONCE, WC_NONCE);
	hdr_mac(s->hdr + HDR_MAC, s->key, s->hdr);
	for (size_t at = 0; at < STORE_HDR; ) {
		ssize_t w = pwrite(s->fd, s->hdr + at, STORE_HDR - at, (off_t)at);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return;
		at += (size_t)w;
	}
}

static int push(struct store *s, uint8_t kind, const uint8_t *p, size_t n)
{
	if (s->nrec == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 64;
		struct srec *r = realloc(s->rec, cap * sizeof *r);
		if (!r) return STORE_ENOMEM;
		s->rec = r;
		s->cap = cap;
	}
	uint8_t *copy = malloc(n ? n : 1);
	if (!copy) return STORE_ENOMEM;
	if (n) memcpy(copy, p, n);
	s->rec[s->nrec++] = (struct srec){ copy, n, kind };
	return STORE_OK;
}

/* seals rec i under key into buf, which must hold RECHDR + 1 + n + WC_MAC */
static void seal_rec(uint8_t *buf, const uint8_t key[32], uint64_t idx,
                     uint8_t kind, const uint8_t *p, size_t n)
{
	uint8_t ad[8], *pt = buf + RECHDR;
	st32(buf, (uint32_t)(1 + n + WC_MAC));
	wc_random(buf + 4, WC_NONCE);
	st64(ad, idx);
	/* plaintext is built in place at the ciphertext offset; wc_seal takes it from there */
	pt[0] = kind;
	if (n) memcpy(pt + 1, p, n);
	wc_seal(pt, key, buf + 4, ad, sizeof ad, pt, 1 + n);
}

/* stops at the first record that will not open; in an append-only file everything
 * behind it is unreachable anyway. *end is the byte offset of the good prefix. */
static int load(struct store *s, const uint8_t *b, size_t n, size_t *end)
{
	size_t off = STORE_HDR;
	while (off < n) {
		size_t left = n - off;
		if (left < RECHDR) break;
		uint32_t len = ld32(b + off);
		if (len < 1 + WC_MAC || len > 1 + STORE_MAX_REC + WC_MAC || left - RECHDR < len) break;
		uint8_t ad[8];
		st64(ad, s->nrec);
		uint8_t *pt = malloc(len - WC_MAC);
		if (!pt) return STORE_ENOMEM;
		int e = wc_open(pt, s->key, b + off + 4, ad, sizeof ad, b + off + RECHDR, len);
		if (e == 0) e = push(s, pt[0], pt + 1, len - WC_MAC - 1);
		wc_wipe(pt, len - WC_MAC);
		free(pt);
		if (e == STORE_ENOMEM) return e;
		if (e) break;
		off += RECHDR + len;
	}
	*end = off;
	return STORE_OK;
}

int store_open(struct store **out, const char *dir, const char *pass, size_t *bad)
{
	if (pass && !*pass) pass = NULL;        /* empty is no passphrase: fall to the keyfile */
	*out = NULL;
	if (mkdir(dir, 0700) && errno != EEXIST) return STORE_EIO;

	char path[4096];
	if (path_of(path, sizeof path, dir, "store")) return STORE_EIO;
	uint8_t *b;
	size_t n;
	if (read_all(path, &b, &n, NULL)) return STORE_EIO;
	/* a crash between the create and the header write: no store, not a broken one */
	if (b && !n) { free(b); b = NULL; }

	struct store *s = calloc(1, sizeof *s);
	if (!s) { free(b); return STORE_ENOMEM; }
	s->fd = -1;
	s->cut = -1;
	s->dir = strdup(dir);
	if (!s->dir) { free(b); store_close(s); return STORE_ENOMEM; }

	uint8_t *hdr = s->hdr;
	int e;
	if (!b) {
		memcpy(hdr, MAGIC, MAGIC_N);
		hdr[6] = STORE_VER;
		hdr[7] = pass ? MODE_PASS : MODE_KEYFILE;
		st32(hdr + 8, pass ? STORE_ARGON2_BLOCKS : 0);
		st32(hdr + 12, pass ? STORE_ARGON2_PASSES : 0);
		st32(hdr + 16, pass ? STORE_ARGON2_LANES : 0);
		wc_random(hdr + 20, 16);
		st64(hdr + HDR_COUNT, 0);
		wc_random(hdr + HDR_NONCE, WC_NONCE);
		e = derive(s->key, hdr, dir, pass);
		if (!e) {
			hdr_mac(hdr + HDR_MAC, s->key, hdr);
			int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
			if (fd < 0) e = STORE_EIO;
			else {
				e = write_all(fd, hdr, STORE_HDR) || fsync(fd) ? STORE_EIO : STORE_OK;
				close(fd);
				if (!e) sync_dir(dir);
			}
		}
	} else if (n < STORE_HDR || memcmp(b, MAGIC, MAGIC_N) || b[6] != STORE_VER) {
		e = STORE_EFORMAT;
	} else {
		memcpy(hdr, b, STORE_HDR);
		e = derive(s->key, hdr, dir, pass);
		if (!e) {
			uint8_t mac[16];
			hdr_mac(mac, s->key, hdr);
			if (!wc_equal(mac, hdr + HDR_MAC, 16)) e = STORE_EKEY;
		}
		size_t end = n;
		if (!e) e = load(s, b, n, &end);
		/* fewer than the header sealed: truncated, or a sealed record was edited.
		 * more is the one benign direction, an append whose header write was lost */
		if (!e && s->nrec < ld64(hdr + HDR_COUNT)) e = STORE_ECORRUPT;
		if (!e && end != n) {
			/* a torn tail, from a write that died mid-record. drop it, but never
			 * throw a whole store away: with nothing recovered this is not a tail */
			if (!s->nrec) e = STORE_ECORRUPT;
			else { s->cut = (off_t)end; if (bad) *bad = s->nrec; }
		}
	}
	if (b) { wc_wipe(b, n); free(b); }
	if (!e) {
		/* no O_APPEND: sync_hdr pwrites offset 0, which O_APPEND would send to the end */
		s->fd = open(path, O_WRONLY | O_NOFOLLOW);
		if (s->fd < 0) e = STORE_EIO;
	}
	if (e) { store_close(s); return e; }
	*out = s;
	return STORE_OK;
}

void store_close(struct store *s)
{
	if (!s) return;
	for (size_t i = 0; i < s->nrec; i++) {
		wc_wipe(s->rec[i].p, s->rec[i].n);
		free(s->rec[i].p);
	}
	free(s->rec);
	free(s->dir);
	if (s->fd >= 0) close(s->fd);
	wc_wipe(s, sizeof *s);
	free(s);
}

int store_append(struct store *s, uint8_t kind, const void *rec, size_t n)
{
	if (n > STORE_MAX_REC) return STORE_EBIG;
	/* not at open: a damaged store stays readable until something actually writes */
	if (s->cut >= 0) {
		if (ftruncate(s->fd, s->cut)) return STORE_EIO;
		s->cut = -1;
	}
	size_t len = 1 + n + WC_MAC;
	uint8_t *buf = malloc(RECHDR + len);
	if (!buf) return STORE_ENOMEM;
	/* in ram before on disk: the ad is the record index, so a failure that left
	 * nrec behind the file would seal the next record under an index already used */
	if (push(s, kind, rec, n)) { free(buf); return STORE_ENOMEM; }

	seal_rec(buf, s->key, s->nrec - 1, kind, rec, n);

	off_t at = lseek(s->fd, 0, SEEK_END);
	int e = at < 0 || write_all(s->fd, buf, RECHDR + len) || fsync(s->fd) ? STORE_EIO : STORE_OK;
	wc_wipe(buf, RECHDR + len);
	free(buf);
	if (!e) sync_hdr(s);
	if (e) {
		/* the stump must go before anything else is written: a cut that failed here
		 * is retried by the next append, which would otherwise seal past it */
		if (at >= 0 && ftruncate(s->fd, at)) s->cut = at;
		s->nrec--;
		wc_wipe(s->rec[s->nrec].p, s->rec[s->nrec].n);
		free(s->rec[s->nrec].p);
	}
	return e;
}

/* every record resealed from ram into <dir>/store.tmp, renamed over <dir>/store: a
 * crash before the rename leaves the old file and the old key working. on success the
 * store is the new file, under hdr and key. */
static int rewrite(struct store *s, const uint8_t hdr[STORE_HDR], const uint8_t key[32])
{
	char tmp[4096], path[4096];
	if (path_of(tmp, sizeof tmp, s->dir, "store.tmp") ||
	    path_of(path, sizeof path, s->dir, "store")) return STORE_EIO;

	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd < 0) return STORE_EIO;
	int e = write_all(fd, hdr, STORE_HDR) ? STORE_EIO : STORE_OK;
	for (size_t i = 0; !e && i < s->nrec; i++) {
		size_t len = RECHDR + 1 + s->rec[i].n + WC_MAC;
		uint8_t *buf = malloc(len);
		if (!buf) { e = STORE_ENOMEM; break; }
		seal_rec(buf, key, i, s->rec[i].kind, s->rec[i].p, s->rec[i].n);
		e = write_all(fd, buf, len) ? STORE_EIO : STORE_OK;
		wc_wipe(buf, len);
		free(buf);
	}
	if (!e && fsync(fd)) e = STORE_EIO;
	close(fd);
	/* the handle for the new store is taken before the rename, not after: an open that
	 * failed after it would leave the file swapped under a caller told nothing changed */
	int nfd = e ? -1 : open(tmp, O_WRONLY | O_NOFOLLOW);
	if (!e && nfd < 0) e = STORE_EIO;
	if (!e && rename(tmp, path)) { close(nfd); e = STORE_EIO; }
	if (e) { unlink(tmp); return e; }
	sync_dir(s->dir);

	close(s->fd);
	s->fd = nfd;
	memcpy(s->key, key, 32);
	memcpy(s->hdr, hdr, STORE_HDR);
	s->cut = -1;    /* a torn tail cannot survive a whole-file rewrite */
	return STORE_OK;
}

/* every record is already in ram, so this reseals from there and never re-reads the
 * old file. it is written whole to <dir>/store.tmp and renamed over <dir>/store: a
 * crash before the rename leaves the old store and the old password untouched. */
int store_rekey(struct store *s, const char *oldpass, const char *newpass)
{
	if (oldpass && !*oldpass) oldpass = NULL;
	if (newpass && !*newpass) newpass = NULL;
	uint8_t old[32];
	int e = derive(old, s->hdr, s->dir, oldpass);
	if (!e && !wc_equal(old, s->key, 32)) e = STORE_EKEY;
	wc_wipe(old, sizeof old);
	if (e) return e;

	uint8_t hdr[STORE_HDR], key[32];
	memcpy(hdr, MAGIC, MAGIC_N);
	hdr[6] = STORE_VER;
	hdr[7] = newpass ? MODE_PASS : MODE_KEYFILE;
	st32(hdr + 8,  newpass ? STORE_ARGON2_BLOCKS : 0);
	st32(hdr + 12, newpass ? STORE_ARGON2_PASSES : 0);
	st32(hdr + 16, newpass ? STORE_ARGON2_LANES : 0);
	wc_random(hdr + 20, 16);
	st64(hdr + HDR_COUNT, s->nrec);
	wc_random(hdr + HDR_NONCE, WC_NONCE);
	/* keyfile mode with no keyfile yet creates one; this is what makes the derive
	 * below reproducible on the next open */
	if ((e = derive(key, hdr, s->dir, newpass))) return e;
	hdr_mac(hdr + HDR_MAC, key, hdr);

	if ((e = rewrite(s, hdr, key))) { wc_wipe(key, sizeof key); return e; }
	wc_wipe(key, sizeof key);
	/* the keyfile is dead weight under a passphrase, and it is still a key */
	if (newpass) {
		char kf[4096];
		if (!path_of(kf, sizeof kf, s->dir, "key")) unlink(kf);
	}
	return STORE_OK;
}

/* ponytail: rewrites the whole file per call -- an edit is rare and the store is
 * already in ram, so a per-record hole-punch is not worth the format it would need */
int store_replace(struct store *s, size_t i, uint8_t kind, const void *rec, size_t n)
{
	if (i >= s->nrec) return STORE_EBIG;
	if (n > STORE_MAX_REC) return STORE_EBIG;
	uint8_t *copy = malloc(n ? n : 1);
	if (!copy) return STORE_ENOMEM;
	if (n) memcpy(copy, rec, n);

	struct srec was = s->rec[i];
	s->rec[i] = (struct srec){ copy, n, kind };
	uint8_t key[32];
	memcpy(key, s->key, 32);
	int e = rewrite(s, s->hdr, key);
	wc_wipe(key, sizeof key);
	if (e) { free(copy); s->rec[i] = was; return e; }
	wc_wipe(was.p, was.n);
	free(was.p);
	return STORE_OK;
}

/* ponytail: one rewrite for the batch, same as store_replace -- see store.h */
int store_void_many(struct store *s, const size_t *idx, size_t n)
{
	if (!n) return STORE_OK;
	uint8_t *seen = calloc(s->nrec ? s->nrec : 1, 1);
	struct srec *was = calloc(n, sizeof *was);
	size_t *at = calloc(n, sizeof *at), m = 0;
	int e = seen && was && at ? STORE_OK : STORE_ENOMEM;

	for (size_t k = 0; !e && k < n; k++) {
		if (idx[k] >= s->nrec) { e = STORE_EBIG; break; }
		if (seen[idx[k]]) continue;
		seen[idx[k]] = 1;
		uint8_t *body = malloc(1);
		if (!body) { e = STORE_ENOMEM; break; }
		at[m] = idx[k];
		was[m++] = s->rec[idx[k]];
		s->rec[idx[k]] = (struct srec){ body, 0, STORE_VOID };
	}
	if (!e) {
		uint8_t key[32];
		memcpy(key, s->key, 32);
		e = rewrite(s, s->hdr, key);
		wc_wipe(key, sizeof key);
	}
	for (size_t k = 0; k < m; k++) {
		if (e) { free(s->rec[at[k]].p); s->rec[at[k]] = was[k]; }
		else { wc_wipe(was[k].p, was[k].n); free(was[k].p); }
	}
	free(seen); free(was); free(at);
	return e;
}

size_t store_count(const struct store *s) { return s->nrec; }

const uint8_t *store_get(const struct store *s, size_t i, uint8_t *kind, size_t *n)
{
	if (i >= s->nrec) return NULL;
	if (kind) *kind = s->rec[i].kind;
	if (n) *n = s->rec[i].n;
	return s->rec[i].p;
}
