#include "wire.h"

#include <stdio.h>
#include <string.h>

#define MAC 16 /* aead tag, must match WC_MAC in crypto.h */
#define SIG 64

typedef struct { const uint8_t *p; size_t n, i; int bad; } rd;
typedef struct { uint8_t *p; size_t n, i; int bad; } wr;

static const uint8_t *rb(rd *r, size_t k)
{
	if (r->bad || k > r->n - r->i) { r->bad = 1; return NULL; }
	const uint8_t *q = r->p + r->i;
	r->i += k;
	return q;
}
static uint8_t r8(rd *r) { const uint8_t *q = rb(r, 1); return q ? q[0] : 0; }
static uint16_t r16(rd *r)
{
	const uint8_t *q = rb(r, 2);
	return q ? (uint16_t)(q[0] | (unsigned)q[1] << 8) : 0;
}
static uint32_t r32(rd *r)
{
	const uint8_t *q = rb(r, 4);
	if (!q) return 0;
	return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}
static uint64_t r64(rd *r)
{
	const uint8_t *q = rb(r, 8);
	if (!q) return 0;
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = v << 8 | q[i];
	return v;
}
static int rdone(const rd *r)
{
	if (r->bad) return WIRE_ETRUNC;
	return r->i == r->n ? WIRE_OK : WIRE_EJUNK;
}

static uint8_t *wb(wr *w, size_t k)
{
	if (w->bad || k > w->n - w->i) { w->bad = 1; return NULL; }
	uint8_t *q = w->p + w->i;
	w->i += k;
	return q;
}
static void wraw(wr *w, const uint8_t *src, size_t k)
{
	uint8_t *q = wb(w, k);
	if (!q) return;
	if (src) memcpy(q, src, k);
	else memset(q, 0, k); /* unsigned field the caller fills later, i.e. a signature */
}
static void w8(wr *w, uint8_t v) { uint8_t *q = wb(w, 1); if (q) q[0] = v; }
static void w16(wr *w, uint16_t v) { uint8_t *q = wb(w, 2); if (q) { q[0] = v; q[1] = v >> 8; } }
static void w32(wr *w, uint32_t v)
{
	uint8_t *q = wb(w, 4);
	if (q) for (int i = 0; i < 4; i++) q[i] = (uint8_t)(v >> (8 * i));
}
static void w64(wr *w, uint64_t v)
{
	uint8_t *q = wb(w, 8);
	if (q) for (int i = 0; i < 8; i++) q[i] = (uint8_t)(v >> (8 * i));
}
static int wdone(const wr *w, size_t *len)
{
	if (w->bad) return WIRE_ESPACE;
	*len = w->i;
	return WIRE_OK;
}

/* length prefix */

void wire_encode_len(uint8_t out[WIRE_HDR], size_t n)
{
	wr w = { out, WIRE_HDR, 0, 0 };
	w32(&w, (uint32_t)n);
}

int wire_decode_len(const uint8_t b[WIRE_HDR], size_t min, size_t max, size_t *n)
{
	rd r = { b, WIRE_HDR, 0, 0 };
	uint32_t v = r32(&r);
	if (v < min || v > max) return WIRE_ELEN;
	*n = v;
	return WIRE_OK;
}

/* frame */

int wire_encode_frame(uint8_t *out, size_t cap, size_t *len, const struct wire_frame *f)
{
	wr w = { out, cap, 0, 0 };
	w8(&w, WIRE_VER);
	w8(&w, f->type);
	w32(&w, 0); /* backfilled */
	switch (f->type) {
	case WIRE_F_REGISTER:
		wraw(&w, f->reg.token, 32);
		wraw(&w, f->reg.pk, 32);
		wraw(&w, f->reg.sig, SIG);
		break;
	case WIRE_F_PUT:
		if (!f->put.blob_n) return WIRE_ELEN;
		wraw(&w, f->put.mailbox, 32);
		wraw(&w, f->put.blob, f->put.blob_n);
		break;
	case WIRE_F_BLOB:
		if (!f->blob.blob_n) return WIRE_ELEN;
		w64(&w, f->blob.seq);
		wraw(&w, f->blob.blob, f->blob.blob_n);
		break;
	case WIRE_F_ACK:
		w64(&w, f->ack.seq);
		break;
	case WIRE_F_PUTOK:
		w64(&w, f->putok.seq);
		break;
	case WIRE_F_REVOKE:
		wraw(&w, f->revoke.mailbox, 32);
		w64(&w, f->revoke.seq);
		break;
	case WIRE_F_ERR:
		w8(&w, f->err.code);
		break;
	case WIRE_F_FETCH: case WIRE_F_DONE: case WIRE_F_PING: case WIRE_F_PONG:
		break;
	default:
		return WIRE_ETYPE;
	}
	if (w.bad) return WIRE_ESPACE;
	size_t payload = w.i - WIRE_FRAME_HDR;
	if (payload > WIRE_MAX_PAYLOAD) return WIRE_ELEN;
	for (int i = 0; i < 4; i++) out[2 + i] = (uint8_t)(payload >> (8 * i));
	return wdone(&w, len);
}

int wire_decode_frame(const uint8_t *b, size_t n, struct wire_frame *f)
{
	rd r = { b, n, 0, 0 };
	memset(f, 0, sizeof *f);
	uint8_t ver = r8(&r);
	f->type = r8(&r);
	uint32_t len = r32(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (ver != WIRE_VER) return WIRE_EVER;
	if (len > WIRE_MAX_PAYLOAD) return WIRE_ELEN;
	if (len > n - r.i) return WIRE_ETRUNC;
	if (len < n - r.i) return WIRE_EJUNK;

	switch (f->type) {
	case WIRE_F_REGISTER:
		f->reg.token = rb(&r, 32);
		f->reg.pk = rb(&r, 32);
		f->reg.sig = rb(&r, SIG);
		break;
	case WIRE_F_PUT:
		f->put.mailbox = rb(&r, 32);
		if (r.bad || r.i == r.n) return WIRE_ETRUNC;
		f->put.blob_n = r.n - r.i;
		f->put.blob = rb(&r, f->put.blob_n);
		break;
	case WIRE_F_BLOB:
		f->blob.seq = r64(&r);
		if (r.bad || r.i == r.n) return WIRE_ETRUNC;
		f->blob.blob_n = r.n - r.i;
		f->blob.blob = rb(&r, f->blob.blob_n);
		break;
	case WIRE_F_ACK:
		f->ack.seq = r64(&r);
		break;
	case WIRE_F_PUTOK:
		f->putok.seq = r64(&r);
		break;
	case WIRE_F_REVOKE:
		f->revoke.mailbox = rb(&r, 32);
		f->revoke.seq = r64(&r);
		break;
	case WIRE_F_ERR:
		f->err.code = r8(&r);
		if (!r.bad && (f->err.code < WIRE_E_BADREQ || f->err.code > WIRE_E_FULL)) return WIRE_ELEN;
		break;
	case WIRE_F_FETCH: case WIRE_F_DONE: case WIRE_F_PING: case WIRE_F_PONG:
		break;
	default:
		return WIRE_ETYPE;
	}
	return rdone(&r);
}

/* blob */

int wire_encode_blob(uint8_t *out, size_t cap, size_t *len, const struct wire_blob *b)
{
	wr w = { out, cap, 0, 0 };
	if (b->ct_n < MAC) return WIRE_ELEN;
	w8(&w, WIRE_VER);
	wraw(&w, b->hdr, 32);
	w32(&w, b->aux);
	wraw(&w, b->nonce, 24);
	wraw(&w, b->ct, b->ct_n);
	return wdone(&w, len);
}

int wire_decode_blob(const uint8_t *b, size_t n, struct wire_blob *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	uint8_t ver = r8(&r);
	o->hdr = rb(&r, 32);
	o->aux = r32(&r);
	o->nonce = rb(&r, 24);
	if (r.bad) return WIRE_ETRUNC;
	if (ver != WIRE_VER) return WIRE_EVER;
	if (r.n - r.i < MAC) return WIRE_ETRUNC;
	o->ct_n = r.n - r.i;
	o->ct = rb(&r, o->ct_n);
	return rdone(&r);
}

/* padding: a GROUP and a SEALED plaintext both land on this ladder, so a blob's
 * size says neither which kind it is nor how long the message was */

static const size_t pads[] = { 256, 1024, 4096, 16384, 65536 };

size_t wire_pad(size_t need)
{
	for (size_t i = 0; i < sizeof pads / sizeof *pads; i++)
		if (need <= pads[i]) return pads[i];
	return 0;
}

/* the pad runs from the end of the variable part to the signature, so the signature
 * covers it; the decoder demands zeros and the smallest pad that fits */
static void wpad(wr *w, size_t upto)
{
	if (w->bad || upto < w->i) { w->bad = 1; return; }
	wraw(w, NULL, upto - w->i);
}

static int rpad(rd *r, size_t upto)
{
	if (r->bad || upto < r->i || upto > r->n) return WIRE_ETRUNC;
	for (; r->i < upto; r->i++) if (r->p[r->i]) return WIRE_EJUNK;
	return WIRE_OK;
}

/* message body: no padding of its own, the plaintext around it carries that */

/* a reply id is optional on TEXT, required on EDIT, DELETE and REACT -- it is what
 * they name -- and forbidden on the rest */
static int body_reply_ok(uint8_t kind, int has)
{
	if (kind == WIRE_K_EDIT || kind == WIRE_K_DELETE || kind == WIRE_K_REACT) return has;
	return kind == WIRE_K_TEXT || !has;
}

static int empty_body(uint8_t kind)
{
	return kind == WIRE_K_DELETE || kind == WIRE_K_TYPING || kind == WIRE_K_HEAL;
}

static int body_len_ok(uint8_t kind, size_t n)
{
	if (kind == WIRE_K_REACT) return n <= WIRE_MAX_REACT;
	if (kind == WIRE_K_GAVATAR) return n <= WIRE_MAX_AVATAR;
	if (kind == WIRE_K_PURGE) return n == 8;
	return 1;
}

int wire_encode_body(uint8_t *out, size_t cap, size_t *len, const struct wire_body *b)
{
	if (b->kind < WIRE_K_TEXT || b->kind > WIRE_K_UNLINK) return WIRE_ETYPE;
	if (!body_reply_ok(b->kind, b->reply != NULL)) return WIRE_ETYPE;
	if (empty_body(b->kind) && b->payload_n) return WIRE_ETYPE;
	if (!body_len_ok(b->kind, b->payload_n)) return WIRE_ELEN;
	if (b->payload_n > WIRE_MAX_BODY - WIRE_BODY_HDR - WIRE_MSGID) return WIRE_ELEN;
	wr w = { out, cap, 0, 0 };
	w8(&w, b->kind);
	w16(&w, b->channel);
	w64(&w, b->time);
	w8(&w, b->reply ? 1 : 0);
	if (b->reply) wraw(&w, b->reply, WIRE_MSGID);
	w32(&w, (uint32_t)b->payload_n);
	wraw(&w, b->payload, b->payload_n);
	return wdone(&w, len);
}

int wire_decode_body(const uint8_t *b, size_t n, struct wire_body *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	o->kind = r8(&r);
	o->channel = r16(&r);
	o->time = r64(&r);
	uint8_t has_reply = r8(&r);
	if (has_reply > 1) return WIRE_EJUNK;
	if (has_reply) o->reply = rb(&r, WIRE_MSGID);
	uint32_t plen = r32(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (o->kind < WIRE_K_TEXT || o->kind > WIRE_K_UNLINK) return WIRE_ETYPE;
	if (!body_reply_ok(o->kind, o->reply != NULL)) return WIRE_ETYPE;
	if (empty_body(o->kind) && plen) return WIRE_ETYPE;
	if (!body_len_ok(o->kind, plen)) return WIRE_ELEN;
	if (plen != n - r.i) return WIRE_ELEN;
	o->payload_n = plen;
	o->payload = rb(&r, plen);
	return rdone(&r);
}

/* file chunk, the payload of a FILE body */

_Static_assert(WIRE_GROUP_PT_HDR + WIRE_BODY_HDR + WIRE_FILE_HDR + 255 + WIRE_FILE_CHUNK <= 65536,
               "a file chunk must fit the 64k pad rung");

static int file_ok(const struct wire_file *f)
{
	if (!f->total || f->total > WIRE_FILE_CHUNKS || f->idx >= f->total) return WIRE_ELEN;
	if (!f->name_n) return WIRE_ELEN;
	if (f->bytes_n > WIRE_FILE_CHUNK) return WIRE_ELEN;
	/* only the last chunk may be short, which is what makes idx * WIRE_FILE_CHUNK the
	 * offset of every chunk without waiting for the ones before it */
	if (f->idx + 1 < f->total && f->bytes_n != WIRE_FILE_CHUNK) return WIRE_ELEN;
	if (f->idx + 1 == f->total && !f->bytes_n) return WIRE_ELEN;
	return WIRE_OK;
}

int wire_encode_file(uint8_t *out, size_t cap, size_t *len, const struct wire_file *f)
{
	int e = file_ok(f);
	if (e) return e;
	wr w = { out, cap, 0, 0 };
	wraw(&w, f->fid, 16);
	w32(&w, f->idx);
	w32(&w, f->total);
	w8(&w, f->name_n);
	wraw(&w, f->name, f->name_n);
	wraw(&w, f->bytes, f->bytes_n);
	return wdone(&w, len);
}

int wire_decode_file(const uint8_t *b, size_t n, struct wire_file *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	o->fid = rb(&r, 16);
	o->idx = r32(&r);
	o->total = r32(&r);
	o->name_n = r8(&r);
	o->name = rb(&r, o->name_n);
	if (r.bad) return WIRE_ETRUNC;
	o->bytes_n = n - r.i;
	o->bytes = rb(&r, o->bytes_n);
	int e = file_ok(o);
	return e ? e : rdone(&r);
}

/* membership record */

int wire_encode_rec(uint8_t *out, size_t cap, size_t *len, const struct wire_rec *rec)
{
	wr w = { out, cap, 0, 0 };
	if (rec->name_n > 255 || rec->nchan > WIRE_MAX_CHANNELS) return WIRE_ELEN;
	wraw(&w, rec->salt, 16);
	w32(&w, rec->version);
	wraw(&w, rec->owner, 32);
	w8(&w, (uint8_t)rec->name_n);
	wraw(&w, rec->name, rec->name_n);
	w8(&w, rec->nchan);
	for (uint8_t i = 0; i < rec->nchan; i++) {
		w16(&w, rec->chan[i].id);
		w8(&w, rec->chan[i].n);
		wraw(&w, rec->chan[i].p, rec->chan[i].n);
	}
	w16(&w, rec->nmemb);
	wraw(&w, rec->members, (size_t)rec->nmemb * 32);
	wraw(&w, rec->sig, SIG);
	return wdone(&w, len);
}

int wire_decode_rec(const uint8_t *b, size_t n, struct wire_rec *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	o->salt = rb(&r, 16);
	o->version = r32(&r);
	o->owner = rb(&r, 32);
	o->name_n = r8(&r);
	o->name = rb(&r, o->name_n);
	o->nchan = r8(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (o->nchan > WIRE_MAX_CHANNELS) return WIRE_ELEN;
	for (uint8_t i = 0; i < o->nchan; i++) {
		o->chan[i].id = r16(&r);
		o->chan[i].n = r8(&r);
		o->chan[i].p = rb(&r, o->chan[i].n);
	}
	o->nmemb = r16(&r);
	if (o->nmemb > WIRE_MAX_MEMBERS) return WIRE_ELEN;
	o->members = rb(&r, (size_t)o->nmemb * 32);
	o->sig = rb(&r, SIG);
	if (r.bad) return WIRE_ETRUNC;
	o->signed_from = b;
	o->signed_n = n - SIG;
	return rdone(&r);
}

/* group plaintext */

int wire_encode_group_pt(uint8_t *out, size_t cap, size_t *len, const struct wire_group_pt *p)
{
	if (!p->body_n) return WIRE_ELEN;
	size_t total = wire_pad(WIRE_GROUP_PT_HDR + p->body_n);
	if (!total) return WIRE_ELEN;
	if (cap < total) return WIRE_ESPACE;
	wr w = { out, total, 0, 0 };
	w8(&w, WIRE_B_GROUP);
	wraw(&w, p->cid, 32);
	w32(&w, p->index);
	wraw(&w, p->sender, 32);
	w32(&w, (uint32_t)p->body_n);
	wraw(&w, p->body, p->body_n);
	wpad(&w, total - SIG);
	wraw(&w, p->sig, SIG);
	return wdone(&w, len);
}

int wire_decode_group_pt(const uint8_t *b, size_t n, struct wire_group_pt *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	uint8_t kind = r8(&r);
	o->cid = rb(&r, 32);
	o->index = r32(&r);
	o->sender = rb(&r, 32);
	uint32_t blen = r32(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (kind != WIRE_B_GROUP) return WIRE_ETYPE;
	if (!blen || n != wire_pad(WIRE_GROUP_PT_HDR + (size_t)blen)) return WIRE_ELEN;
	o->body = rb(&r, blen);
	int e = rpad(&r, n - SIG);
	if (e) return e;
	o->sig = rb(&r, SIG);
	if (r.bad) return WIRE_ETRUNC;
	o->body_n = blen;
	o->signed_from = b;
	o->signed_n = n - SIG;
	return rdone(&r);
}

/* sealed plaintext and its inner */

int wire_encode_sealed_body(uint8_t *out, size_t cap, size_t *len, const struct wire_sealed_body *s)
{
	if (!s->inner_n) return WIRE_ELEN;
	size_t total = wire_pad(WIRE_SEAL_PT_HDR + s->inner_n);
	if (!total) return WIRE_ELEN;
	if (cap < total) return WIRE_ESPACE;
	wr w = { out, total, 0, 0 };
	w8(&w, WIRE_B_SEALED);
	wraw(&w, s->sender, 32);
	wraw(&w, s->to, 32);
	w32(&w, (uint32_t)s->inner_n);
	wraw(&w, s->inner, s->inner_n);
	wpad(&w, total - SIG);
	wraw(&w, s->sig, SIG);
	return wdone(&w, len);
}

int wire_decode_sealed_body(const uint8_t *b, size_t n, struct wire_sealed_body *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	uint8_t kind = r8(&r);
	o->sender = rb(&r, 32);
	o->to = rb(&r, 32);
	uint32_t ilen = r32(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (kind != WIRE_B_SEALED) return WIRE_ETYPE;
	if (!ilen || n != wire_pad(WIRE_SEAL_PT_HDR + (size_t)ilen)) return WIRE_ELEN;
	o->inner = rb(&r, ilen);
	int e = rpad(&r, n - SIG);
	if (e) return e;
	o->sig = rb(&r, SIG);
	if (r.bad) return WIRE_ETRUNC;
	o->inner_n = ilen;
	o->signed_from = b;
	o->signed_n = n - SIG;
	return rdone(&r);
}

int wire_encode_senderkey(uint8_t *out, size_t cap, size_t *len, const struct wire_senderkey *s)
{
	wr w = { out, cap, 0, 0 };
	w8(&w, WIRE_I_SENDERKEY);
	wraw(&w, s->group, 16);
	wraw(&w, s->cid, 32);
	wraw(&w, s->ck, 32);
	wraw(&w, s->hk, 32);
	w32(&w, s->index);
	if (w.bad) return WIRE_ESPACE;
	size_t rn = 0;
	int e = wire_encode_rec(out + w.i, cap - w.i, &rn, &s->rec);
	if (e) return e;
	*len = w.i + rn;
	return WIRE_OK;
}

int wire_decode_senderkey(const uint8_t *b, size_t n, struct wire_senderkey *o)
{
	rd r = { b, n, 0, 0 };
	memset(o, 0, sizeof *o);
	uint8_t kind = r8(&r);
	o->group = rb(&r, 16);
	o->cid = rb(&r, 32);
	o->ck = rb(&r, 32);
	o->hk = rb(&r, 32);
	o->index = r32(&r);
	if (r.bad) return WIRE_ETRUNC;
	if (kind != WIRE_I_SENDERKEY) return WIRE_ETYPE;
	return wire_decode_rec(b + r.i, n - r.i, &o->rec);
}

/* invite url */

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

void wire_hex(char *out, const uint8_t *b, size_t n)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = d[b[i] >> 4];
		out[i * 2 + 1] = d[b[i] & 15];
	}
	out[n * 2] = 0;
}

int wire_unhex(uint8_t *out, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int hi = hexval(s[i * 2]);
		if (hi < 0) return WIRE_EJUNK;          /* checked first: s may end here */
		int lo = hexval(s[i * 2 + 1]);
		if (lo < 0) return WIRE_EJUNK;
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return WIRE_OK;
}

int wire_encode_invite(char *out, size_t cap, const struct wire_invite *v)
{
	char spk[65], token[65];
	wire_hex(spk, v->spk, 32);
	wire_hex(token, v->token, 32);
	int n = snprintf(out, cap, "whimsy://%s:%s/%s/%s", v->host, v->port, spk, token);
	return n > 0 && (size_t)n < cap ? WIRE_OK : WIRE_ESPACE;
}

int wire_decode_invite(const char *s, struct wire_invite *o)
{
	static const char pfx[] = "whimsy://";
	memset(o, 0, sizeof *o);
	if (strncmp(s, pfx, sizeof pfx - 1)) return WIRE_EJUNK;
	s += sizeof pfx - 1;

	size_t i = 0;
	while (s[i] && s[i] != ':') {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '-')) return WIRE_EJUNK;
		if (++i >= sizeof o->host) return WIRE_ELEN;
	}
	if (!i || s[i] != ':') return WIRE_EJUNK;
	memcpy(o->host, s, i);
	s += i + 1;

	unsigned port = 0;
	for (i = 0; s[i] >= '0' && s[i] <= '9'; ) {
		port = port * 10 + (unsigned)(s[i] - '0');
		if (++i >= sizeof o->port) return WIRE_ELEN;
	}
	if (!i || !port || port > 65535 || s[0] == '0' || s[i] != '/') return WIRE_EJUNK;
	memcpy(o->port, s, i);
	s += i + 1;

	if (wire_unhex(o->spk, s, 32) != WIRE_OK) return WIRE_EJUNK;
	if (s[64] != '/') return WIRE_EJUNK;
	if (wire_unhex(o->token, s + 65, 32) != WIRE_OK) return WIRE_EJUNK;
	return s[65 + 64] ? WIRE_EJUNK : WIRE_OK;
}
