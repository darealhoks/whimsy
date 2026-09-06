#include "ui_int.h"


#include "file.h"
#include "wrap.h"

/* ---- composer ---- */

/* the labelled strip inside the composer box: a mode, or a question a command asked.
 * returns its height so the caller can step past it */
static float bar(struct ui *u, float x, float y, float w, float bh)
{
	struct conf *c = u->c;
	const char *tag = mode_tag(u);
	if (!tag) return 0;
	float tx = draw_text(u->d, x + c->pad + 4, y + c->pad / 2, tag, strlen(tag), c->dim, DRAW_REGULAR);
	if (u->mode == M_REPLY && !u->ask[0]) {
		struct whimsy_msg q;
		char nm[WHIMSY_MAX_PET + 1];
		uint8_t col[3];
		if (whimsy_msg(u->w, u->g, u->mode_i, &q) == WHIMSY_OK) {
			whimsy_petname(u->w, q.sender, nm, sizeof nm);
			idcol(u, q.sender, col);
			draw_text(u->d, tx + c->gap, y + c->pad / 2, nm, strlen(nm), col, DRAW_REGULAR);
		}
	}
	float xw = draw_measure(u->d, "\xc3\x97", 2, DRAW_REGULAR);
	draw_text(u->d, x + w - c->pad - 4 - xw, y + c->pad / 2, "\xc3\x97", 2, c->dim, DRAW_REGULAR);
	hit(u, x + w - c->pad * 2 - 4 - xw, y, xw + c->pad * 2, bh, H_UNARM, 0, 0);
	draw_rule(u->d, x + c->pad, y + bh, w - c->pad * 2, c->line);
	return bh;
}

float composer(struct ui *u, float x, float y, float w, int paint)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	float iw = w - (c->pad + 4) * 2;
	uint8_t soft[3] = {255, 255, 255};
	size_t *starts = u->cstart;

	const char *tag = mode_tag(u);
	float bh = tag ? lh + c->pad : 0;

	if (u->comp.secret) {           /* a passphrase answer: stars, one line, no wrapping */
		float h = lh + c->pad * 2 + bh;
		u->cnl = 1;
		if (!paint) return h;
		draw_rect(u->d, x, y, w, h, soft, 13, c->radius);
		y += bar(u, x, y, w, bh);
		float adv = draw_measure(u->d, "*", 1, DRAW_REGULAR);
		size_t fit = adv > 0 ? (size_t)(iw / adv) : 0;
		for (size_t i = 0; i < u->comp.n && i < fit; i++)
			draw_text(u->d, x + c->pad + 4 + adv * (float)i, y + c->pad, "*", 1, c->fg, DRAW_REGULAR);
		if ((SDL_GetTicks() / 500) % 2 == 0)
			draw_rect(u->d, x + c->pad + 4 + adv * (float)(u->comp.n < fit ? u->comp.n : fit),
			          y + c->pad, 2, lh, c->fg, 255, 0);
		return h;
	}

	int nl = u->comp.n ? wrap_lines(u->comp.buf, u->comp.n, iw, draw_wrap, u->d,
	                                starts, MAXWRAP + 2) : 1;
	int vis = nl > COMP_LINES ? COMP_LINES : nl;
	float h = (float)vis * lh + c->pad * 2 + bh;
	u->cnl = nl;

	if (u->ctop > nl - vis) u->ctop = nl - vis;
	if (u->ctop < 0) u->ctop = 0;
	int cl = 0;
	for (int k = 0; k < nl; k++) if (u->comp.cur >= starts[k]) cl = k;
	if (cl < u->ctop) u->ctop = cl;
	else if (cl >= u->ctop + vis) u->ctop = cl - vis + 1;

	if (!paint) return h;
	draw_rect(u->d, x, y, w, h, soft, 13, c->radius);
	y += bar(u, x, y, w, bh);
	hit(u, x, y, w, h - bh, H_COMP, 0, 0);
	float cx = x + c->pad + 4, cy = y + c->pad;
	if (!u->comp.n) {
		const char *ph = u->mode == M_DEL ? "y/n/a" : "message";
		draw_text(u->d, cx, cy, ph, strlen(ph), c->dim, DRAW_REGULAR);
	} else {
		size_t a, b;
		field_sel(&u->comp, &a, &b);
		for (int k = u->ctop; k < u->ctop + vis; k++) {
			float lx = cx, ly = y + c->pad + (float)(k - u->ctop) * lh;
			size_t sx = starts[k], e = starts[k + 1], len = e - sx;
			while (len && u->comp.buf[sx + len - 1] == '\n') len--;
			if (a != b && b > sx && a < e) {  /* selection band under the run */
				size_t f = a > sx ? a : sx, t = b < e ? b : e;
				float bx = lx + draw_measure(u->d, u->comp.buf + sx, f - sx, DRAW_REGULAR);
				float bw = draw_measure(u->d, u->comp.buf + f, t - f, DRAW_REGULAR);
				draw_rect(u->d, bx, ly, bw, lh, c->accent, 70, 0);
			}
			draw_text(u->d, lx, ly, u->comp.buf + sx, len, c->fg, DRAW_REGULAR);
			if (k == cl) {
				cx = lx + draw_measure(u->d, u->comp.buf + sx, u->comp.cur - sx, DRAW_REGULAR);
				cy = ly;
			}
		}
	}
	float edge = x + c->pad + 4 + iw - 2;   /* trailing spaces hang, the caret must not */
	if (cx > edge) cx = edge;
	if ((SDL_GetTicks() / 500) % 2 == 0)
		draw_rect(u->d, cx, cy, 2, lh, c->fg, 255, 0);
	return h;
}

/* caret one wrapped line up or down, keeping its column */
void comp_step_line(struct ui *u, int delta)
{
	int cl = 0;
	for (int k = 0; k < u->cnl; k++) if (u->comp.cur >= u->cstart[k]) cl = k;
	int to = cl + delta;
	if (to < 0 || to >= u->cnl) return;
	size_t col = u->comp.cur - u->cstart[cl];
	size_t len = u->cstart[to + 1] - u->cstart[to];
	while (len && u->comp.buf[u->cstart[to] + len - 1] == '\n') len--;
	u->comp.cur = u->cstart[to] + (col < len ? col : len);
	u->comp.anc = u->comp.cur;
}


/* the command list while the composer starts with ':', or the lines a command left
 * behind. returns the room it took above bottom */
float popup(struct ui *u, float x, float bottom, float w)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	const struct cmd *m[POP_ROWS];
	char abuf[32][64];
	const char *cand[32], *help[32], *word = "";
	size_t wn = 0;
	int n = 0, args = 0;
	if (u->ninfo) {
		n = u->ninfo;
	} else if (u->comp.buf[0] == ':' && !u->ask[0]) {
		n = arg_list(u, abuf, cand, help, 32, &word, &wn);
		if (n < 0) {
			struct cmd_line l;
			cmd_parse(u->comp.buf + 1, &l);
			n = cmd_match(l.nw ? l.w[0] : "", l.nw ? l.n[0] : 0,
			              whimsy_is_owner(u->w, u->g), m, POP_ROWS);
		} else {
			args = 1;
			if (n > POP_ROWS) n = POP_ROWS;
		}
	}
	if (!n) return 0;
	if (u->pop_sel >= n) u->pop_sel = n - 1;

	uint8_t soft[3] = {255, 255, 255};
	float tx = x + c->pad + 4, iw = w - (c->pad + 4) * 2;
	size_t st[MAXWRAP + 1];

	if (u->ninfo) {                 /* a listed line wraps: a fingerprint must stay readable */
		int rows = 0;
		for (int i = 0; i < n; i++)
			rows += wrap_lines(u->info[i], strlen(u->info[i]), iw, draw_wrap, u->d, st, MAXWRAP);
		float h = (float)rows * lh + c->pad * 2, y = bottom - h - c->gap;
		draw_rect(u->d, x, y, w, h, soft, 13, c->radius);
		float ty = y + c->pad;
		for (int i = 0; i < n; i++) {
			size_t ln = strlen(u->info[i]);
			int nl = wrap_lines(u->info[i], ln, iw, draw_wrap, u->d, st, MAXWRAP);
			for (int k = 0; k < nl; k++, ty += lh)
				draw_text(u->d, tx, ty, u->info[i] + st[k], st[k + 1] - st[k], c->fg, DRAW_REGULAR);
		}
		return h + c->gap;
	}

	float h = (float)n * lh + c->pad * 2, y = bottom - h - c->gap;
	draw_rect(u->d, x, y, w, h, soft, 13, c->radius);
	if (args) {
		for (int i = 0; i < n; i++) {
			float ty = y + c->pad + (float)i * lh;
			float nx = draw_text(u->d, tx, ty, cand[i], strlen(cand[i]),
			                     i == u->pop_sel ? c->accent : c->fg, DRAW_REGULAR);
			if (help[i])
				draw_cut(u, nx + c->gap, ty, help[i], strlen(help[i]),
				         x + w - c->pad - (nx + c->gap), c->dim);
		}
		return h + c->gap;
	}
	for (int i = 0; i < n; i++) {
		float ty = y + c->pad + (float)i * lh;
		float nx = draw_text(u->d, tx, ty, m[i]->name, strlen(m[i]->name),
		                     i == u->pop_sel ? c->accent : c->fg, DRAW_REGULAR);
		nx = draw_text(u->d, nx + c->gap, ty, m[i]->args, strlen(m[i]->args), c->dim, DRAW_REGULAR);
		draw_cut(u, nx + c->gap * 2, ty, m[i]->help, strlen(m[i]->help),
		         x + w - c->pad - (nx + c->gap * 2), c->dim);
	}
	return h + c->gap;
}

/* the profile card: who a key is, its fingerprint, and the two things to do about it */
void profile_card(struct ui *u, float W, float H)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	char name[WHIMSY_MAX_PET + 1], fp[WHIMSY_FP_LEN];
	uint8_t col[3];
	int ver = whimsy_verified(u->w, u->card_pk);
	whimsy_petname(u->w, u->card_pk, name, sizeof name);
	whimsy_fingerprint(fp, u->card_pk);
	idcol(u, u->card_pk, col);

	float bw = 360, iw = bw - c->pad * 4;
	size_t st[MAXWRAP + 1];
	int fl = wrap_lines(fp, strlen(fp), iw, draw_wrap, u->d, st, MAXWRAP);
	float bh = c->pad * 4 + 44 + (float)fl * lh + lh;
	float x = (W - bw) / 2, y = (H - bh) / 2;

	draw_rect(u->d, x, y, bw, bh, c->bg, 244, c->radius);
	draw_rect(u->d, x, y, bw, 1, c->line, 255, 0);
	hit(u, x, y, bw, bh, H_CARD, 0, 0);

	float tx = x + c->pad * 2, ty = y + c->pad * 2;
	circle(u, tx + 20, ty + 20, 20, u->card_pk, 0, name, col);
	float nx = draw_cut(u, tx + 52, ty + 20 - lh / 2, name, strlen(name), bw - 100, col);
	if (ver) draw_text(u->d, nx + c->gap, ty + 20 - lh / 2, "\xe2\x9c\x93", 3, ok_col, DRAW_REGULAR);
	ty += 44 + c->pad;

	for (int k = 0; k < fl; k++, ty += lh)
		draw_text(u->d, tx, ty, fp + st[k], st[k + 1] - st[k], c->dim, DRAW_REGULAR);

	ty += c->pad / 2;
	const char *b1 = ver ? "verified" : "verify";
	float w1 = draw_measure(u->d, b1, strlen(b1), DRAW_REGULAR);
	draw_text(u->d, tx, ty, b1, strlen(b1), ver ? c->dim : c->accent, DRAW_REGULAR);
	if (!ver) hit(u, tx, ty, w1, lh, H_CBTN, 0, 0);
	draw_text(u->d, tx + w1 + c->pad * 2, ty, "message", 7, c->accent, DRAW_REGULAR);
	hit(u, tx + w1 + c->pad * 2, ty, draw_measure(u->d, "message", 7, DRAW_REGULAR), lh,
	    H_CBTN, 1, 0);
}

/* the image zoom and the text preview: one overlay, esc or a click closes it */
void file_overlay(struct ui *u, float W, float H)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	uint8_t dark[3] = {0, 0, 0};
	size_t n;
	const uint8_t *b = whimsy_file_open(u->w, u->g, u->over_i, &n);

	draw_rect(u->d, 0, 0, W, H, dark, 200, 0);
	hit(u, 0, 0, W, H, H_ZOOM, u->over_i, OV_NONE);
	if (!b) return;

	if (u->over == OV_IMAGE) {
		float iw, ih, maxh = H - c->pad * 8;
		SDL_Texture *t = tex_for(u, u->over_i, b, n, W - c->pad * 8, &iw, &ih);
		if (!t) return;
		if (ih > maxh) { iw *= maxh / ih; ih = maxh; }
		draw_image_at(u->d, t, (W - iw) / 2, (H - ih) / 2, iw, ih);
		return;
	}

	char text[8192];
	size_t off[64], len[64];
	size_t raw = n < sizeof text ? n : sizeof text;
	size_t sn = whimsy_sanitize(b, raw, text, sizeof text);
	if (sn >= sizeof text) sn = sizeof text - 1;
	int max = (int)((H - c->pad * 8) / lh);
	if (max > 64) max = 64;
	if (max < 1) max = 1;
	int nl = file_lines(text, sn, off, len, max);
	float bw = W - c->pad * 8, bh = (float)nl * lh + c->pad * 2;
	float x = c->pad * 4, y = (H - bh) / 2;
	draw_rect(u->d, x, y, bw, bh, c->bg, 244, c->radius);
	for (int k = 0; k < nl; k++)
		draw_cut(u, x + c->pad, y + c->pad + (float)k * lh, text + off[k], len[k],
		         bw - c->pad * 2, c->fg);
}

