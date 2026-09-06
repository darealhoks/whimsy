#include "ui_int.h"


#include "file.h"
#include "md.h"
#include "wrap.h"

/* ---- log ---- */

static size_t prev_in_chan(struct ui *u, size_t i, uint16_t id)
{
	struct whimsy_msg m;
	while (i--) {
		if (whimsy_msg(u->w, u->g, i, &m) == WHIMSY_OK && m.channel == id) return i;
	}
	return (size_t)-1;
}

static int same_day(uint64_t a, uint64_t b)
{
	time_t ta = (time_t)a, tb = (time_t)b;
	struct tm x, y;
	localtime_r(&ta, &x);
	localtime_r(&tb, &y);
	return x.tm_yday == y.tm_yday && x.tm_year == y.tm_year;
}

/* header when the row starts a run, date rule when the day turned */
static void row_shape(struct ui *u, size_t i, const struct whimsy_msg *m, int *head, int *date)
{
	size_t p = prev_in_chan(u, i, m->channel);
	struct whimsy_msg pm;
	if (p == (size_t)-1 || whimsy_msg(u->w, u->g, p, &pm) != WHIMSY_OK) {
		*head = 1;
		*date = 1;
		return;
	}
	*date = !same_day(pm.time, m->time);
	*head = *date || memcmp(pm.sender, m->sender, WHIMSY_PK) ||
	        m->time > pm.time + GROUP_SECS;
}

/* the message with that id, or (size_t)-1; a reply names one we may not hold */
static size_t find_id(struct ui *u, const uint8_t *id)
{
	struct whimsy_msg m;
	if (!id) return (size_t)-1;
	for (size_t i = whimsy_msg_count(u->w, u->g); i-- > 0; )
		if (whimsy_msg(u->w, u->g, i, &m) == WHIMSY_OK && m.id &&
		    !memcmp(m.id, id, WHIMSY_MSGID)) return i;
	return (size_t)-1;
}

int row_pills(struct ui *u, size_t i, struct pill *out, int cap)
{
	struct whimsy_react r[32];
	size_t n = whimsy_reactions(u->w, u->g, i, r, 32);
	if (n > 32) n = 32;
	return react_pills(r, n, u->self, out, cap);
}

/* ---- files ---- */

#define PREV_LINES 8
#define PREV_BYTES 1024

struct fbody {
	const uint8_t *b;
	size_t n, done, total;
	SDL_Texture *t;
	float iw, ih;
	char info[64];                  /* the size, the chunk count, or "no longer held" */
	char prev[PREV_BYTES];
	size_t off[PREV_LINES], len[PREV_LINES];
	int nl;
};

static void drop_tex(struct ui *u, size_t i)
{
	for (int k = 0; k < WHIMSY_HELD; k++)
		if (u->tex[k].t && u->tex[k].g == u->g && u->tex[k].i == i) {
			draw_image_free(u->tex[k].t);
			u->tex[k].t = NULL;
		}
}

/* the texture for row i, decoded on first use; NULL when the bytes are not an image */
SDL_Texture *tex_for(struct ui *u, size_t i, const uint8_t *b, size_t n,
                            float maxw, float *w, float *h)
{
	int k;
	for (k = 0; k < WHIMSY_HELD; k++)
		if (u->tex[k].t && u->tex[k].g == u->g && u->tex[k].i == i) break;
	if (k == WHIMSY_HELD) {
		int iw, ih;
		SDL_Texture *t;
		for (k = 0; k < WHIMSY_HELD && u->tex[k].t; k++) ;
		if (k == WHIMSY_HELD) { draw_image_free(u->tex[0].t); u->tex[0].t = NULL; k = 0; }
		if (!(t = draw_image(u->d, b, n, &iw, &ih))) return NULL;
		u->tex[k].g = u->g;
		u->tex[k].i = i;
		u->tex[k].t = t;
		u->tex[k].w = iw;
		u->tex[k].h = ih;
	}
	file_fit(u->tex[k].w, u->tex[k].h, maxw, w, h);
	return u->tex[k].t;
}

/* what a FILE row shows below its name: progress or size, then the image or the preview */
static void file_body(struct ui *u, size_t i, float bw, struct fbody *f)
{
	memset(f, 0, sizeof *f);
	whimsy_file_progress(u->w, u->g, i, &f->done, &f->total);
	f->b = whimsy_file_open(u->w, u->g, i, &f->n);
	if (!f->b) {
		drop_tex(u, i);
		if (f->total && f->done < f->total)
			snprintf(f->info, sizeof f->info, "%zu/%zu", f->done, f->total);
		else
			snprintf(f->info, sizeof f->info, "no longer held");
		return;
	}
	file_human(f->n, f->info, sizeof f->info);
	if ((f->t = tex_for(u, i, f->b, f->n, bw * 0.4f, &f->iw, &f->ih))) return;

	size_t raw = f->n < PREV_BYTES ? f->n : PREV_BYTES;
	size_t sn = whimsy_sanitize(f->b, raw, f->prev, sizeof f->prev);
	if (sn >= sizeof f->prev) sn = sizeof f->prev - 1;
	if (sn * 2 < raw) return;       /* mostly stripped: not text, no preview */
	f->nl = file_lines(f->prev, sn, f->off, f->len, PREV_LINES);
}

static float file_h(struct ui *u, size_t i, float bw)
{
	struct fbody f;
	float lh = draw_line_height(u->d);
	file_body(u, i, bw, &f);
	if (f.t) return lh + f.ih + u->c->gap;
	if (f.nl) return lh + (float)f.nl * lh + u->c->gap;
	return lh;
}

static const char *ext_of(const char *name)
{
	const char *d = strrchr(name, '.');
	return d ? d + 1 : "";
}

static int ci_same(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return !*a && !*b;
}

/* the mime for the clipboard, and what the oversize prompt calls re-encodable */
const char *mime_of(const char *name)
{
	const char *e = ext_of(name);
	if (ci_same(e, "png")) return "image/png";
	if (ci_same(e, "jpg") || ci_same(e, "jpeg")) return "image/jpeg";
	if (ci_same(e, "gif")) return "image/gif";
	if (ci_same(e, "bmp")) return "image/bmp";
	return NULL;
}

float row_height(struct ui *u, size_t i, float bw)
{
	struct conf *c = u->c;
	struct whimsy_msg m;
	struct pill p[16];
	if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK) return 0;
	if (i < u->nrh && u->rh[i] > 0) return u->rh[i];

	float lh = draw_line_height(u->d);
	int head, date;
	size_t n, starts[MAXWRAP + 1];
	const char *b = m.text;
	n = m.text_n;
	row_shape(u, i, &m, &head, &date);
	if (m.blocked) {            /* the placeholder row: same shape as the separators above */
		float bh = lh + 5 + (head ? lh : 0) + (date ? lh + c->gap : 0);
		if (i == u->newat) bh += lh + c->gap;
		if (i < u->nrh) u->rh[i] = bh;
		return bh;
	}
	int nl = n ? wrap_lines(b, n, bw, draw_wrap, u->d, starts, MAXWRAP) : 1;
	float h = (float)nl * lh + 5 + (head ? lh : 0) + (date ? lh + c->gap : 0);
	if (i == u->newat) h += lh + c->gap;
	if (find_id(u, m.reply) != (size_t)-1) h += lh;
	if (row_pills(u, i, p, 16)) h += lh + 4;
	if (m.kind == WHIMSY_K_FILE) h += file_h(u, i, bw);
	if (i < u->nrh) u->rh[i] = h;
	return h;
}

static int ci_eq(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return 1;
}

/* accent bands under every occurrence of the live search needle in one run */
static void highlight(struct ui *u, float x, float y, const char *s, size_t n)
{
	size_t fn = strlen(u->find);
	if (!fn || fn > n) return;
	float lh = draw_line_height(u->d);
	for (size_t i = 0; i + fn <= n; i++) {
		if (!ci_eq(s + i, u->find, fn)) continue;
		float bx = x + draw_measure(u->d, s, i, DRAW_REGULAR);
		draw_rect(u->d, bx, y, draw_measure(u->d, s + i, fn, DRAW_REGULAR), lh,
		          u->c->accent, 70, 0);
		i += fn - 1;
	}
}

/* one wrapped line, cut at the md run boundaries. markers stay in the bytes drawn */
static void draw_md_line(struct ui *u, size_t i, float x, float y, const char *b,
                         size_t at, size_t len, const struct md_run *r, int nr, int shown)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	size_t end = at + len;

	for (size_t p = at; p < end; ) {
		uint16_t st = 0;
		size_t seg = end;
		for (int k = 0; k < nr; k++) {
			if (p < r[k].at) { if (r[k].at < seg) seg = r[k].at; continue; }
			if (p < r[k].at + r[k].n) { st = r[k].style; if (r[k].at + r[k].n < seg) seg = r[k].at + r[k].n; break; }
		}
		int font = (st & (MD_BOLD | MD_H1 | MD_H2)) ? DRAW_BOLD
		         : (st & MD_ITALIC) ? DRAW_ITALIC : DRAW_REGULAR;
		const uint8_t *col = (st & MD_LINK) ? c->accent : (st & MD_QUOTE) ? c->dim : c->fg;
		float w = draw_measure(u->d, b + p, seg - p, font);
		int hidden = (st & MD_SPOILER) && !shown;
		if (st & MD_CODE) draw_rect(u->d, x, y, w, lh, c->line, 90, c->radius / 2);
		if (hidden) draw_rect(u->d, x, y, w, lh, c->dim, 235, c->radius / 2);
		else draw_text(u->d, x, y, b + p, seg - p, col, font);
		if ((st & MD_STRIKE) && !hidden) draw_rule(u->d, x, y + lh / 2, w, col);
		if ((st & MD_LINK) && !hidden) hit(u, x, y, w, lh, H_LINK, i, p);
		if (st & MD_SPOILER) hit(u, x, y, w, lh, H_SPOIL, i, 0);
		x += w;
		p = seg;
	}
}

static void draw_rule_label(struct ui *u, float x, float y, float w, const char *s)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	float tw = draw_measure(u->d, s, strlen(s), DRAW_REGULAR);
	float mid = x + (w - tw) / 2;
	draw_rule(u->d, x, y + lh / 2, mid - x - c->gap, c->line);
	draw_text(u->d, mid, y, s, strlen(s), c->dim, DRAW_REGULAR);
	draw_rule(u->d, mid + tw + c->gap, y + lh / 2, x + w - (mid + tw + c->gap), c->line);
}

/* the words that show at the right of the head line while the pointer is over a row */
static float hover_strip(struct ui *u, size_t i, const struct whimsy_msg *m, float rx, float y)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), w = 0;
	const char *lab[4];
	int act[4], nlab = 0;

	lab[nlab] = "reply"; act[nlab++] = A_REPLY;
	if (m->mine) { lab[nlab] = "edit"; act[nlab++] = A_EDIT; }
	if (m->mine) { lab[nlab] = "delete"; act[nlab++] = A_DEL; }
	lab[nlab] = "copy"; act[nlab++] = A_COPY;
	for (int k = 0; k < nlab; k++)
		w += draw_measure(u->d, lab[k], strlen(lab[k]), DRAW_REGULAR) + c->gap;

	int nre = 0;
	const char *re[8];
	size_t ren[8];
	for (; nre < 8; nre++) {
		re[nre] = react_nth(c->reacts, nre, &ren[nre]);
		if (!re[nre]) break;
		w += draw_measure(u->d, re[nre], ren[nre], DRAW_REGULAR) + c->gap;
	}

	float x = rx - w + c->gap;
	for (int k = 0; k < nre; k++) {
		float tw = draw_measure(u->d, re[k], ren[k], DRAW_REGULAR);
		draw_text(u->d, x, y, re[k], ren[k], c->dim, DRAW_REGULAR);
		hit(u, x - c->gap / 2, y - 2, tw + c->gap, lh + 4, H_RCT, i, (size_t)k);
		x += tw + c->gap;
	}
	for (int k = 0; k < nlab; k++) {
		size_t ln = strlen(lab[k]);
		float tw = draw_measure(u->d, lab[k], ln, DRAW_REGULAR);
		draw_text(u->d, x, y, lab[k], ln, c->dim, DRAW_REGULAR);
		hit(u, x - c->gap / 2, y - 2, tw + c->gap, lh + 4, H_ACT, i, (size_t)act[k]);
		x += tw + c->gap;
	}
	return w;
}

static void draw_row_msg(struct ui *u, size_t i, float x, float y, float w, float bw)
{
	struct conf *c = u->c;
	struct whimsy_msg m;
	if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK) return;
	float lh = draw_line_height(u->d), rh = row_height(u, i, bw);
	int head, date;
	row_shape(u, i, &m, &head, &date);
	float bx = x + MIDPAD + AV + 10;
	int hov = u->my >= y && u->my < y + rh && u->mx >= x && u->mx < x + w;
	if (m.blocked) hov = 0;         /* nothing to reply to, edit, copy or react to */

	int armed = u->mode && i == u->mode_i;
	if (hov || armed) {
		float skip = 0;
		if (i == u->newat) skip += lh + c->gap;
		if (date) skip += lh + c->gap;
		draw_rect2(u->d, x + MIDPAD - 4, y + skip, w - MIDPAD * 2 + 8, rh - skip,
		           armed ? c->accent : c->sel, armed ? 26 : 40, 0, c->radius / 2);
		if (armed)
			draw_rect(u->d, x + MIDPAD - 4, y + skip, 2, rh - skip, c->accent, 255, 0);
	}
	if (i == u->newat) {
		draw_rule_label(u, x + MIDPAD, y, w - MIDPAD * 2, "new");
		y += lh + c->gap;
	}
	if (date) {
		char day[32];
		time_t t = (time_t)m.time;
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(day, sizeof day, "%Y-%m-%d", &tm);
		draw_rule_label(u, x + MIDPAD, y, w - MIDPAD * 2, day);
		y += lh + c->gap;
	}
	float hw = hov ? hover_strip(u, i, &m, x + w - MIDPAD, y) : 0;
	if (head) {
		char name[128], hhmm[8];
		uint8_t col[3];
		time_t t = (time_t)m.time;
		struct tm tm;
		whimsy_petname(u->w, m.sender, name, sizeof name);
		idcol(u, m.sender, col);
		localtime_r(&t, &tm);
		strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
		circle(u, x + MIDPAD + AV / 2, y + AV / 2, AV / 2, m.sender, 0, name, col);
		float nx = draw_text(u->d, bx, y, name, strlen(name), col, DRAW_REGULAR);
		hit(u, x + MIDPAD, y, nx - x - MIDPAD, lh, H_PROF, i, 1);
		if (whimsy_verified(u->w, m.sender))
			nx = draw_text(u->d, nx + c->gap / 2, y, "\xe2\x9c\x93", 3, ok_col, DRAW_REGULAR);
		draw_text(u->d, nx + c->gap, y, hhmm, strlen(hhmm), c->dim, DRAW_REGULAR);
		y += lh;
	}

	if (m.blocked) {                /* the bytes are still in the store; :block again shows them */
		draw_text(u->d, bx, y, "blocked message", 15, c->dim, DRAW_REGULAR);
		return;
	}

	size_t to = find_id(u, m.reply);
	if (to != (size_t)-1) {                 /* one quoted line; clicking it jumps */
		struct whimsy_msg q;
		if (whimsy_msg(u->w, u->g, to, &q) == WHIMSY_OK) {
			char lead[160];
			size_t qn;
			const char *qb = q.text;
			qn = q.text_n;
			char nm[WHIMSY_MAX_PET + 1];
			whimsy_petname(u->w, q.sender, nm, sizeof nm);
			int ln = snprintf(lead, sizeof lead, "\xe2\x94\x82 %s: %.*s", nm,
			                  (int)(qn > 100 ? 100 : qn), qb);
			if (ln >= (int)sizeof lead) ln = (int)sizeof lead - 1;
			for (int k = 0; k < ln; k++) if (lead[k] == '\n') lead[k] = ' ';
			/* the strip shares this line when the row has no head, so give way to it */
			draw_cut(u, bx, y, lead, (size_t)ln, head ? bw : bw - hw, c->dim);
			hit(u, bx, y, bw, lh, H_GOTO, to, 0);
		}
		y += lh;
	}

	size_t n, starts[MAXWRAP + 1];
	const char *b = m.text;
	n = m.text_n;
	struct md_run run[64];
	int nrun = n ? md_scan(b, n, run, 64) : 0;
	int nl = n ? wrap_lines(b, n, bw, draw_wrap, u->d, starts, MAXWRAP) : 0;
	for (int k = 0; k < nl; k++) {
		size_t len = starts[k + 1] - starts[k];
		while (len && b[starts[k] + len - 1] == '\n') len--;
		highlight(u, bx, y, b + starts[k], len);
		draw_md_line(u, i, bx, y, b, starts[k], len, run, nrun, i == u->spoil);
		if (k == nl - 1 && m.edited)
			draw_text(u->d, bx + draw_measure(u->d, b + starts[k], len, DRAW_REGULAR) + c->gap,
			          y, "edited", 6, c->dim, DRAW_REGULAR);
		y += lh;
	}

	if (m.kind == WHIMSY_K_FILE) {
		struct fbody f;
		file_body(u, i, bw, &f);
		float fx = draw_text(u->d, bx, y, f.info, strlen(f.info), c->dim, DRAW_REGULAR);
		if (f.b) {
			static const char *const act[2] = {"save", "copy"};
			static const int what[2] = {A_SAVE, A_FCOPY};
			for (int k = 0; k < 2; k++) {
				float tw = draw_measure(u->d, act[k], 4, DRAW_REGULAR);
				fx += c->pad;
				draw_text(u->d, fx, y, act[k], 4, c->accent, DRAW_REGULAR);
				hit(u, fx, y, tw, lh, H_ACT, i, (size_t)what[k]);
				fx += tw;
			}
		}
		y += lh;
		if (f.t) {
			draw_image_at(u->d, f.t, bx, y, f.iw, f.ih);
			hit(u, bx, y, f.iw, f.ih, H_ZOOM, i, OV_IMAGE);
			y += f.ih + c->gap;
		} else if (f.nl) {
			float ph = (float)f.nl * lh;
			draw_rect(u->d, bx, y, bw, ph, c->line, 90, c->radius);
			for (int k = 0; k < f.nl; k++)
				draw_cut(u, bx + c->pad / 2, y + (float)k * lh, f.prev + f.off[k],
				         f.len[k], bw - c->pad, c->dim);
			hit(u, bx, y, bw, ph, H_ZOOM, i, OV_TEXT);
			y += ph + c->gap;
		}
	}

	struct pill p[16];
	int np = row_pills(u, i, p, 16);
	float px = bx;
	for (int k = 0; k < np; k++) {
		char cnt[16];
		int cn = snprintf(cnt, sizeof cnt, " %d", p[k].n);
		const uint8_t *pc = p[k].mine ? c->fg : c->dim;
		if (k) {
			float dw = draw_measure(u->d, " \xc2\xb7 ", 4, DRAW_REGULAR);
			draw_text(u->d, px, y, " \xc2\xb7 ", 4, c->dim, DRAW_REGULAR);
			px += dw;
		}
		float ex = draw_text(u->d, px, y, p[k].text, p[k].text_n, pc, DRAW_REGULAR);
		float nx = draw_text(u->d, ex, y, cnt, (size_t)cn, pc, DRAW_REGULAR);
		hit(u, px, y, nx - px, lh, H_PILL, i, (size_t)k);
		px = nx;
	}
}

/* newest first up the viewport; only what shows is laid out */
void log_pane(struct ui *u, float x, float y, float w, float h)
{
	struct conf *c = u->c;
	float bw = w - MIDPAD * 2 - AV - 10;
	u->log_y = y; u->log_h = h;
	if (bw != u->cache_w) { u->cache_w = bw; invalidate(u); }
	size_t n = whimsy_msg_count(u->w, u->g);
	uint16_t id = chan_id(u);
	size_t idx[256];
	float top[256];
	int nvis = 0;
	float cur = y + h + u->scroll;

	struct whimsy_msg m;
	for (size_t i = n; i-- > 0 && nvis < (int)(sizeof idx / sizeof *idx); ) {
		if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK || m.channel != id) continue;
		cur -= row_height(u, i, bw);
		idx[nvis] = i;
		top[nvis++] = cur;
		if (cur <= y) break;
	}
	/* scrolled past the oldest row: pull the whole run back down */
	if (nvis && top[nvis - 1] > y && u->scroll > 0) {
		float back = top[nvis - 1] - y;
		if (back > u->scroll) back = u->scroll;
		u->scroll -= back;
		for (int k = 0; k < nvis; k++) top[k] -= back;
	}
	if (!nvis) {
		draw_text(u->d, x + MIDPAD, y + c->pad, "no messages here yet", 20, c->dim, DRAW_REGULAR);
		return;
	}
	for (int k = nvis; k-- > 0; )
		if (top[k] < y + h) draw_row_msg(u, idx[k], x, top[k], w, bw);

	/* scrolled up with arrivals below: the pill that jumps back down */
	size_t un = whimsy_unread(u->w, u->g, id);
	if (u->scroll > 0 && un) {
		char lab[32];
		float lh = draw_line_height(u->d);
		int ln = snprintf(lab, sizeof lab, "%zu new", un);
		float tw = draw_measure(u->d, lab, (size_t)ln, DRAW_REGULAR) + c->pad * 2;
		float px = x + w - MIDPAD - tw, py = y + h - lh - c->pad;
		draw_rect(u->d, px, py, tw, lh + 4, c->accent, 190, c->radius);
		draw_text(u->d, px + c->pad, py + 2, lab, (size_t)ln, c->bg, DRAW_REGULAR);
		hit(u, px, py, tw, lh + 4, H_NEWPILL, 0, 0);
	}
}

