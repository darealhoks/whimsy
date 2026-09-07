#include "int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* snprintf-style truncation, defined with the other string helpers below */
size_t term(char *out, size_t cap, size_t w);

int map_store(int e)
{
	switch (e) {
	case STORE_OK:       return WHIMSY_OK;
	case STORE_EKEY:     return WHIMSY_EKEY;
	case STORE_EFORMAT:  return WHIMSY_EFORMAT;
	case STORE_ECORRUPT: return WHIMSY_ECORRUPT;
	case STORE_ENOMEM:   return WHIMSY_ENOMEM;
	default:             return WHIMSY_EIO;
	}
}

int map_group(int e)
{
	switch (e) {
	case GROUP_OK:      return WHIMSY_OK;
	case GROUP_EMEMBER: return WHIMSY_ESTATE;
	case GROUP_EFULL:   return WHIMSY_ESTATE;
	default:            return WHIMSY_EPROTO;
	}
}

const char *whimsy_strerror(int e)
{
	switch (e) {
	case WHIMSY_OK:       return "ok";
	case WHIMSY_EIO:      return "i/o failed";
	case WHIMSY_EKEY:     return "wrong passphrase";
	case WHIMSY_EFORMAT:  return "not a whimsy store, or an older version of one";
	case WHIMSY_ECORRUPT: return "store is corrupt";
	case WHIMSY_ENOMEM:   return "out of memory";
	case WHIMSY_ENET:     return "offline";
	case WHIMSY_EPROTO:   return "protocol error";
	case WHIMSY_EARG:     return "bad argument";
	case WHIMSY_ESTATE:   return "not allowed here";
	default:         return "unknown error";
	}
}

size_t whimsy_sanitize(const void *in, size_t n, void *out, size_t cap)
{
	return text_sanitize(out, cap, in, n);
}

/* identity */

void whimsy_self(const struct whimsy *w, uint8_t out[WHIMSY_PK]) { memcpy(out, w->id.pk, WHIMSY_PK); }

void whimsy_fingerprint(char out[WHIMSY_FP_LEN], const uint8_t pk[WHIMSY_PK])
{
	identity_fingerprint(out, pk);
}

void whimsy_pk_hex(char out[2 * WHIMSY_PK + 1], const uint8_t pk[WHIMSY_PK])
{
	wire_hex(out, pk, WHIMSY_PK);
}

int whimsy_pk_parse(uint8_t out[WHIMSY_PK], const char *hex)
{
	if (strlen(hex) != 2 * WHIMSY_PK) return WHIMSY_EARG;
	if (wire_unhex(out, hex, WHIMSY_PK) != WIRE_OK) return WHIMSY_EARG;
	return wc_pk_ok(out) ? WHIMSY_OK : WHIMSY_EARG;
}

/* server */

/* a send or recv that fails drops the socket, so whimsy_online is honest and
 * the next poll redials instead of failing on a dead fd forever */
int snd(struct whimsy *w, const struct wire_frame *f)
{
	if (!net_send(w->n, f)) return WHIMSY_OK;
	net_close(w->n);
	w->n = NULL;
	return WHIMSY_ENET;
}

int rcv(struct whimsy *w, struct wire_frame *f)
{
	if (!net_recv(w->n, f, w->rbuf, sizeof w->rbuf)) return WHIMSY_OK;
	net_close(w->n);
	w->n = NULL;
	return WHIMSY_ENET;
}

static int do_register(struct whimsy *w, const struct wire_invite *v)
{
	uint8_t msg[64], sig[64];
	memcpy(msg, v->token, 32);
	memcpy(msg + 32, w->id.xpk, 32);
	wc_sign(sig, w->id.sk, msg, sizeof msg);

	struct wire_frame f = { .type = WIRE_F_REGISTER };
	f.reg.token = v->token;
	f.reg.pk = w->id.pk;
	f.reg.sig = sig;
	if (snd(w, &f)) return WHIMSY_ENET;
	if (rcv(w, &f)) return WHIMSY_ENET;
	if (f.type != WIRE_F_DONE) return WHIMSY_EPROTO;

	uint8_t rec[2 + WIRE_MAX_HOST + 6 + 32];
	size_t hn = strlen(v->host), pn = strlen(v->port), o = 0;
	rec[o++] = (uint8_t)hn;
	memcpy(rec + o, v->host, hn);
	o += hn;
	rec[o++] = (uint8_t)pn;
	memcpy(rec + o, v->port, pn);
	o += pn;
	memcpy(rec + o, v->spk, 32);
	o += 32;
	return map_store(store_append(w->st, STORE_SERVER, rec, o));
}

int whimsy_connect(struct whimsy *w, const char *invite)
{
	struct wire_invite v = { 0 };
	if (invite) {
		if (wire_decode_invite(invite, &v) != WIRE_OK) return WHIMSY_EARG;
	} else {
		if (!w->registered) return WHIMSY_ESTATE;
		memcpy(v.host, w->host, sizeof v.host);
		memcpy(v.port, w->port, sizeof v.port);
		memcpy(v.spk, w->spk, 32);
	}
	/* an invite naming a server we are not registered with is a move: register there and
	 * append a STORE_SERVER record, so a later whimsy_connect(w, NULL) dials the new one */
	int fresh = !w->registered ||
	            strcmp(w->host, v.host) || strcmp(w->port, v.port) || !wc_equal(w->spk, v.spk, 32);

	net_close(w->n);
	w->n = NULL;
	int nr = net_dial(&w->n, v.host, v.port, v.spk, &w->id);
	if (nr) { w->n = NULL; return nr == NET_EHS ? WHIMSY_EPROTO : WHIMSY_ENET; }

	if (fresh) {
		int e = do_register(w, &v);
		if (e) { net_close(w->n); w->n = NULL; return e; }
		memcpy(w->host, v.host, sizeof w->host);
		memcpy(w->port, v.port, sizeof w->port);
		memcpy(w->spk, v.spk, 32);
		w->registered = 1;
	}
	return WHIMSY_OK;
}

int whimsy_online(const struct whimsy *w) { return w->n != NULL; }

size_t whimsy_pending(const struct whimsy *w) { return w->nout; }

/* group views */

size_t whimsy_group_count(const struct whimsy *w) { return w->ng; }

const uint8_t *whimsy_group_id(const struct whimsy *w, size_t g)
{
	return g < w->ng ? w->g[g]->g.id : NULL;
}

/* text_sanitize doesn't NUL-terminate; back off to a codepoint boundary before writing the NUL */
size_t term(char *out, size_t cap, size_t w)
{
	if (!cap) return w;
	size_t end = w < cap - 1 ? w : cap - 1;
	/* scanning forward, not back from end: a truncated sanitize leaves the last few
	 * bytes below cap never written, and only lead bytes of whole sequences are read */
	size_t i = 0;
	while (i < end) {
		unsigned char lead = (unsigned char)out[i];
		size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 :
		              (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
		if (i + need > end) break;
		i += need;
	}
	out[i] = 0;
	return w;
}

int whimsy_set_petname(struct whimsy *w, const uint8_t pk[WHIMSY_PK], const char *name)
{
	char t[WHIMSY_MAX_PET];
	size_t n = name ? text_sanitize(t, sizeof t, name, strlen(name)) : 0;
	if (n > sizeof t) return WHIMSY_EARG;

	uint8_t rec[WHIMSY_PK + WHIMSY_MAX_PET];
	memcpy(rec, pk, WHIMSY_PK);
	memcpy(rec + WHIMSY_PK, t, n);
	int e = map_store(store_append(w->st, STORE_PETNAME, rec, WHIMSY_PK + n));
	return e ? e : set_pet(w, pk, t, n);
}

size_t whimsy_links(const struct whimsy *w, const uint8_t pk[WHIMSY_PK],
                    uint8_t *out, size_t cap)
{
	size_t n = 0;
	for (size_t i = 0; i < w->nlnk && n < cap; i++) {
		if (!wc_equal(w->lnk[i].who, pk, WHIMSY_PK)) continue;
		if (!declared(w, w->lnk[i].other, pk)) continue;   /* one half is not a link */
		memcpy(out + n * WHIMSY_PK, w->lnk[i].other, WHIMSY_PK);
		n++;
	}
	return n;
}

/* the petname of pk, or of a key linked to it. one hop: a petname reaches the devices
 * pk is linked with, not the devices those are linked with */
static const struct pet *pet_for(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	const struct pet *p = find_pet(w, pk);
	if (p) return p;
	uint8_t l[WHIMSY_MAX_LINKS * WHIMSY_PK];
	size_t n = whimsy_links(w, pk, l, WHIMSY_MAX_LINKS);
	for (size_t i = 0; i < n; i++)
		if ((p = find_pet(w, l + i * WHIMSY_PK))) return p;
	return NULL;
}

size_t whimsy_petname(const struct whimsy *w, const uint8_t pk[WHIMSY_PK], char *out, size_t cap)
{
	const struct pet *p = pet_for(w, pk);
	char fp[ID_FP_LEN];
	const char *s;
	size_t n;

	if (p) {
		s = p->t;
		n = p->n;
	} else {
		identity_fingerprint(fp, pk);
		s = fp;
		n = (size_t)(strchr(fp, ' ') - fp);
	}
	if (cap) memcpy(out, s, n < cap - 1 ? n : cap - 1);
	return term(out, cap, n);
}

size_t whimsy_group_name(const struct whimsy *w, size_t g, char *out, size_t cap)
{
	if (g >= w->ng || !w->g[g]->g.rec_n) return 0;
	size_t r = text_sanitize(out, cap ? cap - 1 : 0, w->g[g]->g.r.name, w->g[g]->g.r.name_n);
	return term(out, cap, r);
}

size_t whimsy_channel_count(const struct whimsy *w, size_t g)
{
	return g < w->ng && w->g[g]->g.rec_n ? w->g[g]->g.r.nchan : 0;
}

size_t whimsy_channel_name(const struct whimsy *w, size_t g, size_t c, char *out, size_t cap)
{
	if (c >= whimsy_channel_count(w, g)) return 0;
	size_t r = text_sanitize(out, cap ? cap - 1 : 0, w->g[g]->g.r.chan[c].p, w->g[g]->g.r.chan[c].n);
	return term(out, cap, r);
}

uint16_t whimsy_channel_id(const struct whimsy *w, size_t g, size_t c)
{
	return c < whimsy_channel_count(w, g) ? w->g[g]->g.r.chan[c].id : 0;
}

size_t whimsy_member_count(const struct whimsy *w, size_t g)
{
	return g < w->ng ? group_member_count(&w->g[g]->g) : 0;
}

const uint8_t *whimsy_member(const struct whimsy *w, size_t g, size_t i)
{
	return g < w->ng ? group_member_at(&w->g[g]->g, i) : NULL;
}

int whimsy_is_owner(const struct whimsy *w, size_t g)
{
	return g < w->ng && w->g[g]->g.rec_n && wc_equal(w->g[g]->g.r.owner, w->id.pk, 32);
}

