#ifdef _WIN32
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#endif

#include "ui_int.h"


#include "audio.h"
#include "file.h"
#include "md.h"
#include "wrap.h"

/* ---- log ---- */

#define ROWPAD 5                /* mockups/style.css .msg: 5px above and below the text */

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
	const char *aext;               /* the decoder for an audio row, NULL otherwise */
	float iw, ih;
	char info[64];                  /* the size, the chunk count, or "no longer held" */
	char prev[PREV_BYTES];
	size_t off[PREV_LINES], len[PREV_LINES];
	int nl;
};

static void drop_tex(struct ui *u, size_t i)
{
	for (int k = 0; k < WHIMSY_HELD; k++)
		if (u->tex[k].b && u->tex[k].g == u->g && u->tex[k].i == i) {
			draw_image_free(u->tex[k].t);
			u->tex[k].t = NULL;
			u->tex[k].b = NULL;
		}
}

/* the texture for row i, decoded on first use; NULL when the bytes are not an image */
SDL_Texture *tex_for(struct ui *u, size_t i, const uint8_t *b, size_t n,
                            float maxw, float *w, float *h)
{
	int k;
	if (!b) return NULL;
	for (k = 0; k < WHIMSY_HELD; k++)
		if (u->tex[k].b == b && u->tex[k].g == u->g && u->tex[k].i == i) break;
	if (k == WHIMSY_HELD) {
		int iw = 0, ih = 0;
		for (k = 0; k < WHIMSY_HELD && u->tex[k].b; k++) ;
		if (k == WHIMSY_HELD) { draw_image_free(u->tex[0].t); k = 0; }
		u->tex[k].t = draw_image(u->d, b, n, &iw, &ih);
		u->tex[k].g = u->g;
		u->tex[k].i = i;
		u->tex[k].b = b;
		u->tex[k].w = iw;
		u->tex[k].h = ih;
	}
	if (!u->tex[k].t) return NULL;
	file_fit(u->tex[k].w, u->tex[k].h, maxw, w, h);
	return u->tex[k].t;
}

/* what a FILE row shows below its name: progress or size, then the image or the preview */
static void file_body(struct ui *u, size_t i, float bw, struct fbody *f)
{
	struct whimsy_msg m;
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
	if (whimsy_msg(u->w, u->g, i, &m) == WHIMSY_OK && (f->aext = audio_ext(m.text, m.text_n)))
		return;
	if ((f->t = tex_for(u, i, f->b, f->n, bw * 0.4f, &f->iw, &f->ih))) return;

	size_t raw = f->n < PREV_BYTES ? f->n : PREV_BYTES;
	size_t sn = whimsy_sanitize(f->b, raw, f->prev, sizeof f->prev);
	if (sn >= sizeof f->prev) sn = sizeof f->prev - 1;
	if (sn * 2 < raw) return;       /* mostly stripped: not text, no preview */
	f->nl = file_lines(f->prev, sn, f->off, f->len, PREV_LINES);
}

/* the audio card: two rows inside a 1px frame. draw_row_msg lays it out to match */
#define AUD_W 320
#define AUD_PADX 10
#define AUD_PADY 8
#define AUD_ROWGAP 5

static float aud_h(float lh) { return lh * 2 + AUD_ROWGAP + AUD_PADY * 2 + 2; }

static float file_h(struct ui *u, size_t i, float bw)
{
	struct fbody f;
	float lh = draw_line_height(u->d);
	file_body(u, i, bw, &f);
	if (f.aext) return aud_h(lh) + u->c->gap;
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

/* the mime for the clipboard, NULL when the name claims nothing we know */
const char *mime_of(const char *name)
{
	const char *e = ext_of(name);
	if (ci_same(e, "png")) return "image/png";
	if (ci_same(e, "jpg") || ci_same(e, "jpeg")) return "image/jpeg";
	if (ci_same(e, "gif")) return "image/gif";
	if (ci_same(e, "bmp")) return "image/bmp";
	if (ci_same(e, "wav")) return "audio/wav";
	if (ci_same(e, "mp3")) return "audio/mpeg";
	if (ci_same(e, "ogg") || ci_same(e, "oga")) return "audio/ogg";
	return NULL;
}

/* what the oversize prompt calls re-encodable: img_shrink only handles pictures */
int img_mime(const char *name)
{
	const char *m = mime_of(name);
	return m && !strncmp(m, "image/", 6);
}

/* the md style covering byte off, 0 outside every run */
static uint16_t style_at(const struct md_run *r, int nr, size_t off)
{
	for (int k = 0; k < nr; k++)
		if (off >= r[k].at && off < r[k].at + r[k].n) return r[k].style;
	return 0;
}

static int md_font(uint16_t st)
{
	return (st & MD_H1) ? DRAW_H1 : (st & MD_H2) ? DRAW_H2 :
	       (st & MD_BOLD) ? DRAW_BOLD : (st & MD_ITALIC) ? DRAW_ITALIC : DRAW_REGULAR;
}

/* wrap_measure that sizes header runs at their own size, so a header wraps where it
 * really ends. s points into ctx->b, which is what wrap_lines was handed */
struct wrapctx { struct draw *d; const char *b; const struct md_run *r; int nr; };

static float wrap_md(void *u, const char *s, size_t n)
{
	struct wrapctx *w = u;
	uint16_t st = style_at(w->r, w->nr, (size_t)(s - w->b));
	return draw_measure(w->d, s, n, (st & (MD_H1 | MD_H2)) ? md_font(st) : DRAW_REGULAR);
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
		float bh = lh + ROWPAD * 2 + (head ? lh : 0) + (date ? lh + c->gap : 0);
		if (i == u->newat) bh += lh + c->gap;
		if (i < u->nrh) u->rh[i] = bh;
		return bh;
	}
	struct md_run mr[64];
	int nmr = n ? md_scan(b, n, mr, 64) : 0;
	struct wrapctx wc = {u->d, b, mr, nmr};
	int nl = n ? wrap_lines(b, n, bw, wrap_md, &wc, starts, MAXWRAP) : 1;
	float h = ROWPAD * 2 + (head ? lh : 0) + (date ? lh + c->gap : 0);
	for (int k = 0; k < nl; k++)
		h += lh * (n ? draw_style_scale(md_font(style_at(mr, nmr, starts[k]))) : 1);
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

/* the selection, low end first; 0 when none stands */
static int sel_range(struct ui *u, size_t *r0, size_t *o0, size_t *r1, size_t *o1)
{
	if (!u->lsel || u->sa_row == (size_t)-1) return 0;
	int fwd = u->sa_row < u->sb_row || (u->sa_row == u->sb_row && u->sa_off <= u->sb_off);
	*r0 = fwd ? u->sa_row : u->sb_row;
	*o0 = fwd ? u->sa_off : u->sb_off;
	*r1 = fwd ? u->sb_row : u->sa_row;
	*o1 = fwd ? u->sb_off : u->sa_off;
	return *r0 != *r1 || *o0 != *o1;
}

/* the selected byte range of row i, empty when none of it is selected */
static void sel_of_row(struct ui *u, size_t i, size_t *lo, size_t *hi)
{
	size_t r0, o0, r1, o1;
	*lo = *hi = 0;
	if (!sel_range(u, &r0, &o0, &r1, &o1) || i < r0 || i > r1) return;
	*lo = i == r0 ? o0 : 0;
	*hi = i == r1 ? o1 : (size_t)-1;
}

void log_sel_at(struct ui *u, float mx, float my, size_t *row, size_t *off)
{
	const struct lspan *best = NULL;
	float bd = 0;
	struct whimsy_msg m;

	*row = (size_t)-1;
	*off = 0;
	for (int k = 0; k < u->nlspan; k++) {
		const struct lspan *s = &u->lspan[k];
		float dy = my < s->y ? s->y - my : my >= s->y + s->h ? my - (s->y + s->h) : 0;
		float dx = mx < s->x ? s->x - mx : mx >= s->x + s->w ? mx - (s->x + s->w) : 0;
		float d = dy * 10000 + dx;      /* a span on the pointer's own line always wins */
		if (!best || d < bd) { best = s; bd = d; }
	}
	if (!best) return;
	*row = best->row;
	*off = best->at;
	if (whimsy_msg(u->w, u->g, best->row, &m) != WHIMSY_OK) return;

	size_t j = best->at, end = best->at + best->len;
	float px = best->x;
	while (j < end) {
		size_t step = 1;
		float w;
		while (j + step < end && ((unsigned char)m.text[j + step] & 0xc0) == 0x80) step++;
		w = draw_measure(u->d, m.text + j, step, best->font);
		if (mx < px + w / 2) break;
		px += w;
		j += step;
	}
	*off = j;
}

int log_sel_copy(struct ui *u)
{
	size_t r0, o0, r1, o1, n = 0;
	uint16_t id = chan_id(u);
	char buf[4096];

	if (!sel_range(u, &r0, &o0, &r1, &o1)) return 0;
	for (size_t i = r0; i <= r1 && n < sizeof buf; i++) {
		struct whimsy_msg m;
		size_t lo, hi, len;
		if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK || m.channel != id || m.blocked) continue;
		lo = i == r0 ? o0 : 0;
		hi = i == r1 ? o1 : m.text_n;
		if (hi > m.text_n) hi = m.text_n;
		if (lo >= hi) continue;
		if (n) buf[n++] = '\n';
		len = hi - lo;
		if (len > sizeof buf - n) len = sizeof buf - n;
		memcpy(buf + n, m.text + lo, len);
		n += len;
	}
	if (!n) return 0;
	clip_set(buf, n);
	return 1;
}

/* one wrapped line, cut at the md run boundaries. markers stay in the bytes drawn */
static void draw_md_line(struct ui *u, size_t i, float x, float y, const char *b,
                         size_t at, size_t len, const struct md_run *r, int nr, int shown)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d) * draw_style_scale(md_font(style_at(r, nr, at)));
	size_t end = at + len;

	for (size_t p = at; p < end; ) {
		uint16_t st = 0;
		size_t seg = end;
		for (int k = 0; k < nr; k++) {
			if (p < r[k].at) { if (r[k].at < seg) seg = r[k].at; continue; }
			if (p < r[k].at + r[k].n) { st = r[k].style; if (r[k].at + r[k].n < seg) seg = r[k].at + r[k].n; break; }
		}
		int font = md_font(st);
		const uint8_t *col = (st & (MD_LINK | MD_MENTION)) ? c->accent :
		                     (st & MD_QUOTE) ? c->dim : c->fg;
		float w = draw_measure(u->d, b + p, seg - p, font);
		int hidden = (st & MD_SPOILER) && !shown;
		if (u->nlspan < LSPANS)
			u->lspan[u->nlspan++] = (struct lspan){i, p, seg - p, x, y, w, lh, font};
		if (st & MD_CODE) draw_rect(u->d, x, y, w, lh, c->line, 90, c->radius / 2);
		{                               /* the selected slice of this run, under its text */
			size_t lo, hi, s0, s1;
			sel_of_row(u, i, &lo, &hi);
			s0 = p > lo ? p : lo;
			s1 = seg < hi ? seg : hi;
			if (s0 < s1)
				draw_rect(u->d, x + draw_measure(u->d, b + p, s0 - p, font), y,
				          draw_measure(u->d, b + s0, s1 - s0, font), lh, c->sel, 150, 0);
		}
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

/* the pointer-over-a-row strip: one framed pill floating over the row's top right, so
 * nothing below it reflows. nerd font icons, words on a face without them */
static void hover_strip(struct ui *u)
{
	struct conf *c = u->c;
	struct whimsy_msg m;
	size_t i = u->strip_i;
	if (whimsy_msg(u->w, u->g, i, &m) != WHIMSY_OK) return;

	float lh = draw_line_height(u->d);
	const float pad = 8, step = 4;   /* px, not glyph advances: an icon is wider than its cell */
	struct item { const char *s; size_t n; int kind; size_t b; float w; } it[REACT_SLOTS + 4];
	int n = 0;

	for (int k = 0; k < REACT_SLOTS; k++) {
		size_t en;
		const char *em = react_nth(c->reacts, k, &en);
		if (!em) break;
		it[n++] = (struct item){em, en, H_RCT, (size_t)k, 0};
	}
	int nre = n;

	static const uint32_t ICON[4] = {0xf112, 0xf044, 0xf1f8, 0xf0c5};
	static const char *const LAB[4] = {"reply", "edit", "delete", "copy"};
	static const int ACT[4] = {A_REPLY, A_EDIT, A_DEL, A_COPY};
	static char txt[4][8];           /* one utf-8 icon each, or the word on a face without it */
	for (int k = 0; k < 4; k++) {
		if ((k == 1 || k == 2) && !m.mine) continue;
		uint32_t cp = ICON[k];
		size_t tn;
		if (draw_has(u->d, cp)) {
			txt[k][0] = (char)(0xe0 | cp >> 12);
			txt[k][1] = (char)(0x80 | (cp >> 6 & 0x3f));
			txt[k][2] = (char)(0x80 | (cp & 0x3f));
			tn = 3;
		} else {
			tn = strlen(LAB[k]);
			memcpy(txt[k], LAB[k], tn);
		}
		it[n++] = (struct item){txt[k], tn, H_ACT, (size_t)ACT[k], 0};
	}

	/* every item sits in a cell at least as wide as it is tall, so the spacing is even
	 * whatever each glyph's advance claims */
	float w = 0;
	for (int k = 0; k < n; k++) {
		it[k].w = draw_measure(u->d, it[k].s, it[k].n, DRAW_REGULAR);
		if (it[k].w < lh) it[k].w = lh;
		w += it[k].w + (k ? step : 0);
	}
	if (nre && nre < n) w += step * 2;                       /* the divider's own column */

	float ph = lh + pad * 2, pw = w + pad * 2;
	float px = u->strip_x - pw, py = u->strip_y - pad;
	float rad = c->radius;
	/* the same raised surface profile_card uses: written over the window's own alpha,
	 * so the glass behind it does not read through */
	uint8_t surf[3];
	for (int k = 0; k < 3; k++) surf[k] = (uint8_t)((c->bg[k] * 3 + c->line[k]) / 4);
	draw_rect(u->d, px, py, pw, ph, c->line, 120, rad);
	draw_rect_over(u->d, px + 1, py + 1, pw - 2, ph - 2, surf, 250, rad - 1);

	float x = px + pad;
	for (int k = 0; k < n; k++) {
		if (k == nre && nre && nre < n) {
			draw_rect(u->d, x + step - 1, py + (ph - lh) / 2, 1, lh, c->line, 255, 0);
			x += step * 2;
		}
		float tw = draw_measure(u->d, it[k].s, it[k].n, DRAW_REGULAR);
		draw_text(u->d, x + (it[k].w - tw) / 2, u->strip_y, it[k].s, it[k].n, c->dim, DRAW_REGULAR);
		hit_top(u, x - step / 2, py, it[k].w + step, ph, it[k].kind, i, it[k].b);
		x += it[k].w + step;
	}
}

/* the .aud card: name, save, copy over play, track, elapsed/total, in one framed box */
static void audio_card(struct ui *u, size_t i, float bx, float y, float bw,
                       const char *name, size_t nn)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	float cw = bw < AUD_W ? bw : AUD_W, ch = aud_h(lh);
	float ix = bx + 1 + AUD_PADX, iw = cw - 2 - AUD_PADX * 2;
	float y1 = y + 1 + AUD_PADY, y2 = y1 + lh + AUD_ROWGAP;
	static const char *const act[2] = {"save", "copy"};
	static const int what[2] = {A_SAVE, A_FCOPY};
	float aw[2], accw = 0;
	char t[32];
	float frac = 0, total = 0;
	int st = audio_state(u->g, i, &frac);
	int tn;

	if (iw <= 0) return;
	draw_rect(u->d, bx, y, cw, ch, c->line, 255, c->radius);
	draw_rect(u->d, bx + 1, y + 1, cw - 2, ch - 2, c->bg, 40, c->radius - 1);

	for (int k = 0; k < 2; k++) {
		aw[k] = draw_measure(u->d, act[k], 4, DRAW_REGULAR);
		accw += aw[k] + c->pad;
	}
	draw_cut(u, ix, y1, name, nn, iw - accw, c->dim);
	float ax = ix + iw - accw + c->pad;
	for (int k = 0; k < 2; k++) {
		draw_text(u->d, ax, y1, act[k], 4, c->accent, DRAW_REGULAR);
		hit(u, ax, y1, aw[k], lh, H_ACT, i, (size_t)what[k]);
		ax += aw[k] + c->pad;
	}

	const char *lab = st == AUDIO_PLAYING ? "pause" : "play";
	size_t ln = strlen(lab);
	float pw = draw_measure(u->d, lab, ln, DRAW_REGULAR);
	draw_text(u->d, ix, y2, lab, ln, c->accent, DRAW_REGULAR);
	hit(u, ix, y2, pw, lh, H_AUDIO, i, 0);

	if (st) total = audio_total();
	if (total > 0)
		tn = snprintf(t, sizeof t, "%d:%02d / %d:%02d", (int)(frac * total) / 60,
		              (int)(frac * total) % 60, (int)total / 60, (int)total % 60);
	else
		tn = snprintf(t, sizeof t, "%d:%02d", 0, 0);
	float tw = draw_measure(u->d, t, (size_t)tn, DRAW_REGULAR);
	draw_text(u->d, ix + iw - tw, y2, t, (size_t)tn, c->dim, DRAW_REGULAR);

	float trx = ix + pw + AUD_PADX, trw = iw - pw - tw - AUD_PADX * 2;
	if (trw <= 0) return;
	float ty = y2 + lh / 2 - 1.5f;
	draw_rect(u->d, trx, ty, trw, 3, c->line, 200, 2);
	if (st) draw_rect(u->d, trx, ty, trw * frac, 3, c->accent, 255, 2);
	hit(u, trx, y2, trw, lh, H_AUDIO, i, 1);
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

	/* named us, or answers something of ours: the same accent the mention wears */
	size_t rto = find_id(u, m.reply);
	struct whimsy_msg rm;
	int at_me = m.mentions ||
	            (!m.mine && rto != (size_t)-1 && whimsy_msg(u->w, u->g, rto, &rm) == WHIMSY_OK && rm.mine);
	int armed = u->mode && i == u->mode_i;
	if (hov || armed || at_me) {
		float skip = 0;
		if (i == u->newat) skip += lh + c->gap;
		if (date) skip += lh + c->gap;
		draw_rect(u->d, x + MIDPAD - c->pad, y + skip, w - MIDPAD * 2 + c->pad * 2, rh - skip,
		          armed || at_me ? c->accent : c->sel,
		          armed ? 26 : at_me ? (hov ? 34 : 18) : 40, c->radius);
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
	y += ROWPAD;
	if (hov) { u->strip_on = 1; u->strip_i = i; u->strip_x = x + w - MIDPAD; u->strip_y = y; }
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

	size_t to = rto;
	if (to != (size_t)-1) {                 /* one quoted line; clicking it jumps */
		struct whimsy_msg q;
		if (whimsy_msg(u->w, u->g, to, &q) == WHIMSY_OK) {
			char nm[WHIMSY_MAX_PET + 1], qt[128];
			uint8_t qc[3];
			size_t qn = q.text_n > sizeof qt ? sizeof qt : q.text_n;
			whimsy_petname(u->w, q.sender, nm, sizeof nm);
			idcol(u, q.sender, qc);
			memcpy(qt, q.text, qn);
			for (size_t k = 0; k < qn; k++) if (qt[k] == '\n') qt[k] = ' ';
			draw_rect(u->d, bx, y, 2, lh, qc, 255, 0);
			float qx = draw_text(u->d, bx + 10, y, "\xe2\x86\xb0", 3, c->dim, DRAW_REGULAR);
			qx = draw_text(u->d, qx + c->gap, y, nm, strlen(nm), qc, DRAW_REGULAR);
			qx += c->gap;
			draw_cut(u, qx, y, qt, qn, bx + bw - qx, c->dim);
			hit(u, bx, y, bw, lh, H_GOTO, to, 0);
		}
		y += lh;
	}

	size_t n, starts[MAXWRAP + 1];
	const char *b = m.text;
	n = m.text_n;
	struct md_run run[64];
	int nrun = n ? md_scan(b, n, run, 64) : 0;
	struct wrapctx wc = {u->d, b, run, nrun};
	int nl = n ? wrap_lines(b, n, bw, wrap_md, &wc, starts, MAXWRAP) : 0;
	for (int k = 0; k < nl; k++) {
		float klh = lh * draw_style_scale(md_font(style_at(run, nrun, starts[k])));
		size_t len = starts[k + 1] - starts[k];
		while (len && b[starts[k] + len - 1] == '\n') len--;
		highlight(u, bx, y, b + starts[k], len);
		draw_md_line(u, i, bx, y, b, starts[k], len, run, nrun, i == u->spoil);
		if (k == nl - 1 && m.edited)
			draw_text(u->d, bx + draw_measure(u->d, b + starts[k], len, DRAW_REGULAR) + c->gap,
			          y, "edited", 6, c->dim, DRAW_REGULAR);
		y += klh;
	}

	if (m.kind == WHIMSY_K_FILE) {
		struct fbody f;
		file_body(u, i, bw, &f);
		if (f.aext) {
			audio_card(u, i, bx, y, bw, m.text, m.text_n);
			y += aud_h(lh) + c->gap;
		} else {
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
		}
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
	u->nlspan = 0;
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
	u->strip_on = 0;
	for (int k = nvis; k-- > 0; )
		if (top[k] < y + h) draw_row_msg(u, idx[k], x, top[k], w, bw);
	if (u->strip_on) hover_strip(u);

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

