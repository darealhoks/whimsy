#include "int.h"
#include "plat.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* reactions */

/* every row in the channel at or before the watermark. the owner signed the watermark,
 * so two members drop the same set whatever order their rows arrived in */
int apply_purge(struct whimsy *w, struct grp *gr, uint16_t channel, uint64_t before)
{
	struct idxv v = { NULL, 0, 0 };
	int e = WHIMSY_OK;
	for (size_t i = gr->nmsg; i-- > 0 && !e; ) {   /* newest first: a drop never moves a row still to come */
		size_t n;
		const uint8_t *p = store_get(w->st, gr->msg[i], NULL, &n);
		if (!p || n < MSGHDR) continue;
		if (ld16(p + WHIMSY_GID + 8) != channel || ld64(p + WHIMSY_GID) > before) continue;
		e = drop_row_v(w, gr, i, &v);              /* p dies with the batch, not before */
	}
	int ve = void_batch(w, &v);
	spool_flush(w);
	return e ? e : ve;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

int whimsy_purge_all(struct whimsy *w, size_t g, uint16_t channel, size_t n)
{
	if (g >= w->ng || !channel || !n) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	if (!whimsy_is_owner(w, g)) return WHIMSY_ESTATE;
	if (!chan_known(gr, channel)) return WHIMSY_EARG;

	uint64_t *t = calloc(gr->nmsg ? gr->nmsg : 1, sizeof *t);
	if (!t) return WHIMSY_ENOMEM;
	size_t nt = 0;
	for (size_t i = 0; i < gr->nmsg; i++) {
		struct whimsy_msg m;
		if (!whimsy_msg(w, g, i, &m) && m.channel == channel) t[nt++] = m.time;
	}
	if (!nt) { free(t); return WHIMSY_OK; }
	qsort(t, nt, sizeof *t, cmp_u64);
	/* the watermark is a second-resolution timestamp and the predicate is <=, so rows
	 * sharing the n-th oldest one all go: walk back off a tie rather than drop past n */
	size_t k = n > nt ? nt : n;
	while (k && k < nt && t[k] == t[k - 1]) k--;
	if (!k) { free(t); return WHIMSY_OK; }
	uint64_t before = t[k - 1];
	free(t);

	uint8_t pay[8];
	size_t len;
	st64(pay, before);
	int e = emit_build(w, gr, channel, WIRE_K_PURGE, (uint64_t)time(NULL), NULL, pay,
	                   sizeof pay, &len);
	if (e) return e;
	int ae = apply_purge(w, gr, channel, before);
	e = emit_out(w, gr, len, SIZE_MAX);
	return ae ? ae : e;
}

/* text_sanitize drops zero-width codepoints and caps the marks on a base, so what is
 * left of a grapheme is one codepoint that occupies cells plus its marks */
static size_t graphemes(const char *t, size_t n)
{
	size_t g = 0;
	for (size_t i = 0; i < n; ) {
		uint32_t cp;
		i += text_step(t + i, n - i, &cp);
		if (text_cp_width(cp)) g++;
	}
	return g;
}

int whimsy_react(struct whimsy *w, size_t g, const uint8_t *id, const char *emoji, size_t n)
{
	if (g >= w->ng || !id) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	char t[WHIMSY_MAX_REACT];
	size_t tn = emoji ? text_sanitize(t, sizeof t, emoji, n) : 0;
	if (tn > sizeof t) return WHIMSY_EARG;
	if ((emoji && n) && (!tn || graphemes(t, tn) != 1)) return WHIMSY_EARG;

	/* the channel the target sits in, so a receiver that renders per channel places it;
	 * a reaction on a message we never got goes out on the group's first channel */
	size_t i = find_msgid(w, gr, id);
	uint16_t ch = gr->g.r.nchan ? gr->g.r.chan[0].id : 0;
	if (i != gr->nmsg) {
		struct whimsy_msg m;
		if (!whimsy_msg(w, g, i, &m)) ch = m.channel;
	}

	size_t len;
	int e = emit_build(w, gr, ch, WIRE_K_REACT, (uint64_t)time(NULL), id, t, tn, &len);
	if (e) return e;
	int re = save_react(w, gr->g.id, id, w->id.pk, t, tn);
	e = emit_out(w, gr, len, SIZE_MAX);
	return re ? re : e;
}

size_t whimsy_reactions(const struct whimsy *w, size_t g, size_t i,
                        struct whimsy_react *out, size_t cap)
{
	struct whimsy_msg m;
	if (whimsy_msg(w, g, i, &m) || !m.id) return 0;
	size_t n = 0;
	for (size_t j = 0; j < w->nrea; j++) {
		if (!wc_equal(w->rea[j].gid, w->g[g]->g.id, WHIMSY_GID) ||
		    !wc_equal(w->rea[j].id, m.id, WHIMSY_MSGID)) continue;
		if (n < cap) {
			out[n].sender = w->rea[j].pk;
			out[n].text = w->rea[j].t;
			out[n].text_n = w->rea[j].n;
		}
		n++;
	}
	return n;
}

int whimsy_drop(struct whimsy *w, size_t g, size_t i)
{
	if (g >= w->ng || i >= w->g[g]->nmsg) return WHIMSY_EARG;
	return drop_row(w, w->g[g], i);
}

int whimsy_group_new(struct whimsy *w, const char *name,
                     const char *const *channels, uint8_t nchan)
{
	struct grp *gr = add_grp(w);
	if (!gr) return WHIMSY_ENOMEM;
	int e = map_group(group_create(&gr->g, &w->id, name, channels, nchan));
	if (!e) e = save_group(w, gr);
	if (e) drop_last(w);
	return e;
}


static int change(struct whimsy *w, size_t g, const uint8_t pk[32], int add)
{
	if (g >= w->ng) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	int e = map_group(add ? group_add(&gr->g, &w->id, pk) : group_kick(&gr->g, &w->id, pk));
	if (e) return e;
	gr->stale = gr->relink = 1;
	if ((e = save_group(w, gr))) return e;
	/* the queue holds blobs on the chain this change just replaced: a member who gets
	 * the fresh chain first drops the old one and can never open them */
	e = flush_out(w);
	if (e) return e;
	e = reseal(w, gr);
	if (e) return e;
	gr->stale = 0;
	if ((e = save_group(w, gr))) return e;
	if (!(e = publish_links(w, gr))) gr->relink = 0;
	if (e) return e;
	return add ? send_gavatar(w, gr) : WHIMSY_OK;
}

int whimsy_channel(struct whimsy *w, size_t g, char op, const char *a, const char *b)
{
	if (g >= w->ng || !a) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	int e = group_chan(&gr->g, &w->id, op, a, b);
	if (e == GROUP_EGROUP) return WHIMSY_EARG;    /* bad op, duplicate, unknown, or the last channel */
	if ((e = map_group(e))) return e;
	gr->stale = 1;
	if ((e = save_group(w, gr))) return e;
	if ((e = flush_out(w))) return e;
	if (!(e = reseal(w, gr))) { gr->stale = 0; e = save_group(w, gr); }
	return e;
}

int whimsy_group_rename(struct whimsy *w, size_t g, const char *name)
{
	if (g >= w->ng || !name) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	int e = map_group(group_rename(&gr->g, &w->id, name));
	if (e) return e;
	gr->stale = 1;
	if ((e = save_group(w, gr))) return e;
	if ((e = flush_out(w))) return e;
	if (!(e = reseal(w, gr))) { gr->stale = 0; e = save_group(w, gr); }
	return e;
}

/* pk that g names, or one device-link hop from a key g names */
static int grp_near(const struct whimsy *w, const struct grp *g, const uint8_t pk[WHIMSY_PK])
{
	if (group_has(&g->g, pk)) return 1;
	for (size_t i = 0; i < w->nlnk; i++) {
		const uint8_t *o = NULL;
		if (wc_equal(w->lnk[i].who, pk, WHIMSY_PK)) o = w->lnk[i].other;
		else if (wc_equal(w->lnk[i].other, pk, WHIMSY_PK)) o = w->lnk[i].who;
		if (o && group_has(&g->g, o)) return 1;
	}
	return 0;
}

/* a key whose per-key records stay: our own, a device of ours, or one a group other
 * than skip still names -- directly or one device link away.
 * ponytail: linear in groups times links per key, run once per erase */
static int pk_kept(const struct whimsy *w, const struct grp *skip, const uint8_t pk[WHIMSY_PK])
{
	if (wc_equal(pk, w->id.pk, WHIMSY_PK)) return 1;
	for (size_t i = 0; i < w->nlnk; i++)
		if ((wc_equal(w->lnk[i].who, pk, WHIMSY_PK) &&
		     wc_equal(w->lnk[i].other, w->id.pk, WHIMSY_PK)) ||
		    (wc_equal(w->lnk[i].other, pk, WHIMSY_PK) &&
		     wc_equal(w->lnk[i].who, w->id.pk, WHIMSY_PK)))
			return 1;
	for (size_t i = 0; i < w->ng; i++)
		if (w->g[i] != skip && grp_near(w, w->g[i], pk)) return 1;
	return 0;
}

/* a per-key record goes only when the group being erased is what named its key and
 * nothing we keep still does. skip NULL -- a rejoin -- names nobody, so a key the
 * user set a petname or a verification on off any group is never touched */
static int pk_gone(const struct whimsy *w, const struct grp *skip, const uint8_t pk[WHIMSY_PK])
{
	return skip && grp_near(w, skip, pk) && !pk_kept(w, skip, pk);
}

/* every record keyed to this group id is voided in place, then the group goes. the
 * per-key records -- petname, link, verify, avatar, avatar-sent -- go by pk_gone */
int erase_grp(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const struct grp *skip)
{
	struct idxv v = { NULL, 0, 0 };
	int e = WHIMSY_OK;
	for (size_t i = 0; i < store_count(w->st) && !e; i++) {
		uint8_t kind;
		size_t n;
		const uint8_t *p = store_get(w->st, i, &kind, &n);
		int hit = 0;
		switch (kind) {
		case STORE_GROUP: {
			size_t rec_n = n < 2 ? 0 : ld16(p) & 0x7fff;
			struct wire_rec r;
			if (rec_n && rec_n <= GROUP_MAX_REC && n >= 2 + rec_n &&
			    wire_decode_rec(p + 2, rec_n, &r) == WIRE_OK) {
				uint8_t id[WHIMSY_GID];
				group_id(id, r.owner, r.salt);
				hit = wc_equal(id, gid, WHIMSY_GID);
			}
			break;
		}
		case STORE_OUTBOX:
		case STORE_OUTBOX2:
		case STORE_MSG:
		case STORE_SEEN:
		case STORE_MUTE:
		case STORE_REACT:
		case STORE_XFER:
		case STORE_GAVATAR:
		case STORE_GONE:
			hit = n >= WHIMSY_GID && wc_equal(p, gid, WHIMSY_GID);
			break;
		case STORE_PETNAME:
		case STORE_AVATAR:
		case STORE_VERIFY:
		case STORE_ASENT:
			hit = n >= WHIMSY_PK && pk_gone(w, skip, p);
			break;
		case STORE_LINK:
			hit = n >= 2 * WHIMSY_PK && pk_gone(w, skip, p) &&
			      pk_gone(w, skip, p + WHIMSY_PK);
			break;
		default:
			break;
		}
		if (!hit) continue;
		if ((e = idxv_add(&v, i))) break;
	}
	int ve = void_batch(w, &v);
	if (e || ve) return e ? e : ve;
	for (size_t i = w->nav; i-- > 0; )
		if (w->av[i].grp ? wc_equal(w->av[i].key, gid, WHIMSY_GID)
		                 : pk_gone(w, skip, w->av[i].key))
			w->av[i] = w->av[--w->nav];
	for (size_t i = w->npet; i-- > 0; )
		if (pk_gone(w, skip, w->pet[i].pk)) w->pet[i] = w->pet[--w->npet];
	for (size_t i = w->nver; i-- > 0; )
		if (pk_gone(w, skip, w->ver[i])) memcpy(w->ver[i], w->ver[--w->nver], WHIMSY_PK);
	for (size_t i = w->nsent; i-- > 0; )
		if (pk_gone(w, skip, w->sent[i])) memcpy(w->sent[i], w->sent[--w->nsent], WHIMSY_PK);
	for (size_t i = w->nlnk; i-- > 0; )
		if (pk_gone(w, skip, w->lnk[i].who) && pk_gone(w, skip, w->lnk[i].other))
			w->lnk[i] = w->lnk[--w->nlnk];
	return WHIMSY_OK;
}

/* the version of the tombstone for gid, or -1 when there is none */
int64_t gone_ver(const struct whimsy *w, const uint8_t gid[WHIMSY_GID])
{
	int64_t v = -1;
	for (size_t i = 0; i < store_count(w->st); i++) {
		uint8_t kind;
		size_t n;
		const uint8_t *p = store_get(w->st, i, &kind, &n);
		if (!p || kind != STORE_GONE || n != WHIMSY_GID + 8) continue;
		if (wc_equal(p, gid, WHIMSY_GID)) v = (int64_t)ld64(p + WHIMSY_GID);
	}
	return v;
}

/* leaving is a state, not an absence: without this any later senderkey -- a replay
 * included -- rematerialises the group */
int gone_write(struct whimsy *w, const uint8_t gid[WHIMSY_GID], uint64_t version)
{
	uint8_t rec[WHIMSY_GID + 8];
	memcpy(rec, gid, WHIMSY_GID);
	st64(rec + WHIMSY_GID, version);
	return map_store(store_append(w->st, STORE_GONE, rec, sizeof rec));
}

int whimsy_group_leave(struct whimsy *w, size_t g)
{
	if (g >= w->ng) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	if (gr->g.rec_n && wc_equal(w->id.pk, gr->g.r.owner, 32)) {
		while (gr->g.r.nmemb > 1) {
			const uint8_t *m = gr->g.r.members;
			if (wc_equal(m, w->id.pk, 32)) m += 32;
			uint8_t pk[32];
			memcpy(pk, m, 32);
			int e = whimsy_group_kick(w, g, pk);
			if (e) return e;
		}
	}
	uint8_t gid[WHIMSY_GID];
	memcpy(gid, gr->g.id, WHIMSY_GID);
	uint64_t ver = gr->g.rec_n ? gr->g.r.version : 0;
	int e = erase_grp(w, gid, gr);
	if (!e) e = gone_write(w, gid, ver);
	if (e) return e;
	drop_grp(w, g);
	return WHIMSY_OK;
}

/* everything this device knows about anyone: our own messages retracted where the
 * relay still holds them, every group left, then every record that is not this
 * device itself. what stays is the identity, the server and the settings, plus the
 * tombstones -- without those the next poll rebuilds a group we just left. a copy a
 * member already fetched is theirs and no part of this reaches it */
int whimsy_nuke(struct whimsy *w)
{
	int fail = WHIMSY_OK;
	while (w->ng) {
		size_t g = w->ng - 1;
		struct grp *gr = w->g[g];
		uint8_t gid[WHIMSY_GID];
		uint64_t ver = gr->g.rec_n ? gr->g.r.version : 0;
		memcpy(gid, gr->g.id, WHIMSY_GID);
		/* the DELETEs and the revokes they mark need the chain, so before the erase */
		int e = whimsy_purge(w, g, 0, gr->nmsg);
		if (e && !fail) fail = e;
		if (!(e = whimsy_group_leave(w, g))) continue;
		if (!fail) fail = e;
		/* offline the owner's kicks cannot finish; the group still goes */
		if (!(e = erase_grp(w, gid, gr))) e = gone_write(w, gid, ver);
		if (e) return e;
		drop_grp(w, g);
	}

	struct idxv v = { NULL, 0, 0 };
	int e = WHIMSY_OK;
	for (size_t i = 0; i < store_count(w->st) && !e; i++) {
		uint8_t k;
		/* a key no group ever named keeps its petname past erase_grp: nothing
		 * is kept now, so the kind alone decides */
		if (!store_get(w->st, i, &k, NULL)) continue;
		if (k == STORE_IDENTITY || k == STORE_SERVER || k == STORE_SETTING ||
		    k == STORE_GONE || k == STORE_VOID) continue;
		e = idxv_add(&v, i);
	}
	int ve = void_batch(w, &v);
	if (!fail) fail = e ? e : ve;

	if (w->npet) wc_wipe(w->pet, w->npet * sizeof *w->pet);
	if (w->nrea) wc_wipe(w->rea, w->nrea * sizeof *w->rea);
	w->npet = w->nrea = w->nlnk = w->nver = w->nav = w->nsent = w->ntyp = 0;
	spool_flush(w);
	return fail;
}

int whimsy_group_add(struct whimsy *w, size_t g, const uint8_t pk[WHIMSY_PK])
{
	return change(w, g, pk, 1);
}

int whimsy_group_kick(struct whimsy *w, size_t g, const uint8_t pk[WHIMSY_PK])
{
	return change(w, g, pk, 0);
}

int chan_known(const struct grp *gr, uint16_t id)
{
	for (size_t c = 0; gr->g.rec_n && c < gr->g.r.nchan; c++)
		if (gr->g.r.chan[c].id == id) return 1;
	return 0;
}

int whimsy_link(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	int e = save_link(w, w->id.pk, pk);
	if (e) return e;
	int fail = WHIMSY_OK;
	for (size_t g = 0; g < w->ng; g++) {
		int r = publish_links(w, w->g[g]);
		if (r) fail = r;
	}
	return fail;
}

int whimsy_unlink(struct whimsy *w, const uint8_t pk[WHIMSY_PK])
{
	if (!wc_pk_ok(pk)) return WHIMSY_EARG;
	if (!declared(w, w->id.pk, pk)) return WHIMSY_OK;
	int e = drop_link(w, w->id.pk, pk);
	if (e) return e;
	int fail = WHIMSY_OK;
	for (size_t g = 0; g < w->ng; g++) {
		int r = revoke_link(w, w->g[g], pk);
		if (r) fail = r;
	}
	return fail;
}

int whimsy_send(struct whimsy *w, size_t g, uint16_t channel, const char *text, size_t n)
{
	return whimsy_send_reply(w, g, channel, NULL, text, n);
}

int whimsy_send_reply(struct whimsy *w, size_t g, uint16_t channel,
                      const uint8_t *to, const char *text, size_t n)
{
	if (g >= w->ng || n > WHIMSY_MAX_TEXT) return WHIMSY_EARG;
	if (!chan_known(w->g[g], channel)) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	uint64_t t = (uint64_t)time(NULL);
	uint32_t index = gr->g.send.base;       /* the index group_send is about to use */
	size_t len;

	int e = emit_build(w, gr, channel, WIRE_K_TEXT, t, to, text, n, &len);
	if (e) return e;
	int se = add_msg(w, gr, w->id.pk, t, channel, WIRE_K_TEXT, index, to, text, n);
	/* the row's own store index, not its chain index: rotate() restarts the chain, so
	 * a msgid alone would match a later epoch's message too */
	e = emit_out(w, gr, len, se ? SIZE_MAX : gr->msg[gr->nmsg - 1]);
	return se ? se : e;
}

/* typing */

int whimsy_typing(struct whimsy *w, size_t g, uint16_t channel)
{
	if (g >= w->ng || !chan_known(w->g[g], channel)) return WHIMSY_EARG;
	if (strcmp(w->set[WHIMSY_TYPING_SEND], "1")) return WHIMSY_OK;
	struct grp *gr = w->g[g];
	time_t now = time(NULL);
	if (gr->typed_ch == channel && now - gr->typed_at < TYPING_EVERY) return WHIMSY_OK;
	/* before the chain steps: an indicator outlived by TYPING_TTL must never reach the
	 * outbox, so offline or with a queue standing there is nothing to send */
	if (!w->n || w->nout) return WHIMSY_OK;

	size_t len;
	int e = emit_build(w, gr, channel, WIRE_K_TYPING, (uint64_t)now, NULL, NULL, 0, &len);
	if (e) return e;
	gr->typed_ch = channel;
	gr->typed_at = now;
	e = fan_out(w, gr, w->blob, len, SIZE_MAX);
	return e == WHIMSY_ENET ? WHIMSY_OK : e;
}

void clear_typer(struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                        const uint8_t pk[WHIMSY_PK])
{
	for (size_t i = 0; i < w->ntyp; i++)
		if (wc_equal(w->typ[i].gid, gid, WHIMSY_GID) &&
		    wc_equal(w->typ[i].pk, pk, WHIMSY_PK)) w->typ[i].at = 0;
}

void note_typer(struct whimsy *w, const uint8_t gid[WHIMSY_GID], uint16_t chan,
                       const uint8_t pk[WHIMSY_PK])
{
	size_t slot = TYPERS, old = 0;
	for (size_t i = 0; i < w->ntyp; i++) {
		struct typer *t = &w->typ[i];
		if (t->chan == chan && wc_equal(t->gid, gid, WHIMSY_GID) &&
		    wc_equal(t->pk, pk, WHIMSY_PK)) { slot = i; break; }
		if (t->at < w->typ[old].at) old = i;
	}
	if (slot == TYPERS) slot = w->ntyp < TYPERS ? w->ntyp++ : old;
	memcpy(w->typ[slot].gid, gid, WHIMSY_GID);
	memcpy(w->typ[slot].pk, pk, WHIMSY_PK);
	w->typ[slot].chan = chan;
	w->typ[slot].at = time(NULL);
}

size_t whimsy_typers(const struct whimsy *w, size_t g, uint16_t channel,
                     uint8_t *out, size_t cap)
{
	if (g >= w->ng) return 0;
	time_t now = time(NULL);
	size_t n = 0;
	for (size_t i = 0; i < w->ntyp && n < cap; i++) {
		const struct typer *t = &w->typ[i];
		if (t->chan != channel || !wc_equal(t->gid, w->g[g]->g.id, WHIMSY_GID)) continue;
		if (now - t->at >= TYPING_TTL) continue;
		if (wc_equal(t->pk, w->id.pk, WHIMSY_PK)) continue;
		memcpy(out + n * WHIMSY_PK, t->pk, WHIMSY_PK);
		n++;
	}
	return n;
}

/* files */

/* the sender's name with any directory dropped, sanitized, never empty and never a
 * path: a received name reaches both the screen and file_autosave through here */
static size_t clean_name(char *out, size_t cap, const void *in, size_t n)
{
	const char *s = in, *base = s;
	for (size_t i = 0; i < n; i++) if (s[i] == '/') base = s + i + 1;
	n -= (size_t)(base - s);
	size_t w = text_sanitize(out, cap, base, n);
	term(out, cap, w);
	if (!out[0] || !strcmp(out, ".") || !strcmp(out, "..")) {
		snprintf(out, cap, "file");
		return strlen(out);
	}
	return strlen(out);
}

static int write_new(const char *path, const uint8_t *b, size_t n)
{
	int fd = plat_open(path, O_WRONLY | O_CREAT | O_EXCL | PLAT_PRIVATE);
	if (fd < 0) return WHIMSY_EIO;
	for (size_t o = 0; o < n; ) {
		ssize_t k = write(fd, b + o, n - o);
		if (k <= 0) { close(fd); unlink(path); return WHIMSY_EIO; }
		o += (size_t)k;
	}
	return close(fd) ? WHIMSY_EIO : WHIMSY_OK;
}

/* the file_autosave knob, off by default: a completed file also lands in <dir>/files.
 * a name already taken there is left alone rather than overwritten, and a failure here
 * is never a failure to receive */
static void autosave(struct whimsy *w, const struct xfer *x)
{
	char dir[4096], path[4096], name[256];
	whimsy_get(w, WHIMSY_FILE_AUTOSAVE, name, sizeof name);
	if (name[0] != '1') return;
	if (snprintf(dir, sizeof dir, "%s/files", w->dir) >= (int)sizeof dir) return;
	plat_mkdir(dir);
	struct whimsy_msg m;
	size_t g = 0;
	for (; g < w->ng; g++) if (wc_equal(w->g[g]->g.id, x->gid, WHIMSY_GID)) break;
	if (g == w->ng || whimsy_msg(w, g, x->row, &m)) return;
	if (m.text_n >= sizeof name) return;
	memcpy(name, m.text, m.text_n);
	name[m.text_n] = 0;
	if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return;
	write_new(path, x->b, x->n);
}

/* one FILE chunk off the wire. the first chunk of a file to arrive opens the row and
 * the buffer; a repeat inside the same transfer is ignored */
int file_recv(struct whimsy *w, struct grp *gr, const struct group_msg *m)
{
	struct wire_file f;
	if (wire_decode_file(m->payload, m->payload_n, &f) != WIRE_OK) return WHIMSY_OK;
	struct xfer *x = xfer_find(w, gr->g.id, f.fid, m->sender);
	if (!x) {
		char name[256];
		size_t nn = clean_name(name, sizeof name, f.name, f.name_n);
		int e = add_msg(w, gr, m->sender, m->time, m->channel, WIRE_K_FILE,
		                m->index, NULL, name, nn);
		if (e) return e;
		if (!(x = xfer_add(w, gr->g.id, f.fid, m->sender, m->index, gr->nmsg - 1, f.total)))
			return WHIMSY_ENOMEM;
		prune_xfers(w);
	}
	/* a second chunk claiming a different length is the same file id used twice: the
	 * transfer keeps the shape the first chunk set */
	if (f.total != x->total || f.idx >= x->total) return WHIMSY_OK;
	if (x->seen[f.idx / 8] & 1 << (f.idx % 8)) return WHIMSY_OK;
	x->seen[f.idx / 8] |= (uint8_t)(1 << (f.idx % 8));
	size_t off = (size_t)f.idx * WIRE_FILE_CHUNK;
	memcpy(x->b + off, f.bytes, f.bytes_n);
	if (off + f.bytes_n > x->n) x->n = off + f.bytes_n;
	/* before the ack: a chunk we cannot store is one we must be sent again */
	int se = save_chunk(w, x, f.idx, f.bytes, f.bytes_n);
	if (se) { x->seen[f.idx / 8] &= (uint8_t)~(1 << (f.idx % 8)); return se; }
	if (++x->got == x->total) autosave(w, x);
	return WHIMSY_OK;
}

int whimsy_send_file(struct whimsy *w, size_t g, uint16_t channel, const char *path)
{
	if (g >= w->ng || !path) return WHIMSY_EARG;
	if (!chan_known(w->g[g], channel)) return WHIMSY_EARG;
	struct grp *gr = w->g[g];

	int fd = plat_open(path, O_RDONLY);
	if (fd < 0) return WHIMSY_EIO;
	long long sz = 0;
	int wr;    /* an attachment we read out: world-readable is fine */
	if (plat_is_regular_private(fd, &sz, &wr) || !sz || sz > WHIMSY_MAX_FILE) {
		close(fd);
		return sz > WHIMSY_MAX_FILE ? WHIMSY_EARG : WHIMSY_EIO;
	}
	size_t n = (size_t)sz;
	uint8_t *b = malloc(n);
	if (!b) { close(fd); return WHIMSY_ENOMEM; }
	for (size_t o = 0; o < n; ) {
		ssize_t k = read(fd, b + o, n - o);
		if (k <= 0) { free(b); close(fd); return WHIMSY_EIO; }
		o += (size_t)k;
	}
	close(fd);

	char name[256];
	size_t nn = clean_name(name, sizeof name, path, strlen(path));
	uint32_t total = (uint32_t)((n + WIRE_FILE_CHUNK - 1) / WIRE_FILE_CHUNK);
	uint8_t fid[16];
	wc_random(fid, sizeof fid);
	uint64_t t = (uint64_t)time(NULL);

	int e = add_msg(w, gr, w->id.pk, t, channel, WIRE_K_FILE, gr->g.send.base, NULL, name, nn);
	if (e) { free(b); return e; }
	size_t mrec = gr->msg[gr->nmsg - 1];    /* every chunk belongs to this row, so a delete
	                                           after a restart catches them too */
	struct xfer *x = xfer_add(w, gr->g.id, fid, w->id.pk, gr->g.send.base, gr->nmsg - 1, total);
	prune_xfers(w);
	if (!x || !(x->out = calloc(total, sizeof *x->out))) { free(b); return WHIMSY_ENOMEM; }
	memcpy(x->b, b, n);
	free(b);
	x->n = n;
	x->got = total;
	memset(x->seen, 0xff, sizeof x->seen);

	uint8_t chunk[WIRE_FILE_HDR + 255 + WIRE_FILE_CHUNK];
	for (uint32_t i = 0; i < total; i++) {
		size_t off = (size_t)i * WIRE_FILE_CHUNK, left = x->n - off;
		struct wire_file f = { fid, (const uint8_t *)name, x->b + off, i, total,
		                       (uint8_t)nn, left < WIRE_FILE_CHUNK ? left : WIRE_FILE_CHUNK };
		size_t cn, len;
		if (wire_encode_file(chunk, sizeof chunk, &cn, &f) != WIRE_OK) return WHIMSY_EARG;
		if ((e = save_chunk(w, x, i, x->b + off, f.bytes_n))) return e;
		if ((e = emit_build(w, gr, channel, WIRE_K_FILE, t, NULL, chunk, cn, &len))) return e;
		size_t before = w->nout;
		if ((e = emit_out(w, gr, len, mrec))) return e;
		/* queued rather than put: remember which outbox record is this chunk, so
		 * progress is what left the machine and not what was built */
		if (w->nout > before) x->out[i] = w->out[w->nout - 1].id;
	}
	return WHIMSY_OK;
}

const uint8_t *whimsy_file_open(const struct whimsy *w, size_t g, size_t i, size_t *n)
{
	struct whimsy_msg m;
	if (g >= w->ng || whimsy_msg(w, g, i, &m) || m.kind != WHIMSY_K_FILE) return NULL;
	const struct xfer *x = xfer_row(w, w->g[g]->g.id, i);
	if (!x || x->got != x->total) return NULL;
	*n = x->n;
	return x->b;
}

int whimsy_file_save(const struct whimsy *w, size_t g, size_t i, const char *path)
{
	size_t n;
	const uint8_t *b = whimsy_file_open(w, g, i, &n);
	if (!b) return WHIMSY_ESTATE;
	return write_new(path, b, n);
}

void whimsy_file_progress(const struct whimsy *w, size_t g, size_t i,
                          size_t *done, size_t *total)
{
	*done = *total = 0;
	if (g >= w->ng) return;
	const struct xfer *x = xfer_row(w, w->g[g]->g.id, i);
	if (!x) return;
	*total = x->total;
	if (!x->out) { *done = x->got; return; }
	/* ours: a chunk is done once its outbox record is gone, which is delivery. 0 is the
	 * store index of the IDENTITY record and so never an outbox one: it means never queued */
	for (uint32_t k = 0; k < x->total; k++) {
		int queued = 0;
		for (size_t j = 0; j < w->nout && !queued; j++) queued = w->out[j].id == x->out[k];
		*done += !queued;
	}
}

