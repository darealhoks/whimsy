#include "int.h"

#include <stdio.h>
#include <stdlib.h>

/* groups */

struct grp *find_grp(const struct whimsy *w, const uint8_t id[WHIMSY_GID])
{
	for (size_t i = 0; i < w->ng; i++)
		if (wc_equal(w->g[i]->g.id, id, WHIMSY_GID)) return w->g[i];
	return NULL;
}

/* appends a zeroed group; its address is stable, which g.r's pointers rely on */
struct grp *add_grp(struct whimsy *w)
{
	if (w->ng == w->gcap) {
		size_t cap = w->gcap ? w->gcap * 2 : 8;
		struct grp **p = realloc(w->g, cap * sizeof *p);
		if (!p) return NULL;
		w->g = p;
		w->gcap = cap;
	}
	struct grp *gr = calloc(1, sizeof *gr);
	if (gr) w->g[w->ng++] = gr;
	return gr;
}

void drop_last(struct whimsy *w)
{
	struct grp *gr = w->g[--w->ng];
	free(gr->msg);
	wc_wipe(gr, sizeof *gr);
	free(gr);
}

int push_msg(struct grp *gr, size_t idx)
{
	if (gr->nmsg == gr->mcap) {
		size_t cap = gr->mcap ? gr->mcap * 2 : 64;
		size_t *p = realloc(gr->msg, cap * sizeof *p);
		if (!p) return WHIMSY_ENOMEM;
		gr->msg = p;
		gr->mcap = cap;
	}
	gr->msg[gr->nmsg++] = idx;
	return WHIMSY_OK;
}

/* high-water slot for a channel; NULL when full and the channel is new */
struct seenslot *seen_slot(struct grp *gr, uint16_t chan)
{
	for (size_t i = 0; i < gr->nseen; i++)
		if (gr->seen[i].chan == chan) return &gr->seen[i];
	if (gr->nseen == WIRE_MAX_CHANNELS) return NULL;
	gr->seen[gr->nseen] = (struct seenslot){ chan, 0, SIZE_MAX };
	return &gr->seen[gr->nseen++];
}

/* store indices gathered for one store_void_many: voiding K records costs one rewrite */

int idxv_add(struct idxv *v, size_t i)
{
	if (v->n == v->cap) {
		size_t cap = v->cap ? v->cap * 2 : 16;
		size_t *p = realloc(v->p, cap * sizeof *p);
		if (!p) return WHIMSY_ENOMEM;
		v->p = p;
		v->cap = cap;
	}
	v->p[v->n++] = i;
	return WHIMSY_OK;
}

/* one rewrite for the whole vector, which is spent either way */
int void_batch(struct whimsy *w, struct idxv *v)
{
	int e = v->n ? map_store(store_void_many(w->st, v->p, v->n)) : WHIMSY_OK;
	free(v->p);
	*v = (struct idxv){ NULL, 0, 0 };
	return e;
}

/* the newest SEEN for a (group, channel) wins, so the record this one supersedes is
 * dead weight: with a batch open it joins that batch, alone it waits for the next one.
 * ponytail: one dead 26-byte record per read mark -- store_replace here would be a
 * whole-file rewrite every time a message is read */
int seen_write(struct whimsy *w, struct grp *gr, struct seenslot *sl,
                      uint64_t hw, struct idxv *v)
{
	uint8_t rec[WHIMSY_GID + 10];
	memcpy(rec, gr->g.id, WHIMSY_GID);
	st16(rec + WHIMSY_GID, sl->chan);
	st64(rec + WHIMSY_GID + 2, hw);
	int e = map_store(store_append(w->st, STORE_SEEN, rec, sizeof rec));
	if (e) return e;
	if (v && sl->rec != SIZE_MAX && (e = idxv_add(v, sl->rec))) return e;
	sl->hw = hw;
	sl->rec = store_count(w->st) - 1;
	return WHIMSY_OK;
}

uint64_t seen_hw(const struct grp *gr, uint16_t chan)
{
	for (size_t i = 0; i < gr->nseen; i++)
		if (gr->seen[i].chan == chan) return gr->seen[i].hw;
	return 0;
}

static void put_chain(uint8_t *p, const struct group_chain *c)
{
	memcpy(p, c->pk, 32);
	memcpy(p + 32, c->cid, 32);
	memcpy(p + 64, c->ck, 32);
	memcpy(p + 96, c->hk, 32);
	st32(p + 128, c->base);
	memcpy(p + 132, c->seen, GROUP_SKIP / 8);
}

static void get_chain(struct group_chain *c, const uint8_t *p)
{
	memcpy(c->pk, p, 32);
	memcpy(c->cid, p + 32, 32);
	memcpy(c->ck, p + 64, 32);
	memcpy(c->hk, p + 96, 32);
	c->base = ld32(p + 128);
	memcpy(c->seen, p + 132, GROUP_SKIP / 8);
}

int save_group(struct whimsy *w, const struct grp *gr)
{
	size_t need = 2 + gr->g.rec_n + CHAIN_ENC + 1 + (size_t)gr->g.nrecv * CHAIN_ENC;
	uint8_t *b = malloc(need);
	if (!b) return WHIMSY_ENOMEM;
	size_t o = 0;
	/* rec_n is at most GROUP_MAX_REC, so bit 15 is free and carries the pending re-seal:
	 * a client killed in that window must not come back sending on a dead chain */
	st16(b, (uint16_t)(gr->g.rec_n | (gr->stale ? 0x8000u : 0)));
	o = 2;
	memcpy(b + o, gr->g.rec, gr->g.rec_n);
	o += gr->g.rec_n;
	put_chain(b + o, &gr->g.send);
	o += CHAIN_ENC;
	b[o++] = gr->g.nrecv;
	for (uint8_t i = 0; i < gr->g.nrecv; i++) put_chain(b + o + (size_t)i * CHAIN_ENC, &gr->g.recv[i]);
	int e = store_append(w->st, STORE_GROUP, b, need);
	wc_wipe(b, need);
	free(b);
	return map_store(e);
}

/* ECORRUPT with *named set means the record still told us which group it was for:
 * only that group is lost, not every group in the store */
int load_group(struct whimsy *w, const uint8_t *b, size_t n, int *named)
{
	if (n < 3 + CHAIN_ENC) return WHIMSY_ECORRUPT;
	size_t rec_n = ld16(b) & 0x7fff;
	if (rec_n > GROUP_MAX_REC || n < 2 + rec_n + CHAIN_ENC + 1) return WHIMSY_ECORRUPT;
	struct wire_rec r;
	if (wire_decode_rec(b + 2, rec_n, &r) != WIRE_OK) return WHIMSY_ECORRUPT;

	uint8_t gid[WHIMSY_GID];
	group_id(gid, r.owner, r.salt);
	struct grp *gr = find_grp(w, gid);

	uint8_t nrecv = b[2 + rec_n + CHAIN_ENC];
	if (nrecv > GROUP_MAX_MEMBERS || n != 3 + rec_n + CHAIN_ENC + (size_t)nrecv * CHAIN_ENC) {
		/* a placeholder when the group has not been seen yet: a later record for the
		 * same id is newer state and revives it, otherwise it is dropped after load */
		if (!gr && (gr = add_grp(w))) memcpy(gr->g.id, gid, WHIMSY_GID);
		if (gr) { gr->bad = 1; *named = 1; }
		return WHIMSY_ECORRUPT;
	}

	if (!gr && !(gr = add_grp(w))) return WHIMSY_ENOMEM;
	memcpy(gr->g.id, gid, WHIMSY_GID);
	memcpy(gr->g.rec, b + 2, rec_n);
	gr->g.rec_n = rec_n;
	wire_decode_rec(gr->g.rec, rec_n, &gr->g.r);
	get_chain(&gr->g.send, b + 2 + rec_n);
	gr->g.nrecv = nrecv;
	for (uint8_t i = 0; i < nrecv; i++)
		get_chain(&gr->g.recv[i], b + 3 + rec_n + CHAIN_ENC + (size_t)i * CHAIN_ENC);
	gr->stale = ld16(b) >> 15;
	gr->bad = 0;                    /* this record is newer than the one that would not parse */
	return WHIMSY_OK;
}


/* outbox: blobs built but not delivered, oldest first, across restarts */

int push_out(struct whimsy *w, uint64_t id, const uint8_t gid[WHIMSY_GID],
                    const uint8_t *b, size_t n, size_t mrec)
{
	if (w->nout == w->ocap) {
		size_t cap = w->ocap ? w->ocap * 2 : 8;
		struct out *p = realloc(w->out, cap * sizeof *p);
		if (!p) return WHIMSY_ENOMEM;
		w->out = p;
		w->ocap = cap;
	}
	struct out *o = &w->out[w->nout];
	if (!(o->b = malloc(n))) return WHIMSY_ENOMEM;
	o->id = id;
	o->mrec = mrec;
	memcpy(o->gid, gid, WHIMSY_GID);
	memcpy(o->b, b, n);
	o->n = n;
	w->nout++;
	return WHIMSY_OK;
}

void drop_out(struct whimsy *w, uint64_t id)
{
	for (size_t i = 0; i < w->nout; i++) {
		if (w->out[i].id != id) continue;
		wc_wipe(w->out[i].b, w->out[i].n);
		free(w->out[i].b);
		memmove(w->out + i, w->out + i + 1, (--w->nout - i) * sizeof *w->out);
		return;
	}
}

/* the blob goes out verbatim later: same index on the chain, same time inside.
 * OUTHDR + GROUP_MAX_BLOB is 65738, inside store.h's STORE_MAX_REC. the row's store
 * index rides along so a delete after a restart still catches its own queued blob;
 * UINT64_MAX is a blob no row owns */
int queue_out(struct whimsy *w, const struct grp *gr, const uint8_t *b, size_t n,
                     size_t mrec)
{
	uint8_t *rec = malloc(OUTHDR + n);
	if (!rec) return WHIMSY_ENOMEM;
	memcpy(rec, gr->g.id, WHIMSY_GID);
	st64(rec + WHIMSY_GID, mrec == SIZE_MAX ? UINT64_MAX : (uint64_t)mrec);
	memcpy(rec + OUTHDR, b, n);
	int e = map_store(store_append(w->st, STORE_OUTBOX2, rec, OUTHDR + n));
	free(rec);
	return e ? e : push_out(w, store_count(w->st) - 1, gr->g.id, b, n, mrec);
}


/* held files */

void xfer_free(struct xfer *x)
{
	if (!x) return;
	free(x->b);
	free(x->out);
	free(x);
}

struct xfer *xfer_find(const struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                              const uint8_t fid[16], const uint8_t sender[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nheld; i++)
		if (wc_equal(w->held[i]->gid, gid, WHIMSY_GID) && wc_equal(w->held[i]->fid, fid, 16) &&
		    wc_equal(w->held[i]->sender, sender, WHIMSY_PK))
			return w->held[i];
	return NULL;
}

struct xfer *xfer_row(const struct whimsy *w, const uint8_t gid[WHIMSY_GID], size_t row)
{
	for (size_t i = 0; i < w->nheld; i++)
		if (w->held[i]->row == row && wc_equal(w->held[i]->gid, gid, WHIMSY_GID))
			return w->held[i];
	return NULL;
}

/* the table is WHIMSY_HELD deep and the oldest goes: a sender cannot make us hold more
 * than WHIMSY_HELD * WHIMSY_MAX_FILE, and holding nothing is always a legal outcome */
struct xfer *xfer_add(struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                             const uint8_t fid[16], const uint8_t sender[WHIMSY_PK],
                             uint32_t midx, size_t row, uint32_t total)
{
	struct xfer *x = calloc(1, sizeof *x);
	if (!x) return NULL;
	if (!(x->b = calloc(total, WIRE_FILE_CHUNK))) { free(x); return NULL; }
	memcpy(x->gid, gid, WHIMSY_GID);
	memcpy(x->fid, fid, 16);
	memcpy(x->sender, sender, WHIMSY_PK);
	x->row = row;
	x->midx = midx;
	x->total = total;
	if (w->nheld == WHIMSY_HELD) {
		xfer_free(w->held[0]);
		memmove(w->held, w->held + 1, (--w->nheld) * sizeof *w->held);
	}
	w->held[w->nheld++] = x;
	return x;
}

/* one chunk on disk. the server deletes a blob the moment we ack it, so a reassembly
 * held only in memory is unrecoverable after a restart */
int save_chunk(struct whimsy *w, const struct xfer *x, uint32_t idx,
                      const uint8_t *b, size_t n)
{
	uint8_t *rec = malloc(XFERHDR + n);
	if (!rec) return WHIMSY_ENOMEM;
	memcpy(rec, x->gid, WHIMSY_GID);
	memcpy(rec + WHIMSY_GID, x->sender, WHIMSY_PK);
	st32(rec + WHIMSY_GID + WHIMSY_PK, x->midx);
	memcpy(rec + WHIMSY_GID + WHIMSY_MSGID, x->fid, 16);
	st32(rec + WHIMSY_GID + WHIMSY_MSGID + 16, x->total);
	st32(rec + WHIMSY_GID + WHIMSY_MSGID + 20, idx);
	memcpy(rec + XFERHDR, b, n);
	int e = map_store(store_append(w->st, STORE_XFER, rec, XFERHDR + n));
	free(rec);
	return e;
}

/* chunk records of a transfer no longer held: an evicted file, one whose row or group
 * was dropped, one that never completed and lost its slot */
int dead_xfers(struct whimsy *w, struct idxv *v)
{
	for (size_t i = store_count(w->st); i-- > 0; ) {
		uint8_t k;
		size_t n;
		const uint8_t *p = store_get(w->st, i, &k, &n);
		if (!p || k != STORE_XFER || n < XFERHDR) continue;
		size_t j = 0;
		while (j < w->nheld && !(wc_equal(w->held[j]->gid, p, WHIMSY_GID) &&
		                         wc_equal(w->held[j]->fid, p + WHIMSY_GID + WHIMSY_MSGID, 16)))
			j++;
		if (j == w->nheld && idxv_add(v, i)) return WHIMSY_ENOMEM;
	}
	return WHIMSY_OK;
}

/* never call this while a store_get pointer is live -- the void rewrites the file */
void prune_xfers(struct whimsy *w)
{
	struct idxv v = { NULL, 0, 0 };
	if (dead_xfers(w, &v)) free(v.p);
	else void_batch(w, &v);
}

/* petnames */

struct pet *find_pet(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->npet; i++)
		if (wc_equal(w->pet[i].pk, pk, WHIMSY_PK)) return &w->pet[i];
	return NULL;
}

/* t is already sanitized; n == 0 drops the entry */
int set_pet(struct whimsy *w, const uint8_t pk[WHIMSY_PK], const char *t, size_t n)
{
	struct pet *p = find_pet(w, pk);
	if (!n) {
		if (p) *p = w->pet[--w->npet];
		return WHIMSY_OK;
	}
	if (!p) {
		if (w->npet == w->pcap) {
			size_t cap = w->pcap ? w->pcap * 2 : 8;
			struct pet *q = realloc(w->pet, cap * sizeof *q);
			if (!q) return WHIMSY_ENOMEM;
			w->pet = q;
			w->pcap = cap;
		}
		p = &w->pet[w->npet++];
		memcpy(p->pk, pk, WHIMSY_PK);
	}
	p->n = (uint8_t)n;
	memcpy(p->t, t, n);
	return WHIMSY_OK;
}

/* reactions */

/* t is already sanitized; n == 0 drops the entry. the order of what stays is the
 * order it arrived in, which is the order whimsy_reactions hands out */
static struct react *find_react(const struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                               const uint8_t *id, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nrea; i++)
		if (wc_equal(w->rea[i].gid, gid, WHIMSY_GID) &&
		    wc_equal(w->rea[i].id, id, WHIMSY_MSGID) &&
		    wc_equal(w->rea[i].pk, pk, WHIMSY_PK)) return (struct react *)&w->rea[i];
	return NULL;
}

int set_react(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const uint8_t *id,
                     const uint8_t pk[WHIMSY_PK], const char *t, size_t n)
{
	struct react *r = find_react(w, gid, id, pk);
	if (!n) {
		if (r) memmove(r, r + 1, (size_t)(&w->rea[--w->nrea] - r) * sizeof *r);
		return WHIMSY_OK;
	}
	if (!r) {
		if (w->nrea >= REACTIONS) return WHIMSY_ESTATE;
		if (w->nrea == w->rcap) {
			size_t cap = w->rcap ? w->rcap * 2 : 8;
			struct react *q = realloc(w->rea, cap * sizeof *q);
			if (!q) return WHIMSY_ENOMEM;
			w->rea = q;
			w->rcap = cap;
		}
		r = &w->rea[w->nrea++];
		memcpy(r->gid, gid, WHIMSY_GID);
		memcpy(r->id, id, WHIMSY_MSGID);
		memcpy(r->pk, pk, WHIMSY_PK);
	}
	r->n = (uint8_t)n;
	memcpy(r->t, t, n);
	return WHIMSY_OK;
}

/* set_react plus the store record behind it. text is raw bytes; what is stored and
 * what is kept is what text_sanitize returned, and a sender whose text does not fit
 * the cap after that is ignored rather than truncated */
int save_react(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const uint8_t *id,
                      const uint8_t pk[WHIMSY_PK], const void *text, size_t n)
{
	char t[WHIMSY_MAX_REACT];
	size_t tn = text_sanitize(t, sizeof t, text, n);
	if (tn > sizeof t) return WHIMSY_EARG;
	struct react *have = find_react(w, gid, id, pk);
	if (!have && (!tn || w->nrea >= REACTIONS)) return WHIMSY_OK;   /* nothing to clear, nothing to add */
	size_t hn = WHIMSY_GID + WHIMSY_MSGID + WHIMSY_PK;
	uint8_t rec[WHIMSY_GID + WHIMSY_MSGID + WHIMSY_PK + WHIMSY_MAX_REACT];
	memcpy(rec, gid, WHIMSY_GID);
	memcpy(rec + WHIMSY_GID, id, WHIMSY_MSGID);
	memcpy(rec + WHIMSY_GID + WHIMSY_MSGID, pk, WHIMSY_PK);
	memcpy(rec + hn, t, tn);
	int e = map_store(store_append(w->st, STORE_REACT, rec, hn + tn));
	return e ? e : set_react(w, gid, id, pk, t, tn);
}

/* device links */

int declared(const struct whimsy *w, const uint8_t who[WHIMSY_PK],
                    const uint8_t other[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nlnk; i++)
		if (wc_equal(w->lnk[i].who, who, WHIMSY_PK) &&
		    wc_equal(w->lnk[i].other, other, WHIMSY_PK)) return 1;
	return 0;
}

static size_t declared_by(const struct whimsy *w, const uint8_t who[WHIMSY_PK])
{
	size_t n = 0;
	for (size_t i = 0; i < w->nlnk; i++)
		if (wc_equal(w->lnk[i].who, who, WHIMSY_PK)) n++;
	return n;
}

/* half of a link, from us or off a signed blob. a repeat is a no-op; WHIMSY_MAX_LINKS
 * per declaring key is what keeps a member from growing this table without bound */
int add_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
                    const uint8_t other[WHIMSY_PK])
{
	if (wc_equal(who, other, WHIMSY_PK) || !wc_pk_ok(other)) return WHIMSY_EARG;
	if (declared(w, who, other)) return WHIMSY_OK;
	if (declared_by(w, who) >= WHIMSY_MAX_LINKS) return WHIMSY_ESTATE;
	if (w->nlnk == w->lcap) {
		size_t cap = w->lcap ? w->lcap * 2 : 8;
		struct link *p = realloc(w->lnk, cap * sizeof *p);
		if (!p) return WHIMSY_ENOMEM;
		w->lnk = p;
		w->lcap = cap;
	}
	memcpy(w->lnk[w->nlnk].who, who, WHIMSY_PK);
	memcpy(w->lnk[w->nlnk].other, other, WHIMSY_PK);
	w->nlnk++;
	return WHIMSY_OK;
}

void del_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
              const uint8_t other[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nlnk; i++) {
		if (!wc_equal(w->lnk[i].who, who, WHIMSY_PK) ||
		    !wc_equal(w->lnk[i].other, other, WHIMSY_PK)) continue;
		memmove(w->lnk + i, w->lnk + i + 1, (--w->nlnk - i) * sizeof *w->lnk);
		return;
	}
}

static int link_rec(struct whimsy *w, const uint8_t who[WHIMSY_PK],
                    const uint8_t other[WHIMSY_PK], int on)
{
	uint8_t rec[2 * WHIMSY_PK + 1];
	memcpy(rec, who, WHIMSY_PK);
	memcpy(rec + WHIMSY_PK, other, WHIMSY_PK);
	rec[2 * WHIMSY_PK] = (uint8_t)on;
	return map_store(store_append(w->st, STORE_LINK, rec, sizeof rec));
}

/* add_link plus the store record behind it; a repeat is a no-op */
int save_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
                     const uint8_t other[WHIMSY_PK])
{
	if (declared(w, who, other)) return WHIMSY_OK;
	int e = add_link(w, who, other);
	if (e) return e;
	if ((e = link_rec(w, who, other, 1)))
		w->nlnk--;              /* the one add_link just appended */
	return e;
}

/* del_link plus the store record behind it; a repeat is a no-op */
int drop_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
              const uint8_t other[WHIMSY_PK])
{
	if (!declared(w, who, other)) return WHIMSY_OK;
	int e = link_rec(w, who, other, 0);
	if (e) return e;
	del_link(w, who, other);
	return WHIMSY_OK;
}

/* out-of-band verification */


int add_ver(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (whimsy_verified(w, pk)) return WHIMSY_OK;
	if (w->nver == w->vcap) {
		size_t cap = w->vcap ? w->vcap * 2 : 8;
		uint8_t (*p)[WHIMSY_PK] = realloc(w->ver, cap * WHIMSY_PK);
		if (!p) return WHIMSY_ENOMEM;
		w->ver = p;
		w->vcap = cap;
	}
	memcpy(w->ver[w->nver++], pk, WHIMSY_PK);
	return WHIMSY_OK;
}

int whimsy_verified(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nver; i++)
		if (wc_equal(w->ver[i], pk, WHIMSY_PK)) return 1;
	return 0;
}

void del_ver(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nver; i++) {
		if (!wc_equal(w->ver[i], pk, WHIMSY_PK)) continue;
		memmove(w->ver[i], w->ver[i + 1], (--w->nver - i) * WHIMSY_PK);
		return;
	}
}

int whimsy_unverify(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (!wc_pk_ok(pk)) return WHIMSY_EARG;
	if (!whimsy_verified(w, pk)) return WHIMSY_OK;
	uint8_t rec[WHIMSY_PK + 1];
	memcpy(rec, pk, WHIMSY_PK);
	rec[WHIMSY_PK] = 0;
	int e = map_store(store_append(w->st, STORE_VERIFY, rec, sizeof rec));
	if (e) return e;
	del_ver(w, pk);
	return WHIMSY_OK;
}

int whimsy_verify(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (!wc_pk_ok(pk)) return WHIMSY_EARG;
	if (whimsy_verified(w, pk)) return WHIMSY_OK;
	int e = add_ver(w, pk);
	if (e) return e;
	uint8_t rec[WHIMSY_PK + 1];
	memcpy(rec, pk, WHIMSY_PK);
	rec[WHIMSY_PK] = 1;
	if ((e = map_store(store_append(w->st, STORE_VERIFY, rec, sizeof rec)))) w->nver--;
	if (e) return e;
	e = push_avatars(w);            /* they are verified now, so our avatar may go */
	return e == WHIMSY_ENET ? WHIMSY_OK : e;
}

/* blocking. inbound only: see whimsy.h on why there is no outbound half */

static int blk_has(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nblk; i++) if (wc_equal(w->blk[i], pk, WHIMSY_PK)) return 1;
	return 0;
}

int blk_set(struct whimsy *w, const uint8_t pk[WHIMSY_PK], int on)
{
	for (size_t i = 0; i < w->nblk; i++) {
		if (!wc_equal(w->blk[i], pk, WHIMSY_PK)) continue;
		if (!on) memmove(w->blk[i], w->blk[i + 1], (--w->nblk - i) * WHIMSY_PK);
		return WHIMSY_OK;
	}
	if (!on) return WHIMSY_OK;
	if (w->nblk == w->bcap) {
		size_t cap = w->bcap ? w->bcap * 2 : 8;
		uint8_t (*p)[WHIMSY_PK] = realloc(w->blk, cap * WHIMSY_PK);
		if (!p) return WHIMSY_ENOMEM;
		w->blk = p;
		w->bcap = cap;
	}
	memcpy(w->blk[w->nblk++], pk, WHIMSY_PK);
	return WHIMSY_OK;
}

/* the links are resolved here rather than at block time: one record per key, and a
 * device linked after the block is covered too */
int whimsy_blocked(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (blk_has(w, pk)) return 1;
	uint8_t l[WHIMSY_MAX_LINKS * WHIMSY_PK];
	size_t n = whimsy_links(w, pk, l, WHIMSY_MAX_LINKS);
	for (size_t i = 0; i < n; i++) if (blk_has(w, l + i * WHIMSY_PK)) return 1;
	return 0;
}

int whimsy_block(struct whimsy *w, const uint8_t pk[WHIMSY_PK], int on)
{
	if (!wc_pk_ok(pk) || wc_equal(pk, w->id.pk, WHIMSY_PK)) return WHIMSY_EARG;
	uint8_t rec[WHIMSY_PK + 1];
	memcpy(rec, pk, WHIMSY_PK);
	rec[WHIMSY_PK] = on ? 1 : 0;
	int e = map_store(store_append(w->st, STORE_BLOCK, rec, sizeof rec));
	return e ? e : blk_set(w, pk, on);
}

