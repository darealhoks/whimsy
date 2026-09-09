#ifdef _WIN32
#include <windows.h>
#endif

#include "plat.h"
#include "ui_int.h"

#include "audio.h"

#include <stdlib.h>

#include "file.h"


const char *mode_tag(const struct ui *u)
{
	if (u->ask[0]) return u->ask;
	switch (u->mode) {
	case M_REPLY: return "Replying to";
	case M_EDIT:  return "Editing message";
	case M_DEL:   return "Delete message? y / n / a for always";
	default:      return NULL;
	}
}

void arm(struct ui *u, int mode, size_t i)
{
	u->mode = mode;
	u->mode_i = i;
	memset(&u->comp, 0, sizeof u->comp);
}

/* the temp copy paste_file left for a question that has now been answered */
void paste_clean(struct ui *u)
{
	char *sl;
	if (!u->pastetmp[0]) return;
	remove(u->pastetmp);
	if ((sl = strrchr(u->pastetmp, '/'))) { *sl = 0; rmdir(u->pastetmp); }
	u->pastetmp[0] = 0;
}

void ans_clear(struct ui *u)
{
	memset(u->ans, 0, sizeof u->ans);
	u->nans = 0;
}

/* drop whatever the composer was armed with: a mode, or a question a command asked */
void cancel(struct ui *u)
{
	memset(&u->comp, 0, sizeof u->comp);
	u->mode = M_NONE;
	u->ask[0] = u->pend[0] = 0;
	u->ask_typing = 0;
	paste_clean(u);
	ans_clear(u);
	u->ninfo = 0;
	u->find[0] = 0;
}

/* pane layout survives a relaunch through the config file */
void remember(struct ui *u, const char *key, double v)
{
	char val[32];
	snprintf(val, sizeof val, "%g", v);
	conf_set(u->c, key, val);
}

/* ---- state ---- */

void invalidate(struct ui *u)
{
	size_t n = whimsy_msg_count(u->w, u->g);
	if (n > u->nrh) {
		float *p = realloc(u->rh, (n + 64) * sizeof *p);
		if (!p) return;
		u->rh = p;
		u->nrh = n + 64;
	}
	if (u->rh) memset(u->rh, 0, u->nrh * sizeof *u->rh);
	u->cache_g = u->g;
}


void select_group(struct ui *u, size_t g, size_t ch)
{
	size_t gn = whimsy_group_count(u->w);
	if (g >= gn) g = 0;
	u->g = g;
	u->chan = ch;
	u->scroll = 0;
	u->mode = M_NONE;
	u->lsel = 0;
	u->act_i = (size_t)-1;
	u->card = 0;
	invalidate(u);
	if (!gn) { u->chan = 0; u->newat = (size_t)-1; return; }
	size_t n = whimsy_msg_count(u->w, g);
	u->newat = whimsy_first_unread(u->w, g, whimsy_channel_id(u->w, g, ch));
	if (u->newat >= n) u->newat = (size_t)-1;
	else { invalidate(u); scroll_to(u, u->newat); }
}

void ui_send(struct ui *u)
{
	struct whimsy_msg m;
	int e;
	if (!whimsy_group_count(u->w)) return;
	if (u->mode) {
		int mode = u->mode;
		if (whimsy_msg(u->w, u->g, u->mode_i, &m) != WHIMSY_OK) { u->mode = M_NONE; return; }
		if (mode == M_DEL) return;              /* answered by one key, not by enter */
		if (!u->comp.n) return;
		e = mode == M_EDIT
		    ? whimsy_edit(u->w, u->g, u->mode_i, u->comp.buf, u->comp.n)
		    : whimsy_send_reply(u->w, u->g, chan_id(u), m.id, u->comp.buf, u->comp.n);
		if (!e) u->mode = M_NONE;
		if (e) { snprintf(u->err, sizeof u->err, "%s", whimsy_strerror(e)); return; }
		memset(&u->comp, 0, sizeof u->comp);
		u->scroll = 0;
		invalidate(u);
		return;
	}
	if (!u->comp.n) return;
	e = whimsy_send(u->w, u->g, chan_id(u), u->comp.buf, u->comp.n);
	if (e) {
		snprintf(u->err, sizeof u->err, "%s", whimsy_strerror(e));
		return;
	}
	memset(&u->comp, 0, sizeof u->comp);
	u->scroll = 0;
	invalidate(u);
}

/* first keystroke ever asks; after that the knob decides. whimsy_typing rate-limits */
void typed(struct ui *u)
{
	char v[WHIMSY_MAX_VAL + 1];
	whimsy_get(u->w, WHIMSY_TYPING_SEND, v, sizeof v);
	if (!v[0]) {
		if (!u->ask[0]) {
			snprintf(u->ask, sizeof u->ask, "send typing indicators? [y/N]");
			u->ask_typing = 1;
		}
		return;
	}
	if (v[0] == '1' && whimsy_group_count(u->w)) whimsy_typing(u->w, u->g, chan_id(u));
}

/* ---- notifications ---- */

/* the sender's avatar bytes dropped in the cache so %a can name a file. empty when
 * there is none: the command gets an empty word rather than a missing one. the file is
 * cleartext and never removed, so it is written only for a command that asked for it */
static void avatar_path(struct ui *u, const uint8_t *pk, char *out, size_t cap)
{
	char dir[512], hex[2 * WHIMSY_PK + 1];
#ifdef _WIN32
	const char *xdg = getenv("LOCALAPPDATA"), *home = NULL;
#else
	const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
#endif
	size_t n = 0;
	const uint8_t *b;

	out[0] = 0;
	if (!strstr(u->c->notify, "%a")) return;
	b = u->c->avatars < 0.5 ? NULL : whimsy_avatar_get(u->w, pk, &n);
	if (!b || !n) return;
	if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/whimsy", xdg);
	else snprintf(dir, sizeof dir, "%s/.cache/whimsy", home ? home : "/tmp");
	if (plat_mkdir(dir) && errno != EEXIST) return;
	whimsy_pk_hex(hex, pk);
	snprintf(out, cap, "%s/%s.img", dir, hex);
	if (!SDL_SaveFile(out, b, n)) out[0] = 0;
}

/* one %-code expanded into out; returns how far it consumed */
static void expand(const char *in, size_t n, char *out, size_t cap,
                   const char *const val[128])
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 1 < cap; i++) {
		const char *v;
		if (in[i] != '%' || i + 1 >= n) { out[o++] = in[i]; continue; }
		if (in[i + 1] == '%') { out[o++] = '%'; i++; continue; }
		v = val[(unsigned char)in[++i] & 127];
		if (!v) { if (o + 2 >= cap) break; out[o++] = '%'; out[o++] = in[i]; continue; }
		for (; *v && o + 1 < cap; v++) out[o++] = *v;
	}
	out[o] = 0;
}

#ifdef _WIN32
/* CreateProcess splits the command line again, so a word that reached us as message text has
 * to be quoted back into one argument: msvcrt rule, a backslash run doubles only when a quote
 * follows it. appends the word and a separator; 0 if it would not fit */
static int win_quote(char *out, size_t cap, size_t *at, const char *s)
{
	size_t o = *at, nb = 0, i;
#define PUT(c) do { if (o + 1 >= cap) return 0; out[o++] = (char)(c); } while (0)
	PUT('"');
	for (; *s; s++) {
		if (*s == '\\') { nb++; continue; }
		if (*s == '"') { for (i = 0; i < nb; i++) { PUT('\\'); PUT('\\'); } PUT('\\'); }
		else for (i = 0; i < nb; i++) PUT('\\');
		nb = 0;
		PUT(*s);
	}
	for (i = 0; i < nb; i++) { PUT('\\'); PUT('\\'); }
	PUT('"');
	PUT(' ');
#undef PUT
	out[o] = 0;
	*at = o;
	return 1;
}
#endif

/* the command from the config, split on spaces with "..." and '...' kept together, the
 * %-codes filled in per word after the split: a value never re-splits and never sees a
 * shell, so a message that is all quotes and semicolons is just text */
static void run_notify(struct ui *u, const char *const val[128])
{
	char words[16][512];
	char *argv[17];
	int nw = 0;
	const char *p = u->c->notify;

	while (*p && nw < 16) {
		char raw[512];
		size_t n = 0;
		while (*p == ' ') p++;
		if (!*p) break;
		while (*p && *p != ' ' && n + 1 < sizeof raw) {
			if (*p == '"' || *p == '\'') {
				char q = *p++;
				while (*p && *p != q && n + 1 < sizeof raw) raw[n++] = *p++;
				if (*p) p++;
				continue;
			}
			raw[n++] = *p++;
		}
		while (*p && *p != ' ') p++;
		raw[n] = 0;
		expand(raw, n, words[nw], sizeof words[nw], val);
		argv[nw] = words[nw];
		nw++;
	}
	if (!nw) return;
	argv[nw] = NULL;
#ifdef _WIN32
	/* cmd.exe re-parses its own command line, so no quoting here can keep a message-derived
	 * word from becoming a metacharacter. refuse rather than hand it one */
	size_t l0 = strlen(argv[0]);
	if (l0 > 4 && (!_stricmp(argv[0] + l0 - 4, ".bat") || !_stricmp(argv[0] + l0 - 4, ".cmd"))) return;
	char cmd[1024];
	size_t at = 0;
	for (int i = 0; i < nw; i++)
		if (!win_quote(cmd, sizeof cmd, &at, argv[i])) return;
	STARTUPINFOA si = { 0 };
	si.cb = sizeof si;
	PROCESS_INFORMATION pi;
	if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hProcess);      /* no wait: the command reaps itself, as on posix */
		CloseHandle(pi.hThread);
	}
#else
	pid_t pid = fork();
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
#endif
}

/* whimsy_events after a poll: one run of the notify command per group and channel that
 * gained a message from someone else. muted groups are already left out */
static void notify(struct ui *u)
{
	struct whimsy_event ev[16];
	size_t n = whimsy_events(u->w, ev, 16);
	if (!u->c->notify[0]) return;
	if (n > 16) n = 16;
	for (size_t k = 0; k < n; k++) {
		char group[128], chan[128], sender[WHIMSY_MAX_PET + 1], text[256], apath[600];
		const char *val[128] = {0};
		size_t i = whimsy_msg_count(u->w, ev[k].group);
		struct whimsy_msg m;
		int have = 0;
		while (i-- > 0 && !have)
			if (whimsy_msg(u->w, ev[k].group, i, &m) == WHIMSY_OK &&
			    m.channel == ev[k].channel && !m.mine) have = 1;
		if (!have) continue;

		row_title(u, ev[k].group, group, sizeof group);
		chan[0] = 0;
		for (size_t c = 0; c < whimsy_channel_count(u->w, ev[k].group); c++)
			if (whimsy_channel_id(u->w, ev[k].group, c) == ev[k].channel)
				whimsy_channel_name(u->w, ev[k].group, c, chan, sizeof chan);
		whimsy_petname(u->w, m.sender, sender, sizeof sender);
		snprintf(text, sizeof text, "%.*s", (int)(m.text_n < 255 ? m.text_n : 255), m.text);
		avatar_path(u, m.sender, apath, sizeof apath);

		val['g'] = group[0] ? group : sender;   /* a dm has no name: the peer is the title */
		val['c'] = chan;
		val['s'] = sender;
		val['t'] = text;
		val['a'] = apath;
		run_notify(u, val);
	}
}

static void backoff(struct ui *u, uint64_t now)
{
	u->backoff = u->backoff ? (u->backoff < 32 ? u->backoff * 2 : 32) : 1;
	u->next_dial = now + u->backoff;
}

int ui_tick(struct ui *u)
{
	uint64_t now = SDL_GetTicks() / 1000;
	int draw = 0;

	if (!whimsy_online(u->w)) {
		if (now >= u->next_dial) {
			int e = whimsy_connect(u->w, NULL);
			if (e == WHIMSY_OK) {
				u->backoff = 0;
				u->neterr = 0;
				draw = 1;
			} else {
				u->neterr = e;
				backoff(u, now);
			}
		}
		return draw;
	}

	if (now < u->next_poll) return draw;
	char secs[WHIMSY_MAX_VAL + 1];
	whimsy_get(u->w, WHIMSY_POLL_SECS, secs, sizeof secs);
	long s = strtol(secs, NULL, 10);
	u->next_poll = now + (uint64_t)(s > 0 ? s : 5);
	int r = whimsy_poll(u->w);
	/* a dial that succeeds and a poll that then drops the socket must back off too,
	 * or the two branches flip online/offline every frame */
	if (r < 0) { u->neterr = r; backoff(u, now); return 1; }
	u->neterr = 0;
	u->backoff = 0;
	u->next_dial = now;
	if (r > 0) {
		/* arrivals while scrolled up keep a `new` rule where reading stopped */
		if (u->scroll > 0 && u->newat == (size_t)-1) {
			size_t f = whimsy_first_unread(u->w, u->g, chan_id(u));
			if (f < whimsy_msg_count(u->w, u->g)) u->newat = f;
		}
		invalidate(u);
		notify(u);
		draw = 1;
	}
	return draw;
}

int ui_action(struct ui *u)
{
	int a = u->action;
	u->action = UI_NONE;
	return a;
}

int ui_open(struct ui **out, struct whimsy *w, struct draw *d, struct conf *c, SDL_Renderer *r)
{
	struct ui *u = calloc(1, sizeof *u);
	if (!u) return -1;
	u->w = w; u->d = d; u->c = c; u->r = r;
	u->side_w = (float)c->side_w;
	u->memb_w = (float)c->memb_w;
	u->side_open = c->side_open != 0;
	u->memb_open = c->memb_open != 0;
	u->scale = 1;
	u->fat = u->act_i = u->newat = u->spoil = (size_t)-1;
#ifndef _WIN32
	signal(SIGCHLD, SIG_IGN);       /* the notify command reaps itself */
#endif
	whimsy_self(w, u->self);
	invalidate(u);
	*out = u;
	return 0;
}

void ui_close(struct ui *u)
{
	if (!u) return;
	SDL_ClearClipboardData();       /* sdl still holds u as the clip_done userdata */
	audio_stop();
	crop_close(u);
	for (int k = 0; k < WHIMSY_HELD; k++) draw_image_free(u->tex[k].t);
	for (int k = 0; k < AVATARS; k++) if (u->av[k].t) draw_image_free(u->av[k].t);
	free(u->clipdata);
	free(u->rh);
	free(u);
}
