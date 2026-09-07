#include "group.h"

#include <string.h>

#define SIG 64

_Static_assert(WIRE_SEAL_PT_HDR + GROUP_MAX_INNER <= GROUP_MAX_PT, "seal pad too small");

/* ratchet */

static void kdf_step(uint8_t ck[32], const char *label)
{
	uint8_t t[32];
	wc_kdf(t, ck, label);
	memcpy(ck, t, 32);
	wc_wipe(t, sizeof t);
}

static void bits_shift(uint8_t *b, size_t nbytes, uint32_t n)
{
	if (n >= nbytes * 8) { memset(b, 0, nbytes); return; }
	size_t by = n / 8;
	unsigned bi = n % 8;
	for (size_t i = 0; i < nbytes; i++) {
		unsigned lo = i + by < nbytes ? b[i + by] : 0;
		unsigned hi = i + by + 1 < nbytes ? b[i + by + 1] : 0;
		b[i] = (uint8_t)((lo >> bi) | (bi ? hi << (8 - bi) : 0));
	}
}

static void chain_slide(struct group_chain *c, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) kdf_step(c->ck, "ck");
	bits_shift(c->seen, sizeof c->seen, n);
	c->base += n;
}

static void chain_init(struct group_chain *c, const uint8_t pk[32], const uint8_t cid[32],
                       const uint8_t ck[32], const uint8_t hk[32], uint32_t index)
{
	memset(c, 0, sizeof *c);
	if (pk) memcpy(c->pk, pk, 32);
	memcpy(c->cid, cid, 32);
	memcpy(c->ck, ck, 32);
	memcpy(c->hk, hk, 32);
	c->base = index;
}

/* the index is the one counter the relay could read off a chain, so it rides masked.
 * hk is drawn with the chain and travels in the senderkey: it cannot come from ck,
 * which has already ratcheted past the chain start by the time a member is sealed in */
static uint32_t index_mask(const uint8_t hk[32], const uint8_t nonce[WC_NONCE])
{
	uint8_t m[32 + WC_NONCE], h[32];
	memcpy(m, hk, 32);
	memcpy(m + 32, nonce, WC_NONCE);
	wc_hash(h, sizeof h, m, sizeof m);
	wc_wipe(m, sizeof m);
	return (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
}

/* the key for one index. reads only: the window moves in chain_mark, after the aead,
 * so a forged index cannot advance the chain past the sender's genuine messages */
static int chain_key(const struct group_chain *c, uint32_t index, uint8_t key[32])
{
	if (index < c->base) return GROUP_EOLD;
	uint32_t d = index - c->base;
	if (d >= GROUP_SKIP + GROUP_MAX_JUMP) return GROUP_EFAR;
	if (d < GROUP_SKIP && c->seen[d / 8] & 1u << d % 8) return GROUP_EREPLAY;
	uint8_t ck[32];
	memcpy(ck, c->ck, 32);
	for (uint32_t i = 0; i < d; i++) kdf_step(ck, "ck");
	wc_kdf(key, ck, "msg");
	wc_wipe(ck, sizeof ck);
	return GROUP_OK;
}

static void chain_mark(struct group_chain *c, uint32_t index)
{
	uint32_t d = index - c->base;
	if (d >= GROUP_SKIP) { chain_slide(c, d - GROUP_SKIP + 1); d = GROUP_SKIP - 1; }
	c->seen[d / 8] |= (uint8_t)(1u << d % 8);
	uint32_t adv = 0;
	while (adv < GROUP_SKIP && c->seen[adv / 8] & 1u << adv % 8) adv++;
	if (adv) chain_slide(c, adv);
}

/* membership */

void group_id(uint8_t id[16], const uint8_t owner[32], const uint8_t salt[16])
{
	uint8_t m[13 + 48], h[32];
	memcpy(m, "whimsy-gid-v2", 13);
	memcpy(m + 13, owner, 32);
	memcpy(m + 45, salt, 16);
	wc_hash(h, sizeof h, m, sizeof m);
	memcpy(id, h, 16);
}

static void rotate(struct group *g)
{
	uint8_t cid[32], ck[32], hk[32];
	wc_random(cid, 32);             /* a fresh chain id per epoch: the relay must not link epochs */
	wc_random(ck, 32);
	wc_random(hk, 32);
	/* the index carries over: a msgid is sender pk | index, so restarting it here
	 * would let an EDIT or DELETE name a message from an earlier epoch */
	chain_init(&g->send, NULL, cid, ck, hk, g->send.base);
	wc_wipe(ck, sizeof ck);
	wc_wipe(hk, sizeof hk);
	wc_wipe(g->recv, sizeof g->recv);
	g->nrecv = 0;
}

static int store_rec(struct group *g, const struct identity *owner, struct wire_rec *r)
{
	uint8_t tmp[GROUP_MAX_REC];
	size_t n;
	r->sig = NULL;
	if (wire_encode_rec(tmp, sizeof tmp, &n, r) != WIRE_OK) return GROUP_ESPACE;
	wc_sign(tmp + n - SIG, owner->sk, tmp, n - SIG);
	memcpy(g->rec, tmp, n);
	g->rec_n = n;
	return wire_decode_rec(g->rec, g->rec_n, &g->r) == WIRE_OK ? GROUP_OK : GROUP_EWIRE;
}

int group_create(struct group *g, const struct identity *owner, const char *name,
                 const char *const *channels, uint8_t nchan)
{
	size_t name_n = strlen(name);
	if (nchan > WIRE_MAX_CHANNELS || name_n > 255) return GROUP_ESPACE;
	memset(g, 0, sizeof *g);
	uint8_t salt[16];
	wc_random(salt, 16);
	group_id(g->id, owner->pk, salt);

	struct wire_rec r;
	memset(&r, 0, sizeof r);
	r.salt = salt;
	r.version = 1;
	r.owner = owner->pk;
	r.name = (const uint8_t *)name;
	r.name_n = name_n;
	r.nchan = nchan;
	for (uint8_t i = 0; i < nchan; i++) {
		size_t cn = strlen(channels[i]);
		if (cn > 255) return GROUP_ESPACE;
		r.chan[i].p = (const uint8_t *)channels[i];
		r.chan[i].n = (uint8_t)cn;
		r.chan[i].id = (uint16_t)(i + 1);
	}
	r.nmemb = 1;
	r.members = owner->pk;

	int e = store_rec(g, owner, &r);
	if (e) return e;
	rotate(g);
	return GROUP_OK;
}

static int change(struct group *g, const struct identity *owner, const uint8_t pk[32], int add)
{
	if (!g->rec_n || !wc_equal(owner->pk, g->r.owner, 32)) return GROUP_EMEMBER;
	if (!add && wc_equal(pk, g->r.owner, 32)) return GROUP_EMEMBER;

	uint8_t mem[GROUP_MAX_MEMBERS * 32];
	uint16_t n = 0;
	int found = 0;
	for (uint16_t i = 0; i < g->r.nmemb; i++) {
		const uint8_t *m = g->r.members + i * 32;
		if (wc_equal(m, pk, 32)) { found = 1; if (!add) continue; }
		memcpy(mem + n * 32, m, 32);
		n++;
	}
	if (add) {
		if (found) return GROUP_EMEMBER;
		if (n >= GROUP_MAX_MEMBERS) return GROUP_EFULL;
		memcpy(mem + n * 32, pk, 32);
		n++;
	} else if (!found) {
		return GROUP_EMEMBER;
	}

	struct wire_rec r = g->r;       /* keeps id, name, channels; those alias g->rec */
	r.version = g->r.version + 1;
	r.members = mem;
	r.nmemb = n;
	int e = store_rec(g, owner, &r);
	if (e) return e;
	rotate(g);
	return GROUP_OK;
}

int group_rename(struct group *g, const struct identity *owner, const char *name)
{
	if (!g->rec_n || !wc_equal(owner->pk, g->r.owner, 32)) return GROUP_EMEMBER;
	size_t name_n = strlen(name);
	if (name_n > 255) return GROUP_ESPACE;

	struct wire_rec r = g->r;       /* the rest aliases g->rec; store_rec encodes elsewhere first */
	r.name = (const uint8_t *)name;
	r.name_n = name_n;
	r.version = g->r.version + 1;
	int e = store_rec(g, owner, &r);
	if (e) return e;
	rotate(g);
	return GROUP_OK;
}

/* the lowest id no channel holds; never 0, so a body's channel field is always a
 * real channel. the list is at most WIRE_MAX_CHANNELS long, so one always exists */
static uint16_t next_id(const struct wire_rec *r)
{
	for (uint16_t id = 1;; id++) {
		uint8_t i = 0;
		while (i < r->nchan && r->chan[i].id != id) i++;
		if (i == r->nchan) return id;
	}
}

/* channel edits ride the same path as a membership change: bumped version, fresh
 * signature, rotate — group_apply_rec rotates on every record a member accepts.
 * a delete renumbers the channels after it, so old messages in those follow the
 * name they were in, not the index. b is the new name for 'r', else NULL */
int group_chan(struct group *g, const struct identity *owner, char op,
               const char *a, const char *b)
{
	if (!g->rec_n || !wc_equal(owner->pk, g->r.owner, 32)) return GROUP_EMEMBER;
	if (op != 'a' && op != 'd' && op != 'r') return GROUP_EGROUP;
	const char *add = op == 'd' ? NULL : op == 'a' ? a : b;
	size_t an = strlen(a), bn = add ? strlen(add) : 0;
	if (!an || an > 255 || (add && (!bn || bn > 255))) return GROUP_ESPACE;

	struct wire_rec r = g->r;       /* chan pointers alias g->rec; store_rec encodes elsewhere first */
	r.nchan = 0;
	int found = 0;
	uint16_t renamed = 0;
	for (uint8_t i = 0; i < g->r.nchan; i++) {
		const struct wire_rec *o = &g->r;
		int hit = o->chan[i].n == an && !memcmp(o->chan[i].p, a, an);
		if (add && o->chan[i].n == bn && !memcmp(o->chan[i].p, add, bn)) return GROUP_EGROUP;
		if (hit) {
			found = 1;
			renamed = o->chan[i].id;   /* a rename keeps the id, so its messages follow it */
			if (op != 'a') continue;   /* dropped, or replaced below by the new name */
		}
		r.chan[r.nchan++] = g->r.chan[i];
	}
	if (op == 'a' && found) return GROUP_EGROUP;
	if (op != 'a' && !found) return GROUP_EGROUP;
	if (op == 'd' && !r.nchan) return GROUP_EGROUP;    /* the last one stays */
	if (add) {
		if (r.nchan >= WIRE_MAX_CHANNELS) return GROUP_EFULL;
		r.chan[r.nchan].p = (const uint8_t *)add;
		r.chan[r.nchan].n = (uint8_t)bn;
		r.chan[r.nchan].id = op == 'r' ? renamed : next_id(&r);
		r.nchan++;
	}

	r.version = g->r.version + 1;
	int e = store_rec(g, owner, &r);
	if (e) return e;
	rotate(g);
	return GROUP_OK;
}

int group_add(struct group *g, const struct identity *owner, const uint8_t pk[32])
{
	return change(g, owner, pk, 1);
}

int group_kick(struct group *g, const struct identity *owner, const uint8_t pk[32])
{
	return change(g, owner, pk, 0);
}

size_t group_member_count(const struct group *g) { return g->rec_n ? g->r.nmemb : 0; }

const uint8_t *group_member_at(const struct group *g, size_t i)
{
	return i < group_member_count(g) ? g->r.members + i * 32 : NULL;
}

int group_has(const struct group *g, const uint8_t pk[32])
{
	for (size_t i = 0; i < group_member_count(g); i++)
		if (wc_equal(g->r.members + i * 32, pk, 32)) return 1;
	return 0;
}

/* owner is a member and no pk repeats */
static int mem_ok(const struct wire_rec *r)
{
	int owner = 0;
	if (!wc_pk_ok(r->owner)) return 0;
	for (uint16_t i = 0; i < r->nmemb; i++) {
		const uint8_t *a = r->members + i * 32;
		if (!wc_pk_ok(a)) return 0;
		if (wc_equal(a, r->owner, 32)) owner = 1;
		for (uint16_t j = i + 1; j < r->nmemb; j++)
			if (wc_equal(a, r->members + j * 32, 32)) return 0;
	}
	return owner;
}

/* ids are nonzero and unique: a message body names a channel by id, and the frontend
 * maps id to a row */
static int chan_ok(const struct wire_rec *r)
{
	for (uint8_t i = 0; i < r->nchan; i++) {
		if (!r->chan[i].id) return 0;
		for (uint8_t j = i + 1; j < r->nchan; j++)
			if (r->chan[i].id == r->chan[j].id) return 0;
	}
	return 1;
}

/* everything group_apply_rec settles before it touches g: a caller that must know
 * what a record says before the chains rotate asks here first */
static int rec_check(const struct group *g, const uint8_t *rec, size_t n,
                     struct wire_rec *r, uint8_t gid[16])
{
	if (n > GROUP_MAX_REC) return GROUP_ESPACE;
	if (wire_decode_rec(rec, n, r) != WIRE_OK) return GROUP_EWIRE;
	if (!wc_verify(r->sig, r->owner, r->signed_from, r->signed_n)) return GROUP_ESIG;
	/* the signature says the owner wrote it, not that it is sane: a duplicate member
	 * makes reseal seal the same chain twice, an owner outside the list cannot be sealed to */
	if (!r->nmemb || !mem_ok(r) || !chan_ok(r)) return GROUP_EMEMBER;
	group_id(gid, r->owner, r->salt);
	if (!g->rec_n) return GROUP_OK;
	if (!wc_equal(gid, g->id, 16)) return GROUP_EGROUP;
	if (!wc_equal(r->owner, g->r.owner, 32)) return GROUP_EGROUP;
	if (r->version <= g->r.version) return GROUP_EGROUP;
	return GROUP_OK;
}

static int rec_has(const struct wire_rec *r, const uint8_t pk[32])
{
	for (uint16_t i = 0; i < r->nmemb; i++)
		if (wc_equal(r->members + i * 32, pk, 32)) return 1;
	return 0;
}

int group_apply_rec(struct group *g, const uint8_t *rec, size_t n)
{
	struct wire_rec r;
	uint8_t gid[16];
	int e = rec_check(g, rec, n, &r, gid);
	if (e) return e;
	if (!g->rec_n) memcpy(g->id, gid, 16);
	memcpy(g->rec, rec, n);
	g->rec_n = n;
	wire_decode_rec(g->rec, g->rec_n, &g->r);
	rotate(g);
	return GROUP_OK;
}

/* sealed sender keys */

static int seal_key(uint8_t key[32], const uint8_t sk[32], const uint8_t peer[32],
                    const uint8_t eph[32], const uint8_t to_xpk[32])
{
	uint8_t m[14 + 96];
	memcpy(m, "whimsy-seal-v2", 14);
	if (wc_dh(m + 14, sk, peer)) { wc_wipe(m, sizeof m); return -1; }
	memcpy(m + 46, eph, 32);
	memcpy(m + 78, to_xpk, 32);
	wc_hash(key, 32, m, sizeof m);
	wc_wipe(m, sizeof m);
	return 0;
}

int group_seal(const struct identity *me, const uint8_t to[32],
               const void *inner, size_t inner_n, uint8_t *out, size_t cap, size_t *len)
{
	if (wc_equal(to, me->pk, 32)) return GROUP_EMEMBER;

	uint8_t pt[GROUP_MAX_PT + WC_MAC];
	struct wire_sealed_body sb;
	memset(&sb, 0, sizeof sb);
	sb.sender = me->pk;
	sb.to = to;
	sb.inner = inner;
	sb.inner_n = inner_n;
	size_t pt_n;
	if (wire_encode_sealed_body(pt, GROUP_MAX_PT, &pt_n, &sb) != WIRE_OK) return GROUP_ESPACE;
	wc_sign(pt + pt_n - SIG, me->sk, pt, pt_n - SIG);

	uint8_t esk[32], epk[32], xpk[32], key[32], nonce[WC_NONCE];
	wc_x25519_keypair(esk, epk);
	wc_x25519_from_sign_pk(xpk, to);
	if (seal_key(key, esk, xpk, epk, xpk)) { /* low-order member pk: dh would be a constant */
		wc_wipe(esk, sizeof esk);
		wc_wipe(pt, sizeof pt);
		return GROUP_EMEMBER;
	}
	wc_random(nonce, sizeof nonce);
	wc_seal(pt, key, nonce, NULL, 0, pt, pt_n);

	struct wire_blob b;
	memset(&b, 0, sizeof b);
	b.hdr = epk;
	wc_random(&b.aux, sizeof b.aux);   /* nothing reads it; a GROUP blob's index sits here */
	b.nonce = nonce;
	b.ct = pt;
	b.ct_n = pt_n + WC_MAC;
	int e = wire_encode_blob(out, cap, len, &b);

	wc_wipe(esk, sizeof esk);
	wc_wipe(key, sizeof key);
	wc_wipe(pt, sizeof pt);
	return e == WIRE_OK ? GROUP_OK : GROUP_ESPACE;
}

void group_forget(struct group *g, const uint8_t pk[32])
{
	for (uint8_t i = 0; i < g->nrecv; i++)
		if (wc_equal(g->recv[i].pk, pk, 32)) {
			wc_wipe(&g->recv[i], sizeof g->recv[i]);
			g->recv[i] = g->recv[--g->nrecv];
			wc_wipe(&g->recv[g->nrecv], sizeof g->recv[g->nrecv]);
			return;
		}
}

int group_seal_to(const struct group *g, const struct identity *me, size_t i,
                  uint8_t *out, size_t cap, size_t *len)
{
	const uint8_t *to = group_member_at(g, i);
	if (!to) return GROUP_EMEMBER;

	uint8_t inner[GROUP_MAX_INNER];
	struct wire_senderkey sk;
	memset(&sk, 0, sizeof sk);
	sk.group = g->id;
	sk.cid = g->send.cid;
	sk.ck = g->send.ck;
	sk.hk = g->send.hk;
	sk.index = g->send.base;
	sk.rec = g->r;
	size_t inner_n;
	if (wire_encode_senderkey(inner, sizeof inner, &inner_n, &sk) != WIRE_OK) {
		wc_wipe(inner, sizeof inner);   /* a partial encode already holds the chain key */
		return GROUP_ESPACE;
	}

	int e = group_seal(me, to, inner, inner_n, out, cap, len);
	wc_wipe(inner, sizeof inner);
	return e;
}

int group_open(const struct identity *me, const uint8_t *blob, size_t n,
               uint8_t *pt, size_t cap, uint8_t sender[32],
               const uint8_t **inner, size_t *inner_n)
{
	struct wire_blob b;
	if (wire_decode_blob(blob, n, &b) != WIRE_OK) return GROUP_EWIRE;
	size_t pt_n = b.ct_n - WC_MAC;
	if (pt_n > cap) return GROUP_ESPACE;

	uint8_t key[32];
	if (seal_key(key, me->xsk, b.hdr, b.hdr, me->xpk)) return GROUP_ECRYPT;
	int bad = wc_open(pt, key, b.nonce, NULL, 0, b.ct, b.ct_n);
	wc_wipe(key, sizeof key);
	if (bad) return GROUP_ECRYPT;

	struct wire_sealed_body sb;
	if (wire_decode_sealed_body(pt, pt_n, &sb) != WIRE_OK) return GROUP_EWIRE;
	if (!wc_equal(sb.to, me->pk, 32)) return GROUP_EMEMBER;
	if (!wc_verify(sb.sig, sb.sender, sb.signed_from, sb.signed_n)) return GROUP_ESIG;
	if (!sb.inner_n) return GROUP_EWIRE;
	memcpy(sender, sb.sender, 32);
	*inner = sb.inner;
	*inner_n = sb.inner_n;
	return GROUP_OK;
}

int group_open_sealed(const struct identity *me, const uint8_t *blob, size_t n,
                      uint8_t *pt, size_t cap, uint8_t sender[32], struct wire_senderkey *sk)
{
	const uint8_t *inner;
	size_t inner_n;
	int e = group_open(me, blob, n, pt, cap, sender, &inner, &inner_n);
	if (e) return e;
	if (inner[0] != WIRE_I_SENDERKEY) return GROUP_ETYPE;
	return wire_decode_senderkey(inner, inner_n, sk) == WIRE_OK ? GROUP_OK : GROUP_EWIRE;
}

int group_recv_senderkey(struct group *g, const struct identity *me,
                         const uint8_t sender[32], const struct wire_senderkey *sk)
{
	if (g->rec_n) { /* nothing may be mutated before these: apply_rec drops every chain */
		if (!wc_equal(sk->group, g->id, 16)) return GROUP_EGROUP;
		if (sk->rec.version < g->r.version) return GROUP_EGROUP;
		if (!group_has(g, me->pk) || !group_has(g, sender)) return GROUP_EMEMBER;
	}
	if (!g->rec_n || sk->rec.version > g->r.version) {
		/* authorization before mutation: apply_rec rotates, and a record that drops us
		 * would leave a chainless group behind while the caller is still told nothing */
		struct wire_rec r;
		uint8_t gid[16];
		int e = rec_check(g, sk->rec.signed_from, sk->rec.signed_n + SIG, &r, gid);
		if (e) return e;
		if (!rec_has(&r, me->pk)) return g->rec_n ? GROUP_EKICKED : GROUP_EMEMBER;
		e = group_apply_rec(g, sk->rec.signed_from, sk->rec.signed_n + SIG);
		if (e) return e;
	}
	if (!wc_equal(sk->group, g->id, 16)) return GROUP_EGROUP;
	if (!group_has(g, me->pk) || !group_has(g, sender)) return GROUP_EMEMBER;

	/* the cid alone picks the chain in group_recv, so a cid another sender already
	 * holds would silently swallow their messages */
	if (wc_equal(sk->cid, g->send.cid, 32)) return GROUP_EGROUP;
	for (uint8_t i = 0; i < g->nrecv; i++)
		if (wc_equal(g->recv[i].cid, sk->cid, 32) && !wc_equal(g->recv[i].pk, sender, 32))
			return GROUP_EGROUP;

	struct group_chain *c = NULL;
	for (uint8_t i = 0; i < g->nrecv && !c; i++)
		if (wc_equal(g->recv[i].pk, sender, 32)) c = &g->recv[i];
	if (c) {
		/* a re-seal at the position we already hold: keep seen[], chain_init would
		 * rewind the replay window. anything below it is a replayed senderkey */
		if (sk->index == c->base && wc_equal(sk->cid, c->cid, 32) &&
		    wc_equal(sk->ck, c->ck, 32)) return GROUP_OK;
		if (sk->index <= c->base) return GROUP_EOLD;
		/* a fresh cid resets seen[], so it must start past every index already
		 * accepted: a msgid is sender | index and an EDIT resolves by it */
		for (uint32_t d = GROUP_SKIP; d-- > 0;)
			if (c->seen[d / 8] & 1u << d % 8) {
				if (sk->index <= c->base + d) return GROUP_EOLD;
				break;
			}
	} else {
		if (g->nrecv == GROUP_MAX_MEMBERS) return GROUP_EFULL;
		c = &g->recv[g->nrecv++];
	}
	chain_init(c, sender, sk->cid, sk->ck, sk->hk, sk->index);
	return GROUP_OK;
}

/* messages */

int group_send(struct group *g, const struct identity *me, uint16_t channel, uint8_t kind,
               uint64_t time, const uint8_t *reply, const void *payload, size_t n,
               uint8_t *out, size_t cap, size_t *len)
{
	if (!g->rec_n || !group_has(g, me->pk)) return GROUP_EMEMBER;

	uint8_t body[WIRE_MAX_BODY];
	struct wire_body wb;
	memset(&wb, 0, sizeof wb);
	wb.kind = kind;
	wb.channel = channel;
	wb.time = time;
	wb.reply = reply;
	wb.payload = payload;
	wb.payload_n = n;
	size_t body_n;
	if (wire_encode_body(body, sizeof body, &body_n, &wb) != WIRE_OK) return GROUP_ESPACE;

	/* sender and the header repeat inside the aead, and the signature covers them:
	 * the relay sees a chain id and an index, a member cannot re-label a blob */
	uint8_t pt[GROUP_MAX_PTB + WC_MAC];
	struct wire_group_pt gp;
	memset(&gp, 0, sizeof gp);
	gp.cid = g->send.cid;
	gp.index = g->send.base;
	gp.sender = me->pk;
	gp.body = body;
	gp.body_n = body_n;
	size_t pt_n;
	if (wire_encode_group_pt(pt, GROUP_MAX_PTB, &pt_n, &gp) != WIRE_OK) return GROUP_ESPACE;
	wc_sign(pt + pt_n - SIG, me->sk, pt, pt_n - SIG);

	uint8_t key[32], nonce[WC_NONCE];
	wc_kdf(key, g->send.ck, "msg");
	wc_random(nonce, sizeof nonce);
	wc_seal(pt, key, nonce, NULL, 0, pt, pt_n);
	wc_wipe(key, sizeof key);

	struct wire_blob b;
	memset(&b, 0, sizeof b);
	b.hdr = g->send.cid;
	b.aux = g->send.base ^ index_mask(g->send.hk, nonce);
	b.nonce = nonce;
	b.ct = pt;
	b.ct_n = pt_n + WC_MAC;
	int e = wire_encode_blob(out, cap, len, &b);
	wc_wipe(body, sizeof body);
	wc_wipe(pt, pt_n + WC_MAC);
	if (e != WIRE_OK) return GROUP_ESPACE;

	kdf_step(g->send.ck, "ck");
	g->send.base++;
	return GROUP_OK;
}

int group_recv(struct group *g, const uint8_t *blob, size_t n,
               uint8_t *scratch, size_t cap, struct group_msg *m)
{
	struct wire_blob b;
	if (!g->rec_n) return GROUP_EGROUP;
	if (wire_decode_blob(blob, n, &b) != WIRE_OK) return GROUP_EWIRE;

	struct group_chain *c = NULL;
	for (uint8_t i = 0; i < g->nrecv && !c; i++)
		if (wc_equal(g->recv[i].cid, b.hdr, 32)) c = &g->recv[i];
	if (!c) return GROUP_ENOCHAIN;
	uint32_t index = b.aux ^ index_mask(c->hk, b.nonce);

	uint8_t key[32];
	int e = chain_key(c, index, key);
	if (e) return e;
	size_t pt_n = b.ct_n - WC_MAC;
	if (pt_n > cap) { wc_wipe(key, sizeof key); return GROUP_ESPACE; }
	int bad = wc_open(scratch, key, b.nonce, NULL, 0, b.ct, b.ct_n);
	wc_wipe(key, sizeof key);
	if (bad) return GROUP_ECRYPT;

	struct wire_group_pt gp;
	if (wire_decode_group_pt(scratch, pt_n, &gp) != WIRE_OK) return GROUP_EWIRE;
	if (!wc_equal(gp.cid, b.hdr, 32) || gp.index != index) return GROUP_EWIRE;
	/* the chain says who this is: holding someone's chain key does not let you sign as a third member */
	if (!wc_equal(gp.sender, c->pk, 32) || !group_has(g, gp.sender)) return GROUP_EMEMBER;
	if (!wc_verify(gp.sig, gp.sender, gp.signed_from, gp.signed_n)) return GROUP_ESIG;

	struct wire_body wb;
	if (wire_decode_body(gp.body, gp.body_n, &wb) != WIRE_OK) return GROUP_EWIRE;
	chain_mark(c, index);

	memcpy(m->sender, gp.sender, 32);
	m->kind = wb.kind;
	m->channel = wb.channel;
	m->time = wb.time;
	m->index = index;
	m->reply = wb.reply;
	m->payload = wb.payload;
	m->payload_n = wb.payload_n;
	return GROUP_OK;
}
