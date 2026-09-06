#include "int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* sending */

/* a blob the relay still holds for a mailbox, so a delete before the peer polls can
 * retract it. ram only: the seq is the relay's handle and nothing persists it, so a
 * blob put in an earlier run is unrevocable, even though its mrec now survives */
static void spool_add(struct whimsy *w, size_t mrec, const uint8_t mb[WHIMSY_PK], uint64_t seq)
{
	if (w->nspool == SPOOL_MAX)
		memmove(w->spool, w->spool + 1, (--w->nspool) * sizeof *w->spool);
	struct spool *sp = &w->spool[w->nspool++];
	*sp = (struct spool){ mrec, seq, { 0 }, 0 };   /* dead too: the slot may be a reused one */
	memcpy(sp->mb, mb, WHIMSY_PK);
}

/* every mailbox still holding a blob of this row. marking only: drop_row_v runs from
 * ingest too, and a request written into a fetch stream would eat a streamed blob and
 * desync the socket. spool_flush is what sends them */
static void spool_revoke(struct whimsy *w, size_t mrec)
{
	for (size_t i = 0; i < w->nspool; i++)
		if (w->spool[i].mrec == mrec) w->spool[i].dead = 1;
}

/* the marked ones out. a slot whose revoke did not reach the relay stays
 * marked, so the next poll retries it; one the relay took is forgotten */
void spool_flush(struct whimsy *w)
{
	if (w->instream) return;
	for (size_t i = w->nspool; i-- > 0; ) {
		if (!w->spool[i].dead) continue;
		if (!w->n) return;
		struct wire_frame f = { .type = WIRE_F_REVOKE };
		f.revoke.mailbox = w->spool[i].mb;
		f.revoke.seq = w->spool[i].seq;
		if (snd(w, &f) || rcv(w, &f) || f.type != WIRE_F_DONE) return;
		memmove(w->spool + i, w->spool + i + 1, (--w->nspool - i) * sizeof *w->spool);
	}
}

static int put_blob(struct whimsy *w, const uint8_t *mailbox, const uint8_t *b, size_t n,
                    uint64_t *seq)
{
	struct wire_frame f = { .type = WIRE_F_PUT };
	f.put.mailbox = mailbox;
	f.put.blob = b;
	f.put.blob_n = n;
	if (snd(w, &f)) return WHIMSY_ENET;
	if (rcv(w, &f)) return WHIMSY_ENET;
	if (f.type == WIRE_F_PUTOK) {
		if (seq) *seq = f.putok.seq;
		return WHIMSY_OK;
	}
	/* no such account, and a mailbox over quota, are both this blob's loss: the
	 * receiver heals the chain gap. anything else is a desynced socket, and the
	 * blob stays deliverable -- drop the connection so the next poll redials */
	if (f.type == WIRE_F_ERR) return WHIMSY_ESTATE;
	net_close(w->n);
	w->n = NULL;
	return WHIMSY_ENET;
}

/* one blob to every member but us; a mailbox the server refuses is not fatal.
 * mrec is the store index of the row this blob carries, SIZE_MAX for none: with one,
 * every spool slot it lands in is remembered so a delete can retract it */
int fan_out(struct whimsy *w, const struct grp *gr, const uint8_t *b, size_t n,
                   size_t mrec)
{
	if (!w->n) return WHIMSY_ENET;
	for (size_t i = 0; i < group_member_count(&gr->g); i++) {
		const uint8_t *m = group_member_at(&gr->g, i);
		uint64_t seq = 0;
		if (wc_equal(m, w->id.pk, 32)) continue;
		int e = put_blob(w, m, b, n, &seq);
		if (e == WHIMSY_ENET) return e;
		if (e) continue;                        /* this member's loss, not the blob's */
		if (seq && mrec != SIZE_MAX) spool_add(w, mrec, m, seq);
	}
	return WHIMSY_OK;
}

/* the outbox, oldest first: order on the chain is order at the receiver. a mailbox
 * the server refuses is dropped by fan_out, a kicked one is gone from the record, and
 * a dead connection leaves the rest of the queue for the next poll */
int flush_out(struct whimsy *w)
{
	if (!w->n) return WHIMSY_ENET;
	struct idxv v = { NULL, 0, 0 };
	int e = WHIMSY_OK;
	while (w->nout) {
		struct grp *gr = find_grp(w, w->out[0].gid);
		int r = gr ? fan_out(w, gr, w->out[0].b, w->out[0].n, w->out[0].mrec) : WHIMSY_OK;
		if (r) { e = r; break; }                /* still undelivered: it stays queued */
		if ((r = idxv_add(&v, (size_t)w->out[0].id))) { e = r; break; }
		drop_out(w, w->out[0].id);
	}
	int ve = void_batch(w, &v);
	return e ? e : ve;
}

/* a fresh chain to every member, carrying the current membership record */
int reseal(struct whimsy *w, struct grp *gr)
{
	uint8_t seal[GROUP_MAX_SEAL];
	size_t n;
	int fail = WHIMSY_OK;
	if (!w->n) return WHIMSY_ENET;
	/* one member we cannot reach must not cost every member after it their chain,
	 * so the loop finishes and the caller keeps gr->stale for the next poll.
	 * re-sealing a member who already has this chain is a no-op for them */
	for (size_t i = 0; i < group_member_count(&gr->g); i++) {
		const uint8_t *m = group_member_at(&gr->g, i);
		if (wc_equal(m, w->id.pk, 32)) continue;
		/* a member we can never seal to -- a low-order pk -- must not keep the group
		 * stale, or one bad member key leaves it unsendable for good */
		int e = group_seal_to(&gr->g, &w->id, i, seal, sizeof seal, &n);
		if (e) continue;
		e = put_blob(w, m, seal, n, NULL);
		if (e == WHIMSY_ENET) return e;
		if (e && e != WHIMSY_ESTATE) fail = e;
	}
	return fail;
}

/* the blob lands in w->blob. the chain has stepped, so the state goes down before the
 * network; a blob whose step we could not record must not go out, the next send would
 * reuse the index */
int emit_build(struct whimsy *w, struct grp *gr, uint16_t channel, uint8_t kind,
                      uint64_t t, const uint8_t *reply, const void *p, size_t n, size_t *len)
{
	/* the chain rotated and nobody holds it yet: a blob on it is GROUP_EOLD to every
	 * receiver once the re-seal lands at the higher index. alone in the group there is
	 * no receiver, so an offline rename must not brick sending */
	if (gr->stale && group_member_count(&gr->g) > 1) return WHIMSY_ESTATE;
	int e = map_group(group_send(&gr->g, &w->id, channel, kind, t, reply, p, n,
	                             w->blob, sizeof w->blob, len));
	return e ? e : save_group(w, gr);
}

/* with a queue standing, this blob queues too, even online: a later index must not
 * pass an earlier one on the way to a mailbox */
int emit_out(struct whimsy *w, struct grp *gr, size_t len, size_t mrec)
{
	if (!w->nout && w->n) {
		int e = fan_out(w, gr, w->blob, len, mrec);
		if (!e) return e;
	}
	return queue_out(w, gr, w->blob, len, mrec);
}


/* the ask, and the chain we could not agree on dropped so the answer installs whatever
 * index and cid it carries. never from inside the fetch stream: the tail of a poll */
int heal_ask(struct whimsy *w, struct grp *gr)
{
	time_t now = time(NULL);
	if (now - gr->asked < HEAL_EVERY) return WHIMSY_OK;
	gr->asked = now;                /* before the work: a failed ask must not re-forget every poll */
	group_forget(&gr->g, gr->healpk);
	int e = save_group(w, gr);
	if (e) return e;
	size_t len;
	uint16_t ch = gr->g.r.nchan ? gr->g.r.chan[0].id : 0;
	e = emit_build(w, gr, ch, WIRE_K_HEAL, (uint64_t)now, NULL, NULL, 0, &len);
	if (e) return e;                /* a stale chain of our own: the re-seal goes first */
	return emit_out(w, gr, len, SIZE_MAX);
}

/* our half of every link, into one group: what a member who was not there when we
 * linked needs to merge our devices into one row */
int publish_links(struct whimsy *w, struct grp *gr)
{
	uint64_t t = (uint64_t)time(NULL);
	uint16_t ch = gr->g.r.nchan ? gr->g.r.chan[0].id : 0;
	int fail = WHIMSY_OK;
	for (size_t i = 0; i < w->nlnk; i++) {
		size_t len;
		if (!wc_equal(w->lnk[i].who, w->id.pk, WHIMSY_PK)) continue;
		int e = emit_build(w, gr, ch, WIRE_K_LINK, t, NULL, w->lnk[i].other, WHIMSY_PK, &len);
		if (!e) e = emit_out(w, gr, len, SIZE_MAX);
		if (e) fail = e;
	}
	return fail;
}

/* avatars */

static struct av *av_find(const struct whimsy *w, const uint8_t *key, size_t kn, int grp)
{
	for (size_t i = 0; i < w->nav; i++)
		if (w->av[i].grp == grp && wc_equal(w->av[i].key, key, kn)) return &w->av[i];
	return NULL;
}

/* the table entry for a record already in the store; the bytes stay the store's */
int av_load(struct whimsy *w, const uint8_t *key, size_t kn, int grp, size_t rec)
{
	struct av *a = av_find(w, key, kn, grp);
	if (!a) {
		if (w->nav == w->avcap) {
			size_t cap = w->avcap ? w->avcap * 2 : 8;
			struct av *p = realloc(w->av, cap * sizeof *p);
			if (!p) return WHIMSY_ENOMEM;
			w->av = p;
			w->avcap = cap;
		}
		a = &w->av[w->nav++];
		memset(a, 0, sizeof *a);
		memcpy(a->key, key, kn);
		a->grp = grp;
	}
	a->rec = rec;
	return WHIMSY_OK;
}

static const uint8_t *av_get(const struct whimsy *w, const uint8_t *key, size_t kn,
                             int grp, size_t *n)
{
	const struct av *a = av_find(w, key, kn, grp);
	size_t rn;
	if (!a) return NULL;
	const uint8_t *p = store_get(w->st, a->rec, NULL, &rn);
	if (!p || rn <= kn) return NULL;        /* a record with no bytes is a cleared avatar */
	*n = rn - kn;
	return p + kn;
}

/* the record it replaces is voided, so an avatar that was changed leaves the disk.
 * ponytail: two full store rewrites per set, so a peer resetting its avatar in a loop
 * costs us that much disk -- only reachable from a co-member; rate limit if that changes */
int av_set(struct whimsy *w, const uint8_t *key, size_t kn, int grp,
                  const void *b, size_t n)
{
	if (n > WHIMSY_MAX_AVATAR) return WHIMSY_EARG;
	uint8_t *rec = malloc(kn + n);
	if (!rec) return WHIMSY_ENOMEM;
	memcpy(rec, key, kn);
	if (n) memcpy(rec + kn, b, n);
	int e = map_store(store_append(w->st, grp ? STORE_GAVATAR : STORE_AVATAR, rec, kn + n));
	free(rec);
	if (e) return e;
	size_t at = store_count(w->st) - 1;
	struct av *a = av_find(w, key, kn, grp);
	if (a && (e = map_store(store_replace(w->st, a->rec, STORE_VOID, NULL, 0)))) return e;
	if ((e = av_load(w, key, kn, grp, at))) return e;
	return WHIMSY_OK;
}

static int sent_has(const struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->nsent; i++) if (wc_equal(w->sent[i], pk, WHIMSY_PK)) return 1;
	return 0;
}

int sent_add(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (sent_has(w, pk)) return WHIMSY_OK;
	if (w->nsent == w->scap) {
		size_t cap = w->scap ? w->scap * 2 : 8;
		uint8_t (*p)[WHIMSY_PK] = realloc(w->sent, cap * WHIMSY_PK);
		if (!p) return WHIMSY_ENOMEM;
		w->sent = p;
		w->scap = cap;
	}
	memcpy(w->sent[w->nsent++], pk, WHIMSY_PK);
	return WHIMSY_OK;
}

static int sent_save(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (sent_has(w, pk)) return WHIMSY_OK;
	int e = sent_add(w, pk);
	if (e) return e;
	if ((e = map_store(store_append(w->st, STORE_ASENT, pk, WHIMSY_PK)))) w->nsent--;
	return e;
}

/* a new avatar is owed to everyone again */
static int sent_clear(struct whimsy *w)
{
	struct idxv v = { NULL, 0, 0 };
	int e = WHIMSY_OK;
	for (size_t i = 0; i < store_count(w->st) && !e; i++) {
		uint8_t kind;
		size_t n;
		if (!store_get(w->st, i, &kind, &n) || kind != STORE_ASENT) continue;
		e = idxv_add(&v, i);
	}
	int ve = void_batch(w, &v);
	if (e || ve) return e ? e : ve;
	w->nsent = 0;
	return WHIMSY_OK;
}

/* our avatar sealed to every verified key we share a group with, once per key. a key
 * we cannot reach stays out of the sent set and is owed to the next poll */
int push_avatars(struct whimsy *w)
{
	uint8_t inner[1 + WHIMSY_MAX_AVATAR];
	size_t an = 0;
	const uint8_t *a;
	if (!w->n) return WHIMSY_ENET;
	/* the record, not the bytes: a cleared avatar is an empty one and must still go
	 * out, while a user who never set one sends nothing at all */
	if (!av_find(w, w->id.pk, WHIMSY_PK, 0)) return WHIMSY_OK;
	a = av_get(w, w->id.pk, WHIMSY_PK, 0, &an);
	inner[0] = WIRE_I_AVATAR;
	if (a) memcpy(inner + 1, a, an);
	for (size_t g = 0; g < w->ng; g++) {
		struct grp *gr = w->g[g];
		for (size_t i = 0; i < group_member_count(&gr->g); i++) {
			const uint8_t *m = group_member_at(&gr->g, i);
			size_t len;
			if (wc_equal(m, w->id.pk, WHIMSY_PK)) continue;
			if (!whimsy_verified(w, m) || sent_has(w, m)) continue;
			if (group_seal(&w->id, m, inner, 1 + an, w->blob, sizeof w->blob, &len))
				continue;
			int e = put_blob(w, m, w->blob, len, NULL);
			if (e == WHIMSY_ENET) return e;
			if (e) continue;        /* no mailbox there: not ours to fix */
			if ((e = sent_save(w, m))) return e;
		}
	}
	return WHIMSY_OK;
}

/* the group's avatar down its chain. every add sends it again, so a member who was not
 * there when the owner set it still gets one */
int send_gavatar(struct whimsy *w, struct grp *gr)
{
	size_t an = 0, len;
	const uint8_t *a;
	if (!gr->g.rec_n || !gr->g.r.nchan) return WHIMSY_OK;   /* 0 is never a real channel */
	uint16_t ch = gr->g.r.chan[0].id;
	if (!av_find(w, gr->g.id, WHIMSY_GID, 1)) return WHIMSY_OK;
	a = av_get(w, gr->g.id, WHIMSY_GID, 1, &an);
	int e = emit_build(w, gr, ch, WIRE_K_GAVATAR, (uint64_t)time(NULL), NULL, a, an, &len);
	return e ? e : emit_out(w, gr, len, SIZE_MAX);
}

int whimsy_avatar_set(struct whimsy *w, const void *bytes, size_t n)
{
	if (n > WHIMSY_MAX_AVATAR || (n && !bytes)) return WHIMSY_EARG;
	int e = av_set(w, w->id.pk, WHIMSY_PK, 0, bytes, n);
	if (!e) e = sent_clear(w);
	if (e) return e;
	e = push_avatars(w);
	return e == WHIMSY_ENET ? WHIMSY_OK : e;        /* offline: the next poll sends it */
}

const uint8_t *whimsy_avatar_get(const struct whimsy *w, const uint8_t pk[WHIMSY_PK], size_t *n)
{
	return av_get(w, pk, WHIMSY_PK, 0, n);
}

int whimsy_group_avatar_set(struct whimsy *w, size_t g, const void *bytes, size_t n)
{
	if (g >= w->ng || n > WHIMSY_MAX_AVATAR || (n && !bytes)) return WHIMSY_EARG;
	if (!whimsy_is_owner(w, g)) return WHIMSY_ESTATE;
	int e = av_set(w, w->g[g]->g.id, WHIMSY_GID, 1, bytes, n);
	if (e) return e;
	e = send_gavatar(w, w->g[g]);
	return e == WHIMSY_ENET ? WHIMSY_OK : e;
}

const uint8_t *whimsy_group_avatar_get(const struct whimsy *w, size_t g, size_t *n)
{
	return g < w->ng ? av_get(w, w->g[g]->g.id, WHIMSY_GID, 1, n) : NULL;
}

int add_msg(struct whimsy *w, struct grp *gr, const uint8_t sender[32], uint64_t t,
                   uint16_t channel, uint8_t kind, uint32_t index, const uint8_t *reply,
                   const void *text, size_t n)
{
	if (n > WHIMSY_MAX_TEXT) return WHIMSY_EARG;   /* keeps the record under STORE_MAX_REC */
	size_t tn = text_sanitize(NULL, 0, text, n);
	uint8_t *rec = calloc(1, MSGEXT + tn);
	if (!rec) return WHIMSY_ENOMEM;
	memcpy(rec, gr->g.id, WHIMSY_GID);
	st64(rec + WHIMSY_GID, t);
	st16(rec + WHIMSY_GID + 8, channel);
	rec[WHIMSY_GID + 10] = kind | MSGIDX;
	memcpy(rec + WHIMSY_GID + 11, sender, 32);
	st32(rec + MSGHDR, index);
	if (reply) memcpy(rec + MSGHDR + 4, reply, WHIMSY_MSGID);
	text_sanitize(rec + MSGEXT, tn, text, n);

	int e = map_store(store_append(w->st, STORE_MSG, rec, MSGEXT + tn));
	free(rec);
	if (!e) e = push_msg(gr, store_count(w->st) - 1);
	return e;
}

/* every reaction naming a message that is leaving: the record carries the target id,
 * the reactor's key and the text, and is keyed by msgid, so nothing else ever drops it */
static int purge_reacts(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const uint8_t *id,
                        struct idxv *v)
{
	size_t hn = WHIMSY_GID + WHIMSY_MSGID;
	for (size_t i = store_count(w->st); i-- > 0; ) {
		uint8_t k;
		size_t n;
		const uint8_t *p = store_get(w->st, i, &k, &n);
		if (!p || k != STORE_REACT || n < hn) continue;
		if (wc_equal(p, gid, WHIMSY_GID) && wc_equal(p + WHIMSY_GID, id, WHIMSY_MSGID) &&
		    idxv_add(v, i)) return WHIMSY_ENOMEM;
	}
	for (size_t i = w->nrea; i-- > 0; )
		if (wc_equal(w->rea[i].gid, gid, WHIMSY_GID) &&
		    wc_equal(w->rea[i].id, id, WHIMSY_MSGID))
			memmove(&w->rea[i], &w->rea[i + 1], (--w->nrea - i) * sizeof *w->rea);
	return WHIMSY_OK;
}

/* the record under row i leaves the store and the group; every held transfer and every
 * seen high-water that pointed past it slides down with the rows */
int drop_row_v(struct whimsy *w, struct grp *gr, size_t i, struct idxv *v)
{
	uint8_t id[WHIMSY_MSGID];
	size_t mn;
	const uint8_t *m = store_get(w->st, gr->msg[i], NULL, &mn);
	int has_id = m && mn >= MSGEXT && (m[WHIMSY_GID + 10] & MSGIDX);
	if (has_id) memcpy(id, m + WHIMSY_GID + 11, WHIMSY_MSGID);

	/* the row, its reactions, its chunk records and the blob it may still owe the
	 * group all go in one rewrite; nothing here holds a store_get pointer past it */
	size_t mrec = gr->msg[i];
	int e = idxv_add(v, mrec);
	if (e) return e;
	memmove(gr->msg + i, gr->msg + i + 1, (gr->nmsg - i - 1) * sizeof *gr->msg);
	gr->nmsg--;
	for (size_t j = w->nheld; j-- > 0; ) {
		struct xfer *x = w->held[j];
		if (!wc_equal(x->gid, gr->g.id, WHIMSY_GID) || x->row < i) continue;
		if (x->row > i) { x->row--; continue; }
		/* ours and still queued: the chunks never sent leave with the row */
		for (uint32_t k = 0; x->out && k < x->total && !e; k++)
			if (x->out[k] && !(e = idxv_add(v, (size_t)x->out[k])))
				drop_out(w, x->out[k]);
		xfer_free(x);           /* its chunk records go with dead_xfers below */
		memmove(w->held + j, w->held + j + 1, (--w->nheld - j) * sizeof *w->held);
	}
	spool_revoke(w, mrec);          /* what the relay still holds for this row */
	/* a queued blob of this row must not go out after the delete; STORE_OUTBOX2 carries
	 * mrec, so one queued before a restart is caught too */
	for (size_t j = w->nout; j-- > 0 && !e; ) {
		if (w->out[j].mrec != mrec) continue;
		uint64_t oid = w->out[j].id;
		if (!(e = idxv_add(v, (size_t)oid))) drop_out(w, oid);
	}
	/* the high-water is a row index: left alone it would mark the newest message read */
	for (size_t j = 0; j < gr->nseen && !e; j++) {
		if (gr->seen[j].hw <= i) continue;
		e = seen_write(w, gr, &gr->seen[j], gr->seen[j].hw - 1, v);
	}
	if (!e && has_id) e = purge_reacts(w, gr->g.id, id, v);
	if (!e) e = dead_xfers(w, v);          /* a deleted file's plaintext chunks */
	return e;
}

int drop_row(struct whimsy *w, struct grp *gr, size_t i)
{
	struct idxv v = { NULL, 0, 0 };
	int e = drop_row_v(w, gr, i, &v);
	int ve = void_batch(w, &v);
	spool_flush(w);
	return e ? e : ve;
}

/* the message a msgid names, or nmsg. the id is sender pk | index, so a match binds
 * the target's sender too: only that key's own message can carry the id */
size_t find_msgid(const struct whimsy *w, const struct grp *gr, const uint8_t *id)
{
	for (size_t i = gr->nmsg; i-- > 0; ) {
		size_t n;
		const uint8_t *p = store_get(w->st, gr->msg[i], NULL, &n);
		if (!p || n < MSGEXT || !(p[WHIMSY_GID + 10] & MSGIDX)) continue;
		if (wc_equal(p + WHIMSY_GID + 11, id, WHIMSY_MSGID)) return i;
	}
	return gr->nmsg;
}

/* replaces the record a msgid names with the new text, or voids it outright on a delete.
 * the whole store is rewritten, so the text it replaces leaves the disk. no tombstone:
 * the row goes, and a second delete for the same id finds nothing and is WHIMSY_EARG.
 * WHIMSY_ESTATE when sender is not the key that signed the original. */
int apply_change(struct whimsy *w, struct grp *gr, const uint8_t sender[32],
                        const uint8_t *id, const void *text, size_t n, int del)
{
	if (!wc_equal(sender, id, WHIMSY_PK)) return WHIMSY_ESTATE;
	size_t i = find_msgid(w, gr, id);
	if (i == gr->nmsg) return WHIMSY_EARG;
	size_t on;
	const uint8_t *old = store_get(w->st, gr->msg[i], NULL, &on);
	if (!old || on < MSGEXT) return WHIMSY_ECORRUPT;
	if (del) return drop_row(w, gr, i);
	/* a file row's text is its name, and autosave writes it as a path */
	if ((old[WHIMSY_GID + 10] & (uint8_t)~MSGFLAG) != WIRE_K_TEXT) return WHIMSY_ESTATE;
	if (n > WHIMSY_MAX_TEXT) return WHIMSY_EARG;

	size_t tn = text_sanitize(NULL, 0, text, n);
	uint8_t *rec = calloc(1, MSGEXT + tn);
	if (!rec) return WHIMSY_ENOMEM;
	memcpy(rec, old, MSGEXT);
	rec[WHIMSY_GID + 10] |= MSGEDIT;
	if (tn) text_sanitize(rec + MSGEXT, tn, text, n);
	int e = map_store(store_replace(w->st, gr->msg[i], STORE_MSG, rec, MSGEXT + tn));
	free(rec);
	return e;
}

/* our own edit or delete: out to every member, then applied here */
static int change_msg(struct whimsy *w, size_t g, size_t i, const char *text, size_t n, int del)
{
	if (g >= w->ng) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	struct whimsy_msg m;
	int e = whimsy_msg(w, g, i, &m);
	if (e) return e;
	if (!m.mine || !m.id) return WHIMSY_ESTATE;    /* a record older than message ids */

	uint8_t id[WHIMSY_MSGID];
	memcpy(id, m.id, WHIMSY_MSGID);                /* store_replace moves the record */
	size_t len;
	e = emit_build(w, gr, m.channel, del ? WIRE_K_DELETE : WIRE_K_EDIT,
	               (uint64_t)time(NULL), id, text, del ? 0 : n, &len);
	if (e) return e;
	int ae = apply_change(w, gr, w->id.pk, id, text, n, del);
	e = emit_out(w, gr, len, SIZE_MAX);
	return ae ? ae : e;
}

int whimsy_edit(struct whimsy *w, size_t g, size_t i, const char *text, size_t n)
{
	struct whimsy_msg m;
	if (!n || n > WHIMSY_MAX_TEXT) return WHIMSY_EARG;
	/* a file row's text is its name, not something the sender may rewrite */
	if (whimsy_msg(w, g, i, &m) || m.kind != WHIMSY_K_TEXT) return WHIMSY_ESTATE;
	return change_msg(w, g, i, text, n, 0);
}

int whimsy_delete(struct whimsy *w, size_t g, size_t i)
{
	return change_msg(w, g, i, NULL, 0, 1);
}

/* our own last n messages in one go: one DELETE each, then every doomed record --
 * rows, reactions, chunks, owed blobs -- in a single rewrite */
int whimsy_purge(struct whimsy *w, size_t g, uint16_t channel, size_t n)
{
	if (g >= w->ng) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	if (!n || !gr->nmsg) return WHIMSY_OK;
	if (n > gr->nmsg) n = gr->nmsg;

	struct doomed { size_t row; uint16_t chan; uint8_t id[WHIMSY_MSGID]; } *d = calloc(n, sizeof *d);
	if (!d) return WHIMSY_ENOMEM;
	size_t nr = 0;
	/* newest first, so a dropped row never moves one still to come */
	for (size_t i = gr->nmsg; i-- > 0 && nr < n; ) {
		struct whimsy_msg m;
		if (whimsy_msg(w, g, i, &m) || !m.mine || !m.id) continue;
		if (channel && m.channel != channel) continue;
		d[nr].row = i;
		d[nr].chan = m.channel;
		memcpy(d[nr].id, m.id, WHIMSY_MSGID);   /* the record moves under us below */
		nr++;
	}

	uint64_t t = (uint64_t)time(NULL);
	int e = WHIMSY_OK;
	size_t first = nr;                              /* everything from here on was sent */
	while (first > 0) {
		size_t len;
		int be = emit_build(w, gr, d[first - 1].chan, WIRE_K_DELETE, t, d[first - 1].id,
		                    NULL, 0, &len);
		if (be) { e = be; break; }
		first--;
		be = emit_out(w, gr, len, SIZE_MAX);
		if (be) e = be;                         /* offline is queued, not a failure to drop */
	}

	struct idxv v = { NULL, 0, 0 };
	int de = WHIMSY_OK;
	for (size_t j = first; j < nr && !de; j++) de = drop_row_v(w, gr, d[j].row, &v);
	int ve = void_batch(w, &v);
	free(d);
	spool_flush(w);
	return de ? de : (ve ? ve : e);
}

