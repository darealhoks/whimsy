#include "ui_int.h"

#include <stdlib.h>

#include "file.h"
#include "img.h"

/* ---- commands ---- */

void ui_err(struct ui *u, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(u->err, sizeof u->err, fmt, ap);
	va_end(ap);
}

/* the delete confirm answers on one key, no enter: y deletes, a deletes and stops
 * asking, anything else cancels */
void del_answer(struct ui *u, char k)
{
	size_t i = u->mode_i;
	u->mode = M_NONE;
	memset(&u->comp, 0, sizeof u->comp);
	if (k != 'y' && k != 'a') return;
	if (k == 'a') {
		u->c->confirm_delete = 0;
		conf_set(u->c, "confirm_delete", "0");
	}
	int e = whimsy_delete(u->w, u->g, i);
	if (e) { ui_err(u, "%s", whimsy_strerror(e)); return; }
	u->act_i = (size_t)-1;
	invalidate(u);
}

void edit_msg(struct ui *u, size_t i)
{
	struct whimsy_msg m;
	char t[FIELD_MAX];
	if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK) return;
	arm(u, M_EDIT, i);
	snprintf(t, sizeof t, "%.*s", (int)m.text_n, m.text);
	field_insert(&u->comp, t);
}

void del_msg(struct ui *u, size_t i)
{
	arm(u, M_DEL, i);
	if (!u->c->confirm_delete) del_answer(u, 'y');
}

static void info(struct ui *u, const char *fmt, ...)
{
	if (u->ninfo >= INFO_MAX) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(u->info[u->ninfo++], sizeof u->info[0], fmt, ap);
	va_end(ap);
}

/* WHIMSY_OK vanishes, anything else lands on the error line */
int done(struct ui *u, int e)
{
	if (e) ui_err(u, "%s", whimsy_strerror(e));
	else invalidate(u);
	return 0;
}

/* the command still needs an answer: it is asked above the composer and the whole
 * line is run again once it arrives. confirms and :pass are the same machinery */
static int ask(struct ui *u, int secret, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(u->ask, sizeof u->ask, fmt, ap);
	va_end(ap);
	u->comp.secret = secret;
	return 1;
}

void clip_set(const char *s, size_t n)
{
	char *t = SDL_malloc(n + 1);
	if (!t) return;
	memcpy(t, s, n);
	t[n] = 0;
	SDL_SetClipboardText(t);
	SDL_free(t);
}

int yes(const char *s) { return s[0] == 'y' || s[0] == 'Y'; }

/* the newest message in the channel, or (size_t)-1. phase 4 puts a selection here */
static size_t newest(struct ui *u)
{
	uint16_t id = chan_id(u);
	struct whimsy_msg m;
	for (size_t i = whimsy_msg_count(u->w, u->g); i-- > 0; )
		if (whimsy_msg(u->w, u->g, i, &m) == WHIMSY_OK && m.channel == id) return i;
	return (size_t)-1;
}

/* the row a click named while it is still in this channel, else the newest */
static size_t target(struct ui *u)
{
	struct whimsy_msg m;
	if (u->act_i != (size_t)-1 && whimsy_msg(u->w, u->g, u->act_i, &m) == WHIMSY_OK &&
	    m.channel == chan_id(u)) return u->act_i;
	return newest(u);
}

/* hex key, else a petname of someone we already know */
static int resolve(struct ui *u, const char *s, size_t n, uint8_t pk[WHIMSY_PK])
{
	char t[WHIMSY_PK * 2 + 1];
	if (n < sizeof t) {
		memcpy(t, s, n);
		t[n] = 0;
		if (whimsy_pk_parse(pk, t) == WHIMSY_OK) return 1;
	}
	for (size_t g = 0; g < whimsy_group_count(u->w); g++) {
		for (size_t i = 0; i < whimsy_member_count(u->w, g); i++) {
			const uint8_t *m = whimsy_member(u->w, g, i);
			char name[WHIMSY_MAX_PET + 1];
			whimsy_petname(u->w, m, name, sizeof name);
			if (strlen(name) != n || memcmp(name, s, n)) continue;
			memcpy(pk, m, WHIMSY_PK);
			return 1;
		}
	}
	return 0;
}

int set_key(struct ui *u, const char *key, const char *val)
{
	if (conf_set(u->c, key, val)) {
		draw_reload(u->d, u->c, u->scale);
		invalidate(u);
		return 0;
	}
	for (int k = 0; whimsy_key_name(k); k++)
		if (!strcmp(whimsy_key_name(k), key)) return done(u, whimsy_set(u->w, k, val));
	ui_err(u, "no such key, or a value it will not take");
	return 0;
}

/* the dm with pk, or (size_t)-1 */
static size_t find_dm(struct ui *u, const uint8_t pk[WHIMSY_PK])
{
	for (size_t g = 0; g < whimsy_group_count(u->w); g++) {
		const uint8_t *p;
		if (is_dm(u, g) && (p = peer(u, g)) && !memcmp(p, pk, WHIMSY_PK)) return g;
	}
	return (size_t)-1;
}

/* the dm with pk, made when there is none */
int open_dm(struct ui *u, const uint8_t pk[WHIMSY_PK])
{
	size_t i = find_dm(u, pk);
	int e;
	if (i == (size_t)-1) {
		const char *chans[] = {"general"};
		if ((e = whimsy_group_new(u->w, "", chans, 1))) return done(u, e);
		i = whimsy_group_count(u->w) - 1;
		if ((e = whimsy_group_add(u->w, i, pk))) return done(u, e);
	}
	select_group(u, i, 0);
	return 0;
}

/* ---- file commands ---- */


static const void *clip_cb(void *ud, const char *mime, size_t *size)
{
	struct ui *u = ud;
	(void)mime;
	*size = u->clipn;
	return u->clipdata;
}

static void clip_done(void *ud)
{
	struct ui *u = ud;
	free(u->clipdata);
	u->clipdata = NULL;
	u->clipn = 0;
}

/* the bytes themselves on the clipboard, under the mime their name claims */
void clip_bytes(struct ui *u, const void *b, size_t n, const char *mime)
{
	void *copy = malloc(n);
	if (!copy) return;
	memcpy(copy, b, n);
	clip_done(u);
	SDL_SetClipboardData(clip_cb, clip_done, u, &mime, 1);   /* fires the old clip_done */
	u->clipdata = copy;
	u->clipn = n;
}

#define AVATAR_PX 64            /* .map/gui.md: avatars are 64 px, at most 8k */

/* the picked picture cropped square, resized and encoded here: core never parses an
 * image, it carries the bytes a frontend hands it */
static int set_avatar(struct ui *u, const char *path, int group)
{
	size_t n, out_n;
	void *src = SDL_LoadFile(path, &n), *out;
	if (!src) { ui_err(u, "%s", SDL_GetError()); return 0; }
	int e = img_avatar(src, n, AVATAR_PX, WHIMSY_MAX_AVATAR, &out, &out_n);
	SDL_free(src);
	if (e) { ui_err(u, "that image will not re-encode"); return 0; }
	e = group ? whimsy_group_avatar_set(u->w, u->g, out, out_n)
	          : whimsy_avatar_set(u->w, out, out_n);
	free(out);
	return done(u, e);
}

/* re-encode over-cap image bytes and send what comes out under the same name */
static int send_shrunk(struct ui *u, const char *path)
{
	size_t n, out_n;
	void *src = SDL_LoadFile(path, &n), *out;
	char dir[] = "/tmp/whimsy-XXXXXX", tmp[512];
	const char *base = strrchr(path, '/');
	int e;

	if (!src) { ui_err(u, "%s", SDL_GetError()); return 0; }
	e = img_shrink(src, n, WHIMSY_MAX_FILE, &out, &out_n);
	SDL_free(src);
	if (e) { ui_err(u, "that image will not re-encode"); return 0; }
	if (!mkdtemp(dir)) { free(out); ui_err(u, "no temp directory"); return 0; }
	snprintf(tmp, sizeof tmp, "%s/%s.jpg", dir, base ? base + 1 : path);
	if (!SDL_SaveFile(tmp, out, out_n)) { free(out); rmdir(dir); ui_err(u, "%s", SDL_GetError()); return 0; }
	free(out);
	e = whimsy_send_file(u->w, u->g, chan_id(u), tmp);
	remove(tmp);
	rmdir(dir);
	return done(u, e);
}

/* 1 while the oversize question is standing */
static int send_path(struct ui *u, const char *path)
{
	SDL_PathInfo st;
	char big[32], cap[32];
	if (SDL_GetPathInfo(path, &st) && st.size > WHIMSY_MAX_FILE) {
		file_human((size_t)st.size, big, sizeof big);
		file_human(WHIMSY_MAX_FILE, cap, sizeof cap);
		if (!mime_of(path)) { ui_err(u, "%s is %s, the cap is %s", path, big, cap); return 0; }
		if (!u->nans) return ask(u, 0, "image is %s, shrink to fit? [y/N]", big);
		if (!yes(u->ans[0])) return 0;
		return send_shrunk(u, path);
	}
	return done(u, whimsy_send_file(u->w, u->g, chan_id(u), path));
}

/* SDL's own portal backed dialogs; the pick comes back as the command line again */
static void picked(void *ud, const char *const *list, int filter)
{
	struct ui *u = ud;
	char line[512];
	int save = u->dialog == 2;
	(void)filter;
	u->dialog = 0;
	if (!list || !list[0]) return;
	snprintf(line, sizeof line, "%s %s", save ? "save" : "file", list[0]);
	run_cmd(u, line);
}

static void pick(struct ui *u, int which)
{
	SDL_Window *win = SDL_GetRenderWindow(u->r);
	u->dialog = which;
	if (which == 2) SDL_ShowSaveFileDialog(picked, u, win, NULL, 0, NULL);
	else SDL_ShowOpenFileDialog(picked, u, win, NULL, 0, NULL, false);
}

/* 1 when the line is waiting on an answer. line is what was typed after the ':' */
int exec(struct ui *u, const char *line)
{
	struct cmd_line l;
	cmd_parse(line, &l);
	if (!l.nw) return 0;
	const struct cmd *c = cmd_find(l.w[0], l.n[0]);
	if (!c) { ui_err(u, "no such command"); return 0; }
	if (c->owner && !whimsy_is_owner(u->w, u->g)) { ui_err(u, "owner only"); return 0; }

	const char *a1 = l.nw > 1 ? l.w[1] : "";
	size_t n1 = l.nw > 1 ? l.n[1] : 0;
	const char *rest1 = cmd_tail(line, 1), *rest2 = cmd_tail(line, 2), *rest3 = cmd_tail(line, 3);
	char sub = l.nw > 1 ? l.w[1][0] : 0;
	uint8_t pk[WHIMSY_PK];
	size_t i;
	int e;

	if (!strcmp(c->name, "group")) {
		if (sub == 'n') {
			const char *chans[] = {"general"};
			if (!*rest2) { ui_err(u, "give the group a name"); return 0; }
			if ((e = whimsy_group_new(u->w, rest2, chans, 1))) return done(u, e);
			select_group(u, whimsy_group_count(u->w) - 1, 0);
			return 0;
		}
		if (sub == 'r') return done(u, whimsy_group_rename(u->w, u->g, rest2));
		if (sub == 'a') {
			if (!*rest2) return done(u, whimsy_group_avatar_set(u->w, u->g, NULL, 0));
			return set_avatar(u, rest2, 1);
		}
		if (sub == 'd') {
			char name[128];
			row_title(u, u->g, name, sizeof name);
			if (!u->nans) return ask(u, 0, "leave %s? everything local goes with it [y/N]",
			                         name[0] ? name : "this group");
			if (!yes(u->ans[0])) return 0;
			if ((e = whimsy_group_leave(u->w, u->g))) return done(u, e);
			/* leaving shifts every index above u->g: no tex slot's .g still names its group */
			for (int k = 0; k < WHIMSY_HELD; k++)
				if (u->tex[k].t) { draw_image_free(u->tex[k].t); u->tex[k].t = NULL; }
			select_group(u, 0, 0);
			return 0;
		}
		ui_err(u, "n, r, d or a");
		return 0;
	}
	if (!strcmp(c->name, "chan")) {
		if (sub == 'n') return done(u, whimsy_channel(u->w, u->g, 'a', rest2, NULL));
		if (sub == 'r') {
			if (l.nw < 4) { ui_err(u, ":chan r <name> <new>"); return 0; }
			char from[132];
			snprintf(from, sizeof from, "%.*s", (int)l.n[2], l.w[2]);
			return done(u, whimsy_channel(u->w, u->g, 'r', from, rest3));
		}
		if (sub == 'd') {
			if (!*rest2) { ui_err(u, "which channel"); return 0; }
			if (!u->nans) return ask(u, 0, "delete channel %s? [y/N]", rest2);
			if (!yes(u->ans[0])) return 0;
			e = whimsy_channel(u->w, u->g, 'd', rest2, NULL);
			if (!e) select_group(u, u->g, 0);
			return done(u, e);
		}
		ui_err(u, "n, r or d");
		return 0;
	}
	if (!strcmp(c->name, "add") || !strcmp(c->name, "kick") || !strcmp(c->name, "dm") ||
	    !strcmp(c->name, "block") ||
	    !strcmp(c->name, "link") || !strcmp(c->name, "pet") ||
	    (!strcmp(c->name, "verify") && l.nw > 1)) {
		if (!n1 || !resolve(u, a1, n1, pk)) { ui_err(u, "no key or petname like that"); return 0; }
		if (!strcmp(c->name, "block")) {
			int on = !whimsy_blocked(u->w, pk);
			if ((e = whimsy_block(u->w, pk, on))) return done(u, e);
			invalidate(u);
			ui_err(u, on ? "blocked; what they send is hidden, not stopped"
			          : "unblocked");
			return 0;
		}
		if (!strcmp(c->name, "add"))    return done(u, whimsy_group_add(u->w, u->g, pk));
		if (!strcmp(c->name, "link"))   return done(u, whimsy_link(u->w, pk));
		if (!strcmp(c->name, "verify")) return done(u, whimsy_verify(u->w, pk));
		if (!strcmp(c->name, "pet"))    return done(u, whimsy_set_petname(u->w, pk, rest2));
		if (!strcmp(c->name, "kick")) {
			char name[WHIMSY_MAX_PET + 1];
			whimsy_petname(u->w, pk, name, sizeof name);
			if (!u->nans) return ask(u, 0, "kick %s? [y/N]", name);
			if (!yes(u->ans[0])) return 0;
			return done(u, whimsy_group_kick(u->w, u->g, pk));
		}
		return open_dm(u, pk);
	}
	if (!strcmp(c->name, "verify")) {          /* no argument: the fingerprints to compare */
		for (i = 0; i < whimsy_member_count(u->w, u->g); i++) {
			const uint8_t *m = whimsy_member(u->w, u->g, i);
			char name[WHIMSY_MAX_PET + 1], fp[WHIMSY_FP_LEN];
			whimsy_petname(u->w, m, name, sizeof name);
			whimsy_fingerprint(fp, m);
			info(u, "%s%s  %s", name, whimsy_verified(u->w, m) ? " (verified)" : "", fp);
		}
		return 0;
	}
	if (!strcmp(c->name, "me")) {
		uint8_t self[WHIMSY_PK];
		char hex[2 * WHIMSY_PK + 1], fp[WHIMSY_FP_LEN], name[WHIMSY_MAX_PET + 1];
		whimsy_self(u->w, self);
		whimsy_pk_hex(hex, self);
		whimsy_fingerprint(fp, self);
		whimsy_petname(u->w, self, name, sizeof name);
		/* with no petname of our own set, whimsy_petname hands back the fingerprint's
		 * first group, which is already the head of the line below */
		if (strncmp(fp, name, strlen(name))) info(u, "%s", name);
		info(u, "%s", fp);          /* grouped, to read out loud; the clipboard gets it unbroken */
		clip_set(hex, strlen(hex));
		ui_err(u, "copied to clipboard");
		return 0;
	}
	if (!strcmp(c->name, "purge")) {
		int all = n1 == 3 && !memcmp(a1, "all", 3);
		long n = strtol(all ? rest2 : rest1, NULL, 10);
		if (n <= 0) { ui_err(u, "purge how many"); return 0; }
		if (all && !whimsy_is_owner(u->w, u->g)) { ui_err(u, "owner only"); return 0; }
		if (all && !u->nans)
			return ask(u, 0, "delete the oldest %ld here for everyone? [y/N]", n);
		if (all && !yes(u->ans[0])) return 0;
		e = all ? whimsy_purge_all(u->w, u->g, chan_id(u), (size_t)n)
		        : whimsy_purge(u->w, u->g, chan_id(u), (size_t)n);
		if (e) return done(u, e);
		u->act_i = (size_t)-1;
		invalidate(u);
		return 0;
	}
	if (!strcmp(c->name, "mute")) {
		int on = !whimsy_muted(u->w, u->g);
		if ((e = whimsy_mute(u->w, u->g, on))) return done(u, e);
		ui_err(u, on ? "muted" : "unmuted");
		return 0;
	}
	if (!strcmp(c->name, "set")) {
		if (!l.nw || l.nw == 1) { u->set_open = 1; u->set_row = -1; u->set_scroll = 0; return 0; }
		char key[64], v[WHIMSY_MAX_VAL + 1];
		snprintf(key, sizeof key, "%.*s", (int)n1, a1);
		if (l.nw == 2) {
			if (conf_getstr(u->c, key, v, sizeof v)) { info(u, "%s = %s", key, v); return 0; }
			for (int k = 0; whimsy_key_name(k); k++)
				if (!strcmp(whimsy_key_name(k), key)) {
					whimsy_get(u->w, k, v, sizeof v);
					info(u, "%s = %s", key, v);
					info(u, "%s", whimsy_key_help(k));
					return 0;
				}
			ui_err(u, "no such key");
			return 0;
		}
		return set_key(u, key, rest2);
	}
	if (!strcmp(c->name, "pass")) {
		if (!u->nans) return ask(u, 1, "current passphrase, empty for the keyfile");
		if (u->nans == 1) return ask(u, 1, "new passphrase, empty for a keyfile");
		if (u->nans == 2) return ask(u, 1, "again");
		if (strcmp(u->ans[1], u->ans[2])) { ui_err(u, "the two differ"); return 0; }
		e = whimsy_rekey(u->w, u->ans[0][0] ? u->ans[0] : NULL, u->ans[1][0] ? u->ans[1] : NULL);
		if (!e) ui_err(u, "the store is sealed under the new key");
		return done(u, e);
	}
	if (!strcmp(c->name, "file")) {
		if (!*rest1) { pick(u, 1); return 0; }
		return send_path(u, rest1);
	}
	if (!strcmp(c->name, "avatar")) {
		if (!*rest1) return done(u, whimsy_avatar_set(u->w, NULL, 0));
		return set_avatar(u, rest1, 0);
	}
	if (!strcmp(c->name, "nuke")) {
		if (!u->nans) {
			info(u, "every message, group, petname, link, verification and avatar here goes.");
			info(u, "unfetched blobs are revoked; what people already fetched stays theirs.");
			info(u, "your key, your server and your settings stay.");
			return ask(u, 0, "type nuke to confirm");
		}
		if (strcmp(u->ans[0], "nuke")) { ui_err(u, "not nuked"); return 0; }
		e = whimsy_nuke(u->w);
		for (int k = 0; k < WHIMSY_HELD; k++)
			if (u->tex[k].t) { draw_image_free(u->tex[k].t); u->tex[k].t = NULL; }
		u->act_i = (size_t)-1;
		select_group(u, 0, 0);
		return done(u, e);
	}
	if (!strcmp(c->name, "server")) { u->action = UI_SERVER; return 0; }
	if (!strcmp(c->name, "quit"))   { u->action = UI_QUIT; return 0; }

	/* what is left names a message: the one a click named, else the newest */
	i = target(u);
	if (i == (size_t)-1) { ui_err(u, "no message here"); return 0; }
	struct whimsy_msg m;
	if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK) { ui_err(u, "no message here"); return 0; }

	if (!strcmp(c->name, "save")) {
		if (!*rest1) { pick(u, 2); return 0; }
		return done(u, whimsy_file_save(u->w, u->g, i, rest1));
	}
	return 0;
}

/* enter on a ':' line, or on the answer to a question a command asked */
void run_line(struct ui *u)
{
	if (u->ask[0]) {
		if (u->nans < (int)(sizeof u->ans / sizeof *u->ans))
			snprintf(u->ans[u->nans++], sizeof u->ans[0], "%s", u->comp.buf);
		u->ask[0] = 0;
	} else {
		snprintf(u->pend, sizeof u->pend, "%s", u->comp.buf + 1);
		u->nans = 0;
	}
	memset(&u->comp, 0, sizeof u->comp);
	u->ninfo = 0;
	u->pop_sel = 0;
	if (!exec(u, u->pend)) {
		u->pend[0] = 0;
		u->nans = 0;
		paste_clean(u);
		u->comp.secret = 0;
	}
}

/* the completion candidates for the word the caret sits in; buf backs the array */
static int candidates(struct ui *u, const struct cmd_line *l, int wi,
                      char buf[][64], const char *cand[], const char *help[], int cap)
{
	int n = 0, t;
	const struct cmd *c = wi > 0 ? cmd_find(l->w[0], l->n[0]) : NULL;
	if (!c) return 0;
	for (int i = 0; i < cap; i++) help[i] = NULL;
	t = wi - 1 < 3 ? c->type[wi - 1] : CA_NONE;
	if (t == CA_SUB) {
		static const char *const sub[] = {"n", "r", "d", "a"};
		static const char *const what[] = {"new", "rename", "delete", "avatar"};
		int leave = !strcmp(c->name, "group");
		for (; n < (leave ? 4 : 3) && n < cap; n++) {
			cand[n] = sub[n];
			help[n] = n == 2 && leave ? "leave" : what[n];
		}
		return n;
	}
	if (t == CA_CHAN)
		for (size_t i = 0; i < whimsy_channel_count(u->w, u->g) && n < cap; i++, n++) {
			whimsy_channel_name(u->w, u->g, i, buf[n], 64);
			cand[n] = buf[n];
		}
	if (t == CA_GROUP)
		for (size_t i = 0; i < whimsy_group_count(u->w) && n < cap; i++) {
			if (is_dm(u, i)) continue;
			whimsy_group_name(u->w, i, buf[n], 64);
			if (!buf[n][0]) continue;
			cand[n] = buf[n];
			n++;
		}
	if (t == CA_PEER)
		for (size_t i = 0; i < whimsy_member_count(u->w, u->g) && n < cap; i++, n++) {
			whimsy_petname(u->w, whimsy_member(u->w, u->g, i), buf[n], 64);
			cand[n] = buf[n];
		}
	if (t == CA_KEY) {
		for (int i = 0; conf_keys[i] && n < cap; i++) cand[n++] = conf_keys[i];
		for (int k = 0; whimsy_key_name(k) && n < cap; k++) cand[n++] = whimsy_key_name(k);
	}
	return n;
}

/* the word the caret sits in, and the candidates for it already filtered by its prefix.
 * returns -1 on the command word itself -- the popup lists commands there instead */
int arg_list(struct ui *u, char buf[][64], const char *cand[], const char *help[],
                    int cap, const char **word, size_t *wn)
{
	struct cmd_line l;
	cmd_parse(u->comp.buf + 1, &l);
	int wi = l.nw ? l.nw - 1 : 0;
	if (u->comp.n && u->comp.buf[u->comp.n - 1] == ' ') wi = l.nw;
	if (wi == 0) return -1;
	*word = wi < l.nw ? l.w[wi] : "";
	*wn = wi < l.nw ? l.n[wi] : 0;
	int n = candidates(u, &l, wi, buf, cand, help, cap), k = 0;
	for (int i = 0; i < n; i++) {
		if (strlen(cand[i]) < *wn || memcmp(cand[i], *word, *wn)) continue;
		cand[k] = cand[i];
		help[k++] = help[i];
	}
	return k;
}

/* tab: the command name, or the argument the caret is in */
void complete(struct ui *u)
{
	char buf[32][64], out[64];
	const char *cand[32], *help[32], *word = "";
	size_t wn = 0;
	int n = arg_list(u, buf, cand, help, 32, &word, &wn);

	if (n < 0) {                    /* the command word: the popup's own highlight */
		struct cmd_line l;
		const struct cmd *m[64];
		const char *nm[64];
		cmd_parse(u->comp.buf + 1, &l);
		word = l.nw ? l.w[0] : "";
		wn = l.nw ? l.n[0] : 0;
		n = cmd_match(word, wn, whimsy_is_owner(u->w, u->g), m, 64);
		if (!n) return;
		for (int i = 0; i < n; i++) nm[i] = m[i]->name;
		/* the shared prefix first, the highlighted row once it adds nothing */
		if (n < 2 || !cmd_complete(word, wn, nm, n, out, sizeof out) || strlen(out) <= wn)
			snprintf(out, sizeof out, "%s ", m[u->pop_sel < n ? u->pop_sel : n - 1]->name);
		u->comp.n = 1 + strlen(out);
		memcpy(u->comp.buf + 1, out, strlen(out));
		u->comp.buf[u->comp.n] = 0;
		u->comp.cur = u->comp.anc = u->comp.n;
		u->pop_sel = 0;
		return;
	}
	if (!n) return;
	if (n < 2 || !cmd_complete(word, wn, cand, n, out, sizeof out) || strlen(out) <= wn)
		snprintf(out, sizeof out, "%s ", cand[u->pop_sel < n ? u->pop_sel : n - 1]);
	size_t at = wn ? (size_t)(word - u->comp.buf) : u->comp.n;
	if (at + strlen(out) >= FIELD_MAX) return;
	memcpy(u->comp.buf + at, out, strlen(out));
	u->comp.n = at + strlen(out);
	u->comp.buf[u->comp.n] = 0;
	u->comp.cur = u->comp.anc = u->comp.n;
	u->pop_sel = 0;
}

