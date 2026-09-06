#define _GNU_SOURCE             /* accept4 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "conf.h"
#include "draw.h"
#include "field.h"
#include "ui.h"
#include "wrap.h"
#include "whimsy.h"

#define MIN_W 640
#define MIN_H 400

/* $XDG_RUNTIME_DIR/whimsy.sock: one line in, nothing back. a second `whimsy` raises the
 * window a `close = hide` left running, `whimsy quit` ends it. no tray */
static int sock_path(char *out, size_t cap)
{
	const char *run = getenv("XDG_RUNTIME_DIR");
	if (run && *run) return snprintf(out, cap, "%s/whimsy.sock", run) < (int)cap;

	/* no runtime dir: a 0700 dir of our own in /tmp, so nobody else can put the socket there */
	char dir[64];
	struct stat st;
	snprintf(dir, sizeof dir, "/tmp/whimsy-%u", (unsigned)getuid());
	if (mkdir(dir, 0700) && errno != EEXIST) return 0;
	if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
	    (st.st_mode & 077)) return 0;
	return snprintf(out, cap, "%s/whimsy.sock", dir) < (int)cap;
}

static void sock_addr(struct sockaddr_un *a, const char *path)
{
	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	snprintf(a->sun_path, sizeof a->sun_path, "%s", path);
}

/* 1 when an instance was already listening and took the word */
static int sock_tell(const char *path, const char *word)
{
	struct sockaddr_un a;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return 0;
	sock_addr(&a, path);
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return 0; }
	ssize_t k = write(fd, word, strlen(word));
	close(fd);
	return k > 0;
}

/* the lock, not the socket file, says who owns the path: two starts racing past a failed
 * connect would otherwise both bind and unlink each other's live socket */
static int sock_listen(const char *path, int *lock)
{
	struct sockaddr_un a;
	char lp[sizeof a.sun_path + 8];
	snprintf(lp, sizeof lp, "%s.lock", path);
	*lock = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (*lock < 0) return -1;
	if (flock(*lock, LOCK_EX | LOCK_NB)) { close(*lock); *lock = -1; return -1; }

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	unlink(path);
	sock_addr(&a, path);
	mode_t um = umask(0177);
	int e = bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4);
	umask(um);
	if (e) { close(fd); return -1; }
	return fd;
}

/* 'r' raise, 'q' quit, 0 nothing said. nonblocking: this runs on the main loop, so a peer
 * that connects and never writes must not hold the ui */
static int sock_read(int fd)
{
	char b[16];
	int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK);
	if (c < 0) return 0;
	ssize_t n = read(c, b, sizeof b - 1);
	close(c);
	if (n <= 0) return 0;
	b[n] = 0;
	return b[0] == 'q' ? 'q' : 'r';
}

enum screen { S_FIRST, S_UNLOCK, S_INVITE, S_READY };

struct app {
	struct conf c;
	struct draw *d;
	SDL_Window *win;
	SDL_Renderer *r;
	struct whimsy *w;
	struct ui *ui;

	const char *dir;
	enum screen screen;
	struct field pass, confirm, invite;
	int focus;                      /* index into the screen's fields */
	float hit[3][4];                /* field boxes, filled by paint */
	float btn[4];
	char err[128];
	int lost;
	int caret;                      /* blink phase */
};

/* the screen's fields in focus order */
static int fields(struct app *a, struct field *out[3])
{
	if (a->screen == S_FIRST) { out[0] = &a->pass; out[1] = &a->confirm; out[2] = &a->invite; return 3; }
	if (a->screen == S_UNLOCK) { out[0] = &a->pass; return 1; }
	if (a->screen == S_INVITE) { out[0] = &a->invite; return 1; }
	return 0;
}

/* ---- screens ---- */

static int have(const char *dir, const char *name)
{
	char p[512];
	struct stat st;
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return !stat(p, &st);
}

static void after_open(struct app *a)
{
	/* ponytail: the dial blocks the ui for its timeout. phase 2 redials off the tick */
	int e = whimsy_connect(a->w, NULL);
	a->screen = e == WHIMSY_ESTATE ? S_INVITE : S_READY;
	if (e && e != WHIMSY_ESTATE)
		snprintf(a->err, sizeof a->err, "server: %s", whimsy_strerror(e));
	a->focus = 0;
}

static void submit(struct app *a)
{
	a->err[0] = 0;
	if (a->screen == S_FIRST || a->screen == S_UNLOCK) {
		if (a->screen == S_FIRST &&
		    (a->pass.n != a->confirm.n || memcmp(a->pass.buf, a->confirm.buf, a->pass.n))) {
			snprintf(a->err, sizeof a->err, "the two passphrases differ");
			return;
		}
		const char *pass = a->pass.n ? a->pass.buf : NULL;
		int e = whimsy_open(&a->w, a->dir, pass, &a->lost);
		if (e) {
			snprintf(a->err, sizeof a->err, "%s", whimsy_strerror(e));
			return;
		}
		memset(&a->pass, 0, sizeof a->pass);
		memset(&a->confirm, 0, sizeof a->confirm);
		if (a->screen == S_FIRST && a->invite.n) {
			e = whimsy_connect(a->w, a->invite.buf);
			if (e) { snprintf(a->err, sizeof a->err, "invite: %s", whimsy_strerror(e)); a->screen = S_INVITE; return; }
			a->screen = S_READY;
			return;
		}
		after_open(a);
		return;
	}
	if (a->screen == S_INVITE) {
		if (!a->invite.n) { snprintf(a->err, sizeof a->err, "paste the invite url"); return; }
		int e = whimsy_connect(a->w, a->invite.buf);
		if (e) { snprintf(a->err, sizeof a->err, "invite: %s", whimsy_strerror(e)); return; }
		a->screen = S_READY;
	}
}

/* ---- painting ---- */

static float text(struct app *a, float x, float y, const char *s, const uint8_t rgb[3])
{
	return draw_text(a->d, x, y, s, strlen(s), rgb, DRAW_REGULAR);
}

/* the title, one accent word at .3em tracking */
static void title(struct app *a, float x, float y)
{
	const char *s = "whimsy";
	float track = (float)a->c.size * 0.3f;
	for (const char *p = s; *p; p++)
		x = draw_text(a->d, x, y, p, 1, a->c.accent, DRAW_REGULAR) + track;
}

static float star_track(struct app *a) { return (float)a->c.size * 0.25f; }

#define MAXLINE 6

/* returns the box height: a wrapped field grows */
static float field_paint(struct app *a, float x, float y, float w, struct field *f, int slot,
                         const char *placeholder)
{
	struct conf *c = &a->c;
	float lh = draw_line_height(a->d), h = lh + c->pad * 2;
	float tx = x + c->pad + 4, iw = w - (c->pad + 4) * 2;
	size_t starts[MAXLINE + 2];
	int nl = 1;
	float cx = tx, cy = y + c->pad;

	if (f->n && !f->secret) {
		nl = wrap_lines(f->buf, f->n, iw, draw_wrap, a->d, starts, MAXLINE + 1);
		h = (float)nl * lh + c->pad * 2;
	}
	uint8_t soft[3] = {255, 255, 255};
	if (slot >= 0) { a->hit[slot][0] = x; a->hit[slot][1] = y; a->hit[slot][2] = w; a->hit[slot][3] = h; }
	draw_rect(a->d, x, y, w, h, soft, 13, c->radius);

	if (!f->n) {
		if (placeholder) text(a, tx, y + c->pad, placeholder, c->dim);
	} else if (f->secret) {
		/* mockup: .25em tracking, ink centred in the box. what does not fit is not shown */
		float adv = draw_measure(a->d, "*", 1, DRAW_REGULAR) + star_track(a);
		float sy = y + h / 2 - draw_ink_mid(a->d, '*', DRAW_REGULAR);
		size_t fit = adv > 0 ? (size_t)(iw / adv) : 0;
		for (size_t i = 0; i < f->n && i < fit; i++)
			draw_text(a->d, tx + adv * (float)i, sy, "*", 1, c->fg, DRAW_REGULAR);
		cx = tx + adv * (float)(f->cur < fit ? f->cur : fit);
	} else {
		for (int k = 0; k < nl; k++) {
			float ly = y + c->pad + (float)k * lh;
			draw_text(a->d, tx, ly, f->buf + starts[k], starts[k + 1] - starts[k], c->fg, DRAW_REGULAR);
			if (f->cur >= starts[k] && (f->cur < starts[k + 1] || k == nl - 1)) {
				cx = tx + draw_measure(a->d, f->buf + starts[k], f->cur - starts[k], DRAW_REGULAR);
				cy = ly;
			}
		}
	}
	if (slot == a->focus && a->caret)
		draw_rect(a->d, cx, cy, 2, lh, c->fg, 255, 0);
	return h;
}

static void button(struct app *a, float x, float y, float w, const char *label)
{
	float lh = draw_line_height(a->d), h = lh + a->c.pad * 2;
	a->btn[0] = x; a->btn[1] = y; a->btn[2] = w; a->btn[3] = h;
	draw_rect(a->d, x, y, w, h, a->c.accent, 255, a->c.radius);
	float tw = draw_measure(a->d, label, strlen(label), DRAW_REGULAR);
	text(a, x + (w - tw) / 2, y + a->c.pad, label, a->c.bg);
}

static void paint(struct app *a, float lw, float lh_win, float scale)
{
	struct conf *c = &a->c;
	float lh = draw_line_height(a->d);

	if (lw < MIN_W || lh_win < MIN_H) {
		const char *s = "whimsy needs 640x400";
		float tw = draw_measure(a->d, s, strlen(s), DRAW_REGULAR);
		text(a, (lw - tw) / 2, (lh_win - lh) / 2, s, c->dim);
		return;
	}

	if (a->screen == S_READY) {
		ui_paint(a->ui, lw, lh_win, scale);
		return;
	}

	float w = a->screen == S_UNLOCK ? 300 : 440;
	float x = (lw - w) / 2;
	float fh = lh + c->pad * 2;

	/* height of the block, laid out once so it can be centred */
	struct field *fs[3];
	int nfield = fields(a, fs);
	if (!nfield) nfield = 1;
	memset(a->hit, 0, sizeof a->hit);
	memset(a->btn, 0, sizeof a->btn);
	float h = lh * 2.5f + (float)nfield * (lh * 2 + fh + c->gap * 2) + fh + 18;
	float y = (lh_win - h) / 2;

	title(a, x, y);
	y += lh * 2.5f;

	if (a->screen == S_FIRST) {
		char hint[600];
		snprintf(hint, sizeof hint, "new store at %s", a->dir);
		text(a, x, y, hint, c->dim);
		y += lh + c->gap;
	}

	if (a->screen == S_FIRST || a->screen == S_UNLOCK) {
		text(a, x, y, "passphrase", c->dim);
		y += lh;
		a->pass.secret = 1;
		y += field_paint(a, x, y, w, &a->pass, 0, NULL) + c->gap;
		if (a->screen == S_FIRST) {
			text(a, x, y, "empty keeps a 0600 keyfile instead", c->dim);
			y += lh + c->gap;
			text(a, x, y, "again", c->dim);
			y += lh;
			a->confirm.secret = 1;
			y += field_paint(a, x, y, w, &a->confirm, 1, NULL) + c->gap;
		}
	}

	if (a->screen == S_FIRST || a->screen == S_INVITE) {
		int slot = a->screen == S_FIRST ? 2 : 0;
		text(a, x, y, "invite", c->dim);
		y += lh;
		y += field_paint(a, x, y, w, &a->invite, slot, "whimsy://host:port/server/token") + c->gap;
		text(a, x, y, "from whoever runs the relay", c->dim);
		y += lh + c->gap;
	}

	y += 6;
	button(a, x, y, w, a->screen == S_UNLOCK ? "open" : "connect");
	y += fh + c->gap;

	if (a->lost) {
		text(a, x, y, "some history could not be read and was dropped", c->dim);
		y += lh;
	}
	if (a->err[0]) text(a, x, y, a->err, c->accent);
}

/* ---- shell ---- */

static void usage(void)
{
	fputs("usage: whimsy [quit] [--dir PATH] [--config PATH]\n", stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	struct app a;
	memset(&a, 0, sizeof a);

	char dir[512], cfg[512], sock[sizeof ((struct sockaddr_un *)0)->sun_path];
	const char *tell = "raise";
	int sfd = -1, lock = -1, ret = 0;
	const char *home = getenv("HOME"), *xdg;
	if ((xdg = getenv("XDG_DATA_HOME")) && *xdg) snprintf(dir, sizeof dir, "%s/whimsy", xdg);
	else snprintf(dir, sizeof dir, "%s/.local/share/whimsy", home ? home : ".");
	if ((xdg = getenv("XDG_CONFIG_HOME")) && *xdg) snprintf(cfg, sizeof cfg, "%s/whimsy/whimsy.conf", xdg);
	else snprintf(cfg, sizeof cfg, "%s/.config/whimsy/whimsy.conf", home ? home : ".");

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "quit")) tell = "quit";
		else if (!strcmp(argv[i], "--dir") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(cfg, sizeof cfg, "%s", argv[++i]);
		else usage();
	}
	a.dir = dir;
	if (!sock_path(sock, sizeof sock)) {
		fprintf(stderr, "no usable ipc socket path\n");
		return 1;
	}
	/* the running instance takes it, or there is none and `quit` has nothing to do */
	if (sock_tell(sock, tell) || strcmp(tell, "raise")) return 0;
	sfd = sock_listen(sock, &lock);
	conf_load(&a.c, cfg);

	if (!have(dir, "store")) a.screen = S_FIRST;
	else if (!have(dir, "key")) a.screen = S_UNLOCK;
	else {
		int e = whimsy_open(&a.w, dir, NULL, &a.lost);
		if (e) { a.screen = S_UNLOCK; snprintf(a.err, sizeof a.err, "%s", whimsy_strerror(e)); }
		else after_open(&a);
	}

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "sdl: %s\n", SDL_GetError());
		return 1;
	}
	if (!strcmp(a.c.renderer, "software")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

	a.win = SDL_CreateWindow("whimsy", 1280, 800,
	                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!a.win) { fprintf(stderr, "window: %s\n", SDL_GetError()); ret = 1; goto done; }
	a.r = SDL_CreateRenderer(a.win, NULL);
	if (!a.r) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); ret = 1; goto done; }
	SDL_SetRenderDrawBlendMode(a.r, SDL_BLENDMODE_NONE);

	float scale = SDL_GetWindowDisplayScale(a.win);
	if (scale <= 0) scale = 1;
	if (draw_open(&a.d, a.r, &a.c, scale)) {
		fprintf(stderr, "no usable font: set `font` in %s\n", a.c.path);
		ret = 1;
		goto done;
	}
	SDL_StartTextInput(a.win);

	int run = 1, dirty = 1;
	uint64_t blink = SDL_GetTicks();
	while (run) {
		if (a.screen == S_READY && !a.ui && ui_open(&a.ui, a.w, a.d, &a.c, a.r)) {
			fprintf(stderr, "out of memory\n");
			ret = 1;
			break;
		}
		SDL_Event e;
		if (SDL_WaitEventTimeout(&e, 500)) {
			do {
				dirty = 1;
				if (a.screen == S_READY) {
					if (e.type == SDL_EVENT_QUIT) {
						if (strcmp(a.c.close, "hide")) { run = 0; break; }
						SDL_HideWindow(a.win);  /* still polling, still notifying */
						continue;
					}
					ui_event(a.ui, &e, scale);
					switch (ui_action(a.ui)) {
					case UI_QUIT: run = 0; break;
					case UI_SERVER:
						a.screen = S_INVITE;    /* the ui stays open behind it */
						a.focus = 0;
						memset(&a.invite, 0, sizeof a.invite);
						break;
					}
					continue;
				}
				struct field *fs[3];
				int nfield = fields(&a, fs);
				struct field *f = nfield ? fs[a.focus] : NULL;
				switch (e.type) {
				case SDL_EVENT_QUIT: run = 0; break;
				case SDL_EVENT_MOUSE_BUTTON_DOWN: {
					/* button.x is window points; paint works in pixels/scale */
					float k = SDL_GetWindowPixelDensity(a.win) / scale;
					float mx = e.button.x * k, my = e.button.y * k;
					for (int i = 0; i < nfield; i++) {
						const float *b = a.hit[i];
						if (b[2] && mx >= b[0] && mx < b[0] + b[2] && my >= b[1] && my < b[1] + b[3]) {
							a.focus = i;
							a.caret = 1;
							blink = SDL_GetTicks();
						}
					}
					const float *b = a.btn;
					if (b[2] && mx >= b[0] && mx < b[0] + b[2] && my >= b[1] && my < b[1] + b[3])
						submit(&a);
					break;
				}
				case SDL_EVENT_TEXT_INPUT:
					if (nfield) field_insert(f, e.text.text);
					break;
				case SDL_EVENT_KEY_DOWN:
					if (e.key.key == SDLK_ESCAPE) { run = 0; break; }
					if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
						if (nfield && a.focus + 1 < nfield) a.focus++;
						else if (nfield) submit(&a);
						break;
					}
					if (e.key.key == SDLK_TAB || e.key.key == SDLK_DOWN) {
						if (nfield) a.focus = (a.focus + 1) % nfield;
						break;
					}
					if (e.key.key == SDLK_UP) {
						if (nfield) a.focus = (a.focus + nfield - 1) % nfield;
						break;
					}
					if (nfield) field_key(f, e.key.key, e.key.mod, 1);
					break;
				}
			} while (run && SDL_PollEvent(&e));
		}

		uint64_t now = SDL_GetTicks();
		if (now - blink >= 500) { blink = now; a.caret = !a.caret; dirty = 1; }
		if (sfd >= 0) {
			int said = sock_read(sfd);
			if (said == 'q') run = 0;
			if (said == 'r') { SDL_ShowWindow(a.win); SDL_RaiseWindow(a.win); dirty = 1; }
		}
		if (a.screen == S_READY && ui_tick(a.ui)) dirty = 1;
		if (conf_reload(&a.c)) { draw_reload(a.d, &a.c, scale); dirty = 1; }

		float s = SDL_GetWindowDisplayScale(a.win);
		if (s > 0 && s != scale) { scale = s; draw_reload(a.d, &a.c, scale); dirty = 1; }

		if (!dirty || (SDL_GetWindowFlags(a.win) & SDL_WINDOW_HIDDEN)) { dirty = 0; continue; }
		dirty = 0;

		int pw, ph;
		SDL_GetWindowSizeInPixels(a.win, &pw, &ph);
		SDL_SetRenderDrawBlendMode(a.r, SDL_BLENDMODE_NONE);
		SDL_SetRenderDrawColor(a.r, a.c.bg[0], a.c.bg[1], a.c.bg[2], (uint8_t)(a.c.alpha * 255));
		SDL_RenderClear(a.r);
		paint(&a, (float)pw / scale, (float)ph / scale, scale);
		SDL_RenderPresent(a.r);
	}

done:
	if (sfd >= 0) { close(sfd); unlink(sock); }
	if (lock >= 0) close(lock);
	if (a.win) SDL_StopTextInput(a.win);
	ui_close(a.ui);
	draw_close(a.d);
	if (a.w) whimsy_close(a.w);
	SDL_DestroyRenderer(a.r);
	SDL_DestroyWindow(a.win);
	SDL_Quit();
	return ret;
}
