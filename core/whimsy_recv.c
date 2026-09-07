#include "int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* receiving */

size_t whimsy_msg_count(const struct whimsy *w, size_t g)
{
	return g < w->ng ? w->g[g]->nmsg : 0;
}

int whimsy_seen(struct whimsy *w, size_t g, uint16_t channel, size_t i)
{
	if (g >= w->ng || i >= w->g[g]->nmsg) return WHIMSY_EARG;
	struct grp *gr = w->g[g];
	struct seenslot *sl = seen_slot(gr, channel);
	if (!sl) return WHIMSY_EARG;
	if (i + 1 <= sl->hw) return WHIMSY_OK;
	return seen_write(w, gr, sl, i + 1, NULL);
}

size_t whimsy_unread(const struct whimsy *w, size_t g, uint16_t channel)
{
	if (g >= w->ng) return 0;
	size_t n = 0;
	for (size_t i = whimsy_first_unread(w, g, channel); i < w->g[g]->nmsg; i++) {
		struct whimsy_msg m;
		if (!whimsy_msg(w, g, i, &m) && m.channel == channel && !m.mine && !m.blocked) n++;
	}
	return n;
}

size_t whimsy_first_unread(const struct whimsy *w, size_t g, uint16_t channel)
{
	if (g >= w->ng) return 0;
	size_t nmsg = w->g[g]->nmsg;
	for (size_t i = (size_t)seen_hw(w->g[g], channel); i < nmsg; i++) {
		struct whimsy_msg m;
		if (!whimsy_msg(w, g, i, &m) && m.channel == channel && !m.mine && !m.blocked) return i;
	}
	return nmsg;
}

int whimsy_msg(const struct whimsy *w, size_t g, size_t i, struct whimsy_msg *m)
{
	if (g >= w->ng || i >= w->g[g]->nmsg) return WHIMSY_EARG;
	size_t n;
	const uint8_t *p = store_get(w->st, w->g[g]->msg[i], NULL, &n);
	if (!p || n < MSGHDR) return WHIMSY_ECORRUPT;
	m->time = ld64(p + WHIMSY_GID);
	m->channel = ld16(p + WHIMSY_GID + 8);
	m->kind = p[WHIMSY_GID + 10] & (uint8_t)~MSGFLAG;
	m->edited = (p[WHIMSY_GID + 10] & MSGEDIT) != 0;
	m->sender = p + WHIMSY_GID + 11;
	int ext = (p[WHIMSY_GID + 10] & MSGIDX) && n >= MSGEXT;
	/* sender and index are adjacent, so the id is the record's own bytes */
	m->id = ext ? m->sender : NULL;
	m->reply = ext && !zeroed(p + MSGHDR + 4, WHIMSY_MSGID) ? p + MSGHDR + 4 : NULL;
	m->text = (const char *)p + (ext ? MSGEXT : MSGHDR);
	m->text_n = n - (ext ? MSGEXT : MSGHDR);
	m->mine = wc_equal(m->sender, w->id.pk, 32);
	m->blocked = !m->mine && whimsy_blocked(w, m->sender);
	return WHIMSY_OK;
}

static char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* substring search over sanitized text; ascii case folds, everything else matches
 * byte for byte */
static int contains(const char *h, size_t hn, const char *n, size_t nn)
{
	if (nn > hn) return 0;
	for (size_t i = 0; i + nn <= hn; i++) {
		size_t j = 0;
		while (j < nn && fold(h[i + j]) == fold(n[j])) j++;
		if (j == nn) return 1;
	}
	return 0;
}

size_t whimsy_search(const struct whimsy *w, size_t g, const char *needle,
                     size_t *out, size_t cap)
{
	if (g >= w->ng || !needle || !*needle) return 0;
	char nd[WHIMSY_MAX_TEXT];
	size_t nn = text_sanitize(nd, sizeof nd, needle, strlen(needle));
	if (nn > sizeof nd) return 0;           /* longer than any message can be */
	size_t n = 0;
	for (size_t i = 0; i < w->g[g]->nmsg; i++) {
		struct whimsy_msg m;
		if (whimsy_msg(w, g, i, &m) || m.kind != WIRE_K_TEXT) continue;
		if (!contains(m.text, m.text_n, nd, nn)) continue;
		if (n < cap) out[n] = i;
		n++;
	}
	return n;
}

/* acks for one poll are batched and sent after DONE, so this many u64 must still fit the
 * socket send buffer while the server is not yet reading: raising it risks a mutual stall */
#define FETCH_MAX   512         /* blobs one poll will take before dropping the socket;
                                 * server/whimsyd.c streams fewer than this per fetch */
#define MAX_GROUPS  128         /* a relay can hand us unsolicited senderkeys forever */

static int co_member(const struct whimsy *w, const uint8_t pk[32])
{
	for (size_t g = 0; g < w->ng; g++) if (group_has(&w->g[g]->g, pk)) return 1;
	return 0;
}

/* a sealed senderkey: 1 when it installed a chain, 0 when it is not ours to open */
static int ingest_sealed(struct whimsy *w, const uint8_t *b, size_t n)
{
	uint8_t sender[32];
	struct wire_senderkey sk;
	const uint8_t *inner;
	size_t inner_n;
	struct grp *gr = NULL;
	int r = 0;
	if (group_open(&w->id, b, n, w->pt, sizeof w->pt, sender, &inner, &inner_n)) {
		wc_wipe(w->pt, sizeof w->pt);
		return 0;
	}
	if (inner[0] == WIRE_I_AVATAR) {
		/* theirs, signed by them: no row and no event, the frontend reads it per key.
		 * a stranger who knows our mailbox id could otherwise fill the store */
		if (inner_n - 1 <= WHIMSY_MAX_AVATAR && co_member(w, sender))
			r = av_set(w, sender, WHIMSY_PK, 0, inner + 1, inner_n - 1) ? -1 : 1;
		wc_wipe(w->pt, sizeof w->pt);
		return r;
	}
	if (inner[0] == WIRE_I_SENDERKEY &&
	    wire_decode_senderkey(inner, inner_n, &sk) == WIRE_OK) {
		gr = find_grp(w, sk.group);
		int fresh = !gr;
		/* the fresh branch only: a block never touches a group we already joined */
		if (fresh && whimsy_blocked(w, sk.rec.owner)) {
			wc_wipe(w->pt, sizeof w->pt);
			return 0;
		}
		uint32_t was = fresh ? 0 : gr->g.r.version;
		/* a relay hands out senderkeys for groups we never joined, so the table
		 * that holds them is capped */
		if (fresh && w->ng < MAX_GROUPS && !(gr = add_grp(w))) r = -1;
		int ge = r || !gr ? GROUP_OK : group_recv_senderkey(&gr->g, &w->id, sender, &sk);
		if (gr && !r && !ge && fresh) {   /* gr is NULL past MAX_GROUPS */
			/* nothing this record claims may decide anything before its owner
			 * signature checked out, which is what the join above did: an unsigned
			 * one would otherwise clear the tombstone that keeps us out */
			int64_t gone = gone_ver(w, gr->g.id);
			if (gone >= 0 && (uint64_t)gr->g.r.version <= (uint64_t)gone) {
				drop_last(w);           /* leaving stays left, ack it away */
				wc_wipe(w->pt, sizeof w->pt);
				return 0;
			}
			/* a rejoin is a new epoch: the tombstone and anything of the old one
			 * still keyed to this gid go before the group is saved. the new group
			 * is already in w->g, so pk_kept keeps its members' per-key records */
			if (erase_grp(w, gr->g.id, NULL)) { drop_last(w); r = -1; }
		}
		if (ge == GROUP_EKICKED && !fresh) {
			/* the owner dropped us: nothing was mutated, so the group is still whole
			 * and can be erased. the tombstone stops a replay bringing it back */
			uint8_t gid[WHIMSY_GID];
			memcpy(gid, gr->g.id, WHIMSY_GID);
			size_t i = 0;
			while (i < w->ng && w->g[i] != gr) i++;
			if (erase_grp(w, gid, gr) || gone_write(w, gid, sk.rec.version)) r = -1;
			else { drop_grp(w, i); r = 1; }
		} else if (r || ge) {
			/* a chain the two of us cannot agree on: no re-seal of theirs will ever
			 * install, so ask for a fresh record and a fresh chain instead */
			/* a member's: an ex-member or a stranger who learned the gid would
			 * otherwise drive a group-wide re-seal every HEAL_EVERY for good */
			if (!fresh && gr && group_has(&gr->g, sender) &&
			    (ge == GROUP_EOLD || ge == GROUP_EGROUP || ge == GROUP_EFULL)) {
				memcpy(gr->healpk, sender, WHIMSY_PK);
				gr->heal = 1;
			}
			if (fresh && !r) drop_last(w);   /* a join whose record never named us lands here */
		} else if (gr) {
			/* a newer record rotated our chains too, so everyone gets a fresh one.
			 * not here: a put inside a fetch would read a streamed blob as its reply */
			if (gr->g.r.version != was) gr->stale = gr->relink = 1;
			r = save_group(w, gr) ? -1 : 1;
		}
	}
	wc_wipe(w->pt, sizeof w->pt);   /* the peer's chain key was in there */
	return r;
}

/* 1 when the blob changed something worth redrawing, 0 when it is useless to us and
 * the caller should ack it away, < 0 when a later poll could still use it and the ack
 * must be withheld. only a signed member can reach the < 0 paths, so a stranger cannot
 * park an unackable blob in the mailbox */
static int ingest(struct whimsy *w, const uint8_t *b, size_t n)
{
	struct wire_blob wb;
	if (wire_decode_blob(b, n, &wb) != WIRE_OK) return 0;

	/* nothing on the wire says GROUP or SEALED: the chain id decides, and a blob no
	 * chain claims is tried as a sealed one. the blob names a chain, not a group */
	struct grp *gr = NULL;
	struct group_msg m;
	int e = GROUP_ENOCHAIN;
	/* a cid is attacker-chosen cleartext and can collide across groups, so a chain that
	 * matches the blob but does not open it must not end the search */
	for (size_t i = 0; i < w->ng; i++) {
		int ge = group_recv(&w->g[i]->g, b, n, w->scratch, sizeof w->scratch, &m);
		if (!ge) { gr = w->g[i]; e = GROUP_OK; break; }
		if (e == GROUP_ENOCHAIN) e = ge;
	}

	if (e == GROUP_ENOCHAIN) {
		int r = ingest_sealed(w, b, n);
		if (r) return r;
		/* no chain anywhere: either garbage, or a member sent before their re-seal
		 * reached us. withhold the ack only while some member's chain is missing */
		for (size_t i = 0; i < w->ng; i++)
			if ((size_t)(w->g[i]->g.nrecv + 1) < group_member_count(&w->g[i]->g)) return -2;
		return 0;
	}
	if (e) { wc_wipe(w->scratch, sizeof w->scratch); return 0; }
	if (save_group(w, gr)) { wc_wipe(w->scratch, sizeof w->scratch); return -1; }
	/* a peer over the cap is a peer we disagree with, not a blob to retry: the group
	 * state is saved, the text is dropped */
	/* a channel no record carries would be invisible here and never counted */
	if (!chan_known(gr, m.channel)) { wc_wipe(w->scratch, sizeof w->scratch); return 0; }
	/* above the block guard: refusing a blocked key's heal would be the one outbound
	 * difference a block makes, and so the one way a blocked peer could detect one.
	 * the chain does not rotate here, so nothing is stale and sending stays open;
	 * rate limited, since it is work any member can ask the rest of the group for */
	if (m.kind == WIRE_K_HEAL) {
		clear_typer(w, gr->g.id, m.sender);
		if (time(NULL) - gr->healed >= HEAL_EVERY) {
			gr->healed = time(NULL);
			gr->seal = gr->relink = 1;
		}
		wc_wipe(w->scratch, sizeof w->scratch);
		return 1;
	}
	/* one guard for every kind a blocked key can send. the text is still stored, so
	 * unblocking shows it with no re-fetch; nothing else of theirs touches our state */
	if (whimsy_blocked(w, m.sender)) {
		int be = m.kind == WIRE_K_TEXT && m.payload_n <= WHIMSY_MAX_TEXT &&
		         add_msg(w, gr, m.sender, m.time, m.channel, m.kind, m.index, m.reply,
		                 m.payload, m.payload_n);
		wc_wipe(w->scratch, sizeof w->scratch);
		return be ? -1 : 1;
	}
	int r = 1;
	if (m.kind == WIRE_K_TYPING) {
		note_typer(w, gr->g.id, m.channel, m.sender);
		wc_wipe(w->scratch, sizeof w->scratch);
		return 1;
	}
	clear_typer(w, gr->g.id, m.sender);     /* anything else from them ends the indicator */
	/* not a message: no row, no unread, no event, and the target need not be here */
	if (m.kind == WIRE_K_REACT) {
		int re = save_react(w, gr->g.id, m.reply, m.sender, m.payload, m.payload_n);
		wc_wipe(w->scratch, sizeof w->scratch);
		return re == WHIMSY_ENOMEM || re == WHIMSY_EIO ? -1 : 1;
	}
	if (m.kind == WIRE_K_TEXT && m.payload_n <= WHIMSY_MAX_TEXT &&
	    add_msg(w, gr, m.sender, m.time, m.channel, m.kind, m.index, m.reply,
	            m.payload, m.payload_n))
		r = -1;
	if (m.kind == WIRE_K_FILE && file_recv(w, gr, &m)) r = -1;
	/* the owner's alone: no row, no unread, no event -- it is the group's picture */
	if (m.kind == WIRE_K_GAVATAR && gr->g.rec_n && wc_equal(m.sender, gr->g.r.owner, 32) &&
	    m.payload_n <= WHIMSY_MAX_AVATAR &&
	    av_set(w, gr->g.id, WHIMSY_GID, 1, m.payload, m.payload_n))
		r = -1;
	/* the owner's alone, and the watermark is what travels: the same rows go everywhere */
	if (m.kind == WIRE_K_PURGE && gr->g.rec_n && wc_equal(m.sender, gr->g.r.owner, 32) &&
	    m.payload_n == 8 && apply_purge(w, gr, m.channel, ld64(m.payload)))
		r = -1;
	if (m.kind == WIRE_K_EDIT || m.kind == WIRE_K_DELETE) {
		int ce = apply_change(w, gr, m.sender, m.reply, m.payload, m.payload_n,
		                      m.kind == WIRE_K_DELETE);
		if (ce == WHIMSY_ENOMEM || ce == WHIMSY_EIO) r = -1;
	}
	/* the group signature bound this half to m.sender; a sender past the cap is a
	 * peer we disagree with, not a blob to retry */
	if (r > 0 && m.kind == WIRE_K_LINK && m.payload_n == WHIMSY_PK) {
		int lr = save_link(w, m.sender, m.payload);
		if (lr == WHIMSY_ENOMEM || lr == WHIMSY_EIO) r = -1;
	}
	if (r > 0 && (m.kind == WIRE_K_TEXT || m.kind == WIRE_K_FILE) &&
	    !wc_equal(m.sender, w->id.pk, WHIMSY_PK))
		note_event(gr, m.channel);
	wc_wipe(w->scratch, sizeof w->scratch);
	return r;
}

#define REDIAL_SECS 5

/* an undecodable blob is left on the server only while the chain that would open it may
 * still arrive, and only for HOLD_TRIES polls: past that it is garbage anyone can mint */
static int hold_again(struct whimsy *w, uint64_t seq)
{
	for (size_t i = 0; i < w->nhold; i++)
		if (w->hold[i].seq == seq) {
			if (++w->hold[i].tries < HOLD_TRIES) return 1;
			w->hold[i] = w->hold[--w->nhold];
			return 0;
		}
	if (w->nhold == HOLDS) return 0;
	w->hold[w->nhold].seq = seq;
	w->hold[w->nhold++].tries = 1;
	return 1;
}

int whimsy_poll(struct whimsy *w)
{
	if (!w->n) {
		time_t now = time(NULL);
		if (!w->registered || now < w->redial) return WHIMSY_ENET;
		w->redial = now + REDIAL_SECS;
		if (whimsy_connect(w, NULL)) return WHIMSY_ENET;
	}
	for (size_t i = 0; i < w->ng; i++) w->g[i]->nevt = 0;
	int fe = flush_out(w);          /* before the fetch: a put inside one would read a streamed blob */
	if (fe) return fe;
	spool_flush(w);                 /* same reason, and it retries what an offline delete marked */
	if (!w->n) return WHIMSY_ENET;
	struct wire_frame f = { .type = WIRE_F_FETCH };
	if (snd(w, &f)) return WHIMSY_ENET;

	int changed = 0;
	uint64_t ack[FETCH_MAX];
	size_t nack = 0;
	w->instream = 1;                /* a delete applied by ingest only marks its revokes */
	for (int i = 0; i < FETCH_MAX; i++) {
		if (rcv(w, &f)) { w->instream = 0; return WHIMSY_ENET; }
		if (f.type == WIRE_F_DONE) break;
		if (f.type != WIRE_F_BLOB) { w->instream = 0; return WHIMSY_EPROTO; }
		int r = ingest(w, f.blob.blob, f.blob.blob_n);
		if (r == -2 && !hold_again(w, f.blob.seq)) r = 0;
		if (r < 0) continue;            /* leave it on the server for the next poll */
		changed += r;
		ack[nack++] = f.blob.seq;
		/* the stream is unbounded: drop it unacked, the server keeps it for the next poll.
		 * acking here would write into a server still streaming and not reading us */
		if (i + 1 == FETCH_MAX) {
			net_close(w->n);
			w->n = NULL;
			w->instream = 0;
			return changed;
		}
	}
	w->instream = 0;
	/* only now: while the server was streaming it may have stopped reading us */
	for (size_t i = 0; i < nack; i++) {
		struct wire_frame a = { .type = WIRE_F_ACK, .ack = { ack[i] } };
		if (snd(w, &a)) return WHIMSY_ENET;
	}
	if (nack) {     /* the pong proves the acks were read, so a caller may look at the mailbox */
		struct wire_frame p = { .type = WIRE_F_PING };
		if (snd(w, &p) || rcv(w, &p)) return WHIMSY_ENET;
		if (p.type != WIRE_F_PONG) return WHIMSY_EPROTO;
	}
	for (size_t i = 0; i < w->ng; i++) {
		int e;
		if (w->g[i]->stale || w->g[i]->seal) {
			e = reseal(w, w->g[i]);
			if (e == WHIMSY_ENET) return WHIMSY_ENET;
			if (!e) {
				w->g[i]->seal = 0;
				if (w->g[i]->stale) { w->g[i]->stale = 0; save_group(w, w->g[i]); }
			}
		}
		if (w->g[i]->heal) {
			e = heal_ask(w, w->g[i]);
			if (e == WHIMSY_ENET) return WHIMSY_ENET;
			if (!e) w->g[i]->heal = 0;
		}
		if (w->g[i]->relink && !w->g[i]->stale) {
			e = publish_links(w, w->g[i]);
			if (e == WHIMSY_ENET) return WHIMSY_ENET;
			if (!e) w->g[i]->relink = 0;
		}
	}
	/* the sent set makes this a no-op once everyone owed has one */
	if (push_avatars(w) == WHIMSY_ENET) return WHIMSY_ENET;
	return changed;
}
