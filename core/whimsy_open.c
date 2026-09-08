#include "int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
/* notifications */

void note_event(struct grp *gr, uint16_t chan, int mention)
{
	for (size_t i = 0; i < gr->nevt; i++)
		if (gr->evt[i] == chan) { gr->ment[i] |= mention != 0; return; }
	if (gr->nevt == WIRE_MAX_CHANNELS) return;
	gr->ment[gr->nevt] = mention != 0;
	gr->evt[gr->nevt++] = chan;
}

size_t whimsy_events(const struct whimsy *w, struct whimsy_event *out, size_t cap)
{
	size_t n = 0;
	for (size_t g = 0; g < w->ng; g++) {
		int lv = w->g[g]->notify;
		if (lv == WHIMSY_N_MUTE || lv == WHIMSY_N_NONE) continue;
		for (size_t i = 0; i < w->g[g]->nevt; i++) {
			if (lv == WHIMSY_N_MENTION && !w->g[g]->ment[i]) continue;
			if (n < cap) { out[n].group = g; out[n].channel = w->g[g]->evt[i]; }
			n++;
		}
	}
	return n;
}

int whimsy_notify_level(const struct whimsy *w, size_t g)
{
	return g < w->ng ? w->g[g]->notify : WHIMSY_N_ALL;
}

int whimsy_muted(const struct whimsy *w, size_t g)
{
	return whimsy_notify_level(w, g) == WHIMSY_N_MUTE;
}

int whimsy_notify(struct whimsy *w, size_t g, int level)
{
	if (g >= w->ng || level < WHIMSY_N_ALL || level > WHIMSY_N_NONE) return WHIMSY_EARG;
	uint8_t rec[WHIMSY_GID + 1];
	memcpy(rec, w->g[g]->g.id, WHIMSY_GID);
	rec[WHIMSY_GID] = (uint8_t)level;
	int e = map_store(store_append(w->st, STORE_MUTE, rec, sizeof rec));
	if (!e) w->g[g]->notify = level;
	return e;
}

/* settings. every key is an integer today, so one range check covers set; a text
 * key would want its own kind here */

static const struct {
	const char *name, *help, *def;
	long lo, hi;
} keys[WHIMSY_NKEY] = {
	[WHIMSY_POLL_SECS] = { "poll_secs", "seconds between fetch round trips", "1", 1, 3600 },
	[WHIMSY_FILE_AUTOSAVE] = { "file_autosave", "also write a received file to <dir>/files",
	                           "0", 0, 1 },
	[WHIMSY_TYPING_SEND] = { "typing", "tell the others while you are composing", "", 0, 1 },
};

const char *whimsy_key_name(int k)
{
	return k >= 0 && k < WHIMSY_NKEY ? keys[k].name : NULL;
}

const char *whimsy_key_help(int k)
{
	return k >= 0 && k < WHIMSY_NKEY ? keys[k].help : NULL;
}

size_t whimsy_get(const struct whimsy *w, int k, char *out, size_t cap)
{
	if (k < 0 || k >= WHIMSY_NKEY) return term(out, cap, 0);
	const char *v = w->set[k][0] ? w->set[k] : keys[k].def;
	size_t n = strlen(v);
	if (cap) memcpy(out, v, n < cap - 1 ? n : cap - 1);
	return term(out, cap, n);
}

int whimsy_set(struct whimsy *w, int k, const char *val)
{
	if (k < 0 || k >= WHIMSY_NKEY || !val) return WHIMSY_EARG;
	size_t n = strlen(val);
	if (!n || n > WHIMSY_MAX_VAL) return WHIMSY_EARG;

	char *end;
	long v = strtol(val, &end, 10);
	if (*end || v < keys[k].lo || v > keys[k].hi) return WHIMSY_EARG;

	uint8_t rec[1 + WHIMSY_MAX_VAL];
	rec[0] = (uint8_t)k;
	memcpy(rec + 1, val, n);
	int e = map_store(store_append(w->st, STORE_SETTING, rec, 1 + n));
	if (e) return e;
	memcpy(w->set[k], val, n + 1);
	return WHIMSY_OK;
}

/* open */

/* Not a whimsy_key: a setting key >= WHIMSY_NKEY is ignored by load and by any other
 * build, which is what makes this mark safe to leave in a shared store. Its value is the
 * record count at the last open that reported dropped records; anything below that index
 * has been reported already.
 * ponytail: a torn tail cut after the mark was written lowers the count and can hide a
 * later drop -- rewrite the mark from a record id if that ever matters */
#define KEY_ACK 255

static size_t ack_read(struct store *st)
{
	size_t ack = 0;
	for (size_t i = 0; i < store_count(st); i++) {
		uint8_t kind;
		size_t n;
		const uint8_t *p = store_get(st, i, &kind, &n);
		if (kind != STORE_SETTING || n != 9 || p[0] != KEY_ACK) continue;
		ack = ld64(p + 1);
	}
	return ack;
}

static int ack_write(struct store *st)
{
	uint8_t rec[9];
	rec[0] = KEY_ACK;
	st64(rec + 1, store_count(st));
	return map_store(store_append(st, STORE_SETTING, rec, sizeof rec));
}

/* everything the groups own: dropped whole when one GROUP record will not parse.
 * partial is not an option -- keeping an older GROUP record for a group whose newer
 * one was skipped would put us back on a retired chain and reuse indices */
/* one group and everything keyed to it */
void drop_grp(struct whimsy *w, size_t i)
{
	struct grp *gr = w->g[i];
	for (size_t j = w->nout; j-- > 0; ) {
		if (!wc_equal(w->out[j].gid, gr->g.id, WHIMSY_GID)) continue;
		wc_wipe(w->out[j].b, w->out[j].n);
		free(w->out[j].b);
		memmove(w->out + j, w->out + j + 1, (--w->nout - j) * sizeof *w->out);
	}
	/* its held files hold assembled plaintext and nothing can reach them once the
	 * group is gone; their chunk records went with erase_grp */
	for (size_t j = w->nheld; j-- > 0; )
		if (wc_equal(w->held[j]->gid, gr->g.id, WHIMSY_GID)) {
			xfer_free(w->held[j]);
			memmove(w->held + j, w->held + j + 1, (--w->nheld - j) * sizeof *w->held);
		}
	free(gr->msg);
	wc_wipe(gr, sizeof *gr);
	free(gr);
	memmove(w->g + i, w->g + i + 1, (--w->ng - i) * sizeof *w->g);
}

static void drop_groups(struct whimsy *w)
{
	while (w->ng) drop_last(w);
	for (size_t i = 0; i < w->nout; i++) {
		wc_wipe(w->out[i].b, w->out[i].n);
		free(w->out[i].b);
	}
	w->nout = 0;
}

/* A record body this build cannot read is skipped, not fatal: only the store container
 * -- header, framing, aead -- is version-gated, so adding a record kind or changing one
 * costs the records of that kind and never the store. *skipped is set when a record the
 * last open had not already reported was dropped, for the frontend to say history was
 * lost once: records this build cannot read stay in the store, so without the ACK mark
 * every later open would report the same ones forever. An unreadable IDENTITY is the one thing that
 * stays fatal: minting a fresh one silently would be a new person on the same disk. */
static int load(struct whimsy *w, int *have_id, int *skipped)
{
	int nogroups = 0;
	size_t ack = ack_read(w->st);
	for (size_t i = 0; i < store_count(w->st); i++) {
		uint8_t kind;
		size_t n;
		const uint8_t *p = store_get(w->st, i, &kind, &n);
		int e = WHIMSY_OK, skip = 0;
		switch (kind) {
		case STORE_IDENTITY:
			if (n != 32) return WHIMSY_ECORRUPT;
			identity_from_seed(&w->id, p);
			*have_id = 1;
			break;
		case STORE_SERVER: {
			size_t hn = n < 2 + 32 ? 0 : p[0];
			size_t pn = n < 2 + hn + 32 ? 0 : p[1 + hn];
			if (n < 2 + 32 || n != 2 + hn + pn + 32 || !hn || !pn ||
			    hn >= sizeof w->host || pn >= sizeof w->port) { skip = 1; break; }
			memcpy(w->host, p + 1, hn);
			w->host[hn] = 0;
			memcpy(w->port, p + 2 + hn, pn);
			w->port[pn] = 0;
			memcpy(w->spk, p + 2 + hn + pn, 32);
			w->registered = 1;
			break;
		}
		case STORE_GROUP: {
			int named = 0;
			e = load_group(w, p, n, &named);
			if (e == WHIMSY_ECORRUPT) { skip = 1; if (!named) nogroups = 1; e = WHIMSY_OK; }
			break;
		}
		case STORE_LINK:
			if (n != 2 * WHIMSY_PK && n != 2 * WHIMSY_PK + 1) { skip = 1; break; }
			if (n == 2 * WHIMSY_PK || p[2 * WHIMSY_PK])
				e = add_link(w, p, p + WHIMSY_PK);
			else
				del_link(w, p, p + WHIMSY_PK);
			if (e == WHIMSY_ESTATE || e == WHIMSY_EARG) e = WHIMSY_OK;
			break;
		case STORE_SETTING:
			if (n < 2 || n > 1 + WHIMSY_MAX_VAL) { skip = 1; break; }
			if (p[0] < WHIMSY_NKEY) {       /* a key this build dropped is skipped */
				memcpy(w->set[p[0]], p + 1, n - 1);
				w->set[p[0]][n - 1] = 0;
			}
			break;
		case STORE_SEEN: {
			struct grp *gr = n != WHIMSY_GID + 10 ? NULL : find_grp(w, p);
			if (!gr) { skip = 1; break; }
			struct seenslot *sl = seen_slot(gr, ld16(p + WHIMSY_GID));
			if (!sl) { skip = 1; break; }
			sl->hw = ld64(p + WHIMSY_GID + 2);
			sl->rec = i;
			break;
		}
		case STORE_REACT: {
			size_t tn = WHIMSY_GID + WHIMSY_MSGID + WHIMSY_PK;
			if (n < tn || n > tn + WHIMSY_MAX_REACT) { skip = 1; break; }
			e = set_react(w, p, p + WHIMSY_GID, p + WHIMSY_GID + WHIMSY_MSGID,
			              (const char *)p + tn, n - tn);
			if (e == WHIMSY_ESTATE) e = WHIMSY_OK;
			break;
		}
		case STORE_PETNAME:
			if (n < WHIMSY_PK || n > WHIMSY_PK + WHIMSY_MAX_PET) { skip = 1; break; }
			e = set_pet(w, p, (const char *)p + WHIMSY_PK, n - WHIMSY_PK);
			break;
		case STORE_OUTBOX:      /* an older store's: opaque, so no row owns it */
			if (n == 8) { drop_out(w, ld64(p)); break; }
			if (n < WHIMSY_GID + 2 || n > WHIMSY_GID + GROUP_MAX_BLOB) { skip = 1; break; }
			e = push_out(w, i, p, p + WHIMSY_GID, n - WHIMSY_GID, SIZE_MAX);
			break;
		case STORE_OUTBOX2: {
			if (n < OUTHDR + 2 || n > OUTHDR + GROUP_MAX_BLOB) { skip = 1; break; }
			uint64_t mrec = ld64(p + WHIMSY_GID);
			if (mrec != UINT64_MAX && mrec >= i) { skip = 1; break; }  /* the row is appended first */
			e = push_out(w, i, p, p + OUTHDR, n - OUTHDR,
			             mrec == UINT64_MAX ? SIZE_MAX : (size_t)mrec);
			break;
		}
		case STORE_XFER: {
			struct grp *gr = n < XFERHDR ? NULL : find_grp(w, p);
			if (!gr) { skip = 1; break; }
			const uint8_t *fid = p + WHIMSY_GID + WHIMSY_MSGID;
			uint32_t total = ld32(fid + 16), idx = ld32(fid + 20);
			size_t bn = n - XFERHDR, row = find_msgid(w, gr, p + WHIMSY_GID);
			if (row == gr->nmsg || !total || total > WIRE_FILE_CHUNKS || idx >= total ||
			    !bn || bn > WIRE_FILE_CHUNK) { skip = 1; break; }
			struct xfer *x = xfer_find(w, gr->g.id, fid, p + WHIMSY_GID);
			if (!x) x = xfer_add(w, gr->g.id, fid, p + WHIMSY_GID,
			                     ld32(p + WHIMSY_GID + WHIMSY_PK), row, total);
			if (!x) { e = WHIMSY_ENOMEM; break; }
			if (total != x->total || x->seen[idx / 8] & 1 << (idx % 8)) { skip = 1; break; }
			x->seen[idx / 8] |= (uint8_t)(1 << (idx % 8));
			size_t off = (size_t)idx * WIRE_FILE_CHUNK;
			memcpy(x->b + off, p + XFERHDR, bn);
			if (off + bn > x->n) x->n = off + bn;
			x->got++;
			break;
		}
		case STORE_MSG: {
			struct grp *gr = n < MSGHDR ? NULL : find_grp(w, p);
			if (!gr) { skip = 1; break; }
			if (n >= MSGEXT && (p[WHIMSY_GID + 10] & MSGDEL)) break;  /* an old tombstone is gone, not lost */
			e = push_msg(gr, i);
			break;
		}
		case STORE_VERIFY:      /* pk alone is an old record: always a verify */
			if (n != WHIMSY_PK && n != WHIMSY_PK + 1) { skip = 1; break; }
			if (n == WHIMSY_PK || p[WHIMSY_PK]) e = add_ver(w, p);
			else del_ver(w, p);
			break;
		case STORE_AVATAR:
			if (n < WHIMSY_PK || n > WHIMSY_PK + WHIMSY_MAX_AVATAR) { skip = 1; break; }
			e = av_load(w, p, WHIMSY_PK, 0, i);
			break;
		case STORE_GAVATAR:
			if (n < WHIMSY_GID || n > WHIMSY_GID + WHIMSY_MAX_AVATAR) { skip = 1; break; }
			e = av_load(w, p, WHIMSY_GID, 1, i);
			break;
		case STORE_ASENT:
			if (n != WHIMSY_PK) { skip = 1; break; }
			e = sent_add(w, p);
			break;
		case STORE_BLOCK:
			if (n != WHIMSY_PK + 1) { skip = 1; break; }
			e = blk_set(w, p, p[WHIMSY_PK] != 0);
			break;
		case STORE_MUTE: {
			struct grp *gr = n != WHIMSY_GID + 1 ? NULL : find_grp(w, p);
			if (!gr) { skip = 1; break; }
			gr->notify = p[WHIMSY_GID] <= WHIMSY_N_NONE ? p[WHIMSY_GID] : WHIMSY_N_MUTE;
			break;
		}
		case STORE_GONE:                /* read on demand, by gone_ver */
			if (n != WHIMSY_GID + 8) skip = 1;
			break;
		case STORE_VOID:                /* dropped in place: nothing to load, not a loss */
			break;
		default:                        /* a kind a newer build wrote */
			skip = 1;
			break;
		}
		if (e) return e;
		if (skip && i >= ack) *skipped = 1;
	}
	if (nogroups) drop_groups(w);
	else for (size_t i = w->ng; i-- > 0; ) if (w->g[i]->bad) drop_grp(w, i);
	return WHIMSY_OK;
}

int whimsy_open(struct whimsy **out, const char *dir, const char *pass, int *lost)
{
	*out = NULL;
	if (lost) *lost = 0;
	struct whimsy *w = calloc(1, sizeof *w);
	if (!w) return WHIMSY_ENOMEM;

	size_t torn = 0;
	int skipped = 0;
	int e = map_store(store_open(&w->st, dir, pass, &torn));
	int have_id = 0;
	if (!e) e = load(w, &have_id, &skipped);
	if (!e) prune_xfers(w);         /* after load: store_replace invalidates its record pointers */
	/* also for a torn tail: store_open only reports it, the file is cut by this append */
	if (!e && (skipped || torn)) e = ack_write(w->st);
	if (!e && lost) *lost = torn || skipped;
	if (!e && !have_id) {
		identity_generate(&w->id);
		e = map_store(store_append(w->st, STORE_IDENTITY, w->id.seed, 32));
	}
	if (!e && !(w->dir = strdup(dir))) e = WHIMSY_ENOMEM;
	if (e) { whimsy_close(w); return e; }
	*out = w;
	return WHIMSY_OK;
}

int whimsy_rekey(struct whimsy *w, const char *oldpass, const char *newpass)
{
	return map_store(store_rekey(w->st, oldpass, newpass));
}

void whimsy_close(struct whimsy *w)
{
	if (!w) return;
	net_close(w->n);
	store_close(w->st);
	for (size_t i = 0; i < w->ng; i++) {
		free(w->g[i]->msg);
		wc_wipe(w->g[i], sizeof *w->g[i]);
		free(w->g[i]);
	}
	free(w->g);
	free(w->pet);
	free(w->rea);
	free(w->lnk);
	free(w->ver);
	free(w->blk);
	free(w->av);
	free(w->sent);
	for (size_t i = 0; i < w->nout; i++) {
		wc_wipe(w->out[i].b, w->out[i].n);
		free(w->out[i].b);
	}
	free(w->out);
	for (size_t i = 0; i < w->nheld; i++) xfer_free(w->held[i]);
	free(w->dir);
	identity_wipe(&w->id);
	wc_wipe(w, sizeof *w);
	free(w);
}

