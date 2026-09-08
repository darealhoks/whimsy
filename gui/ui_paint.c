#include "ui_int.h"


#include "file.h"

/* ---- helpers ---- */

/* s cut to maxw with a trailing ellipsis; returns the x after the run */
float draw_cut(struct ui *u, float x, float y, const char *s, size_t n, float maxw,
                      const uint8_t rgb[3])
{
	static const char ell[] = "\xe2\x80\xa6";
	if (maxw <= 0) return x;
	if (draw_measure(u->d, s, n, DRAW_REGULAR) <= maxw)
		return draw_text(u->d, x, y, s, n, rgb, DRAW_REGULAR);
	float ew = draw_measure(u->d, ell, 3, DRAW_REGULAR);
	while (n && draw_measure(u->d, s, n, DRAW_REGULAR) + ew > maxw) {
		n--;
		while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
	}
	x = draw_text(u->d, x, y, s, n, rgb, DRAW_REGULAR);
	return draw_text(u->d, x, y, ell, 3, rgb, DRAW_REGULAR);
}

/* pane widths for this frame; the middle keeps MID_MIN, members give way first */
static void panes(struct ui *u, float W, float *sw, float *mw)
{
	if (u->side_w < SIDE_MIN) u->side_w = SIDE_MIN;
	if (u->memb_w < MEMB_MIN) u->memb_w = MEMB_MIN;
	*sw = (W >= WIDE || u->side_open) ? u->side_w : 0;
	*mw = u->memb_open ? u->memb_w : 0;
	float over = *sw + *mw + MID_MIN - W, take;
	if (over <= 0) return;
	take = *mw - MEMB_MIN;
	if (take > over) take = over;
	if (take > 0) { *mw -= take; over -= take; }
	take = *sw - SIDE_MIN;
	if (take > over) take = over;
	if (take > 0) *sw -= take;
}

uint16_t chan_id(struct ui *u)
{
	return whimsy_channel_id(u->w, u->g, u->chan);
}

int is_dm(struct ui *u, size_t g)
{
	char n[8];
	return whimsy_group_name(u->w, g, n, sizeof n) == 0 && whimsy_member_count(u->w, g) == 2;
}

/* the other member of a dm, or NULL */
const uint8_t *peer(struct ui *u, size_t g)
{
	for (size_t i = 0; i < whimsy_member_count(u->w, g); i++) {
		const uint8_t *m = whimsy_member(u->w, g, i);
		if (memcmp(m, u->self, WHIMSY_PK)) return m;
	}
	return NULL;
}

void row_title(struct ui *u, size_t g, char *out, size_t cap)
{
	const uint8_t *p;
	if (is_dm(u, g) && (p = peer(u, g))) whimsy_petname(u->w, p, out, cap);
	else whimsy_group_name(u->w, g, out, cap);
}

static double lum(const uint8_t rgb[3])
{
	return (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) / 255.0;
}

/* identity colour, pulled toward the far end of the background's lightness */
void idcol(struct ui *u, const uint8_t pk[WHIMSY_PK], uint8_t out[3])
{
	whimsy_colour(pk, out);
	if (lum(u->c->bg) > 0.5)
		for (int i = 0; i < 3; i++) out[i] = (uint8_t)(out[i] * 0.45);
}

/* unread messages in the channel that name us; the badge wears an '@' for them */
static size_t chan_mentions(struct ui *u, size_t g, uint16_t id)
{
	size_t n = 0;
	struct whimsy_msg m;
	for (size_t i = whimsy_first_unread(u->w, g, id); i < whimsy_msg_count(u->w, g); i++)
		if (!whimsy_msg(u->w, g, i, &m) && m.channel == id && m.mentions) n++;
	return n;
}

static size_t group_unread(struct ui *u, size_t g)
{
	size_t n = 0;
	for (size_t c = 0; c < whimsy_channel_count(u->w, g); c++)
		n += whimsy_unread(u->w, g, whimsy_channel_id(u->w, g, c));
	return n;
}

void hit(struct ui *u, float x, float y, float w, float h, int kind, size_t a, size_t b)
{
	if (u->nhit >= (int)(sizeof u->hit / sizeof *u->hit)) return;
	u->hit[u->nhit++] = (struct hit){x, y, w, h, kind, a, b};
}

void hit_top(struct ui *u, float x, float y, float w, float h, int kind, size_t a, size_t b)
{
	if (u->nhit >= (int)(sizeof u->hit / sizeof *u->hit)) return;
	memmove(u->hit + 1, u->hit, (size_t)u->nhit++ * sizeof *u->hit);
	u->hit[0] = (struct hit){x, y, w, h, kind, a, b};
}

/* ---- rows ---- */

/* the avatar for a key -- a pk, or a group id when grp -- decoded once and held. NULL
 * when there is none, when the bytes will not decode, or when `avatars` is off */
static SDL_Texture *avatar_of(struct ui *u, const uint8_t *key, int grp, int *iw, int *ih)
{
	size_t n = 0, kn = grp ? WHIMSY_GID : WHIMSY_PK;
	const uint8_t *src = NULL;
	int slot = -1, old = 0;

	if (u->c->avatars < 0.5) return NULL;
	if (grp) {
		for (size_t g = 0; g < whimsy_group_count(u->w) && !src; g++)
			if (!memcmp(whimsy_group_id(u->w, g), key, WHIMSY_GID))
				src = whimsy_group_avatar_get(u->w, g, &n);
	} else {
		src = whimsy_avatar_get(u->w, key, &n);
	}
	if (!src || !n) return NULL;

	for (int i = 0; i < AVATARS; i++) {
		if (u->av[i].t && u->av[i].grp == grp && !memcmp(u->av[i].key, key, kn)) {
			slot = i;
			break;
		}
		if (!u->av[i].t) { if (slot < 0) slot = i; continue; }
		if (u->av[i].used < u->av[old].used) old = i;
	}
	if (slot < 0) slot = old;
	if (u->av[slot].t && u->av[slot].src != src) {
		draw_image_free(u->av[slot].t);
		u->av[slot].t = NULL;
	}
	if (!u->av[slot].t) {
		SDL_Texture *t = draw_image(u->d, src, n, &u->av[slot].w, &u->av[slot].h);
		if (!t) return NULL;
		memset(u->av[slot].key, 0, WHIMSY_PK);
		memcpy(u->av[slot].key, key, kn);
		u->av[slot].grp = grp;
		u->av[slot].src = src;
		u->av[slot].t = t;
	}
	u->av[slot].used = ++u->avclock;
	*iw = u->av[slot].w;
	*ih = u->av[slot].h;
	return u->av[slot].t;
}

/* every circle in the ui: the avatar when one is held, else the name's initial on the
 * identity colour */
void circle(struct ui *u, float cx, float cy, float r, const uint8_t *key, int grp,
                   const char *name, const uint8_t col[3])
{
	int iw = 0, ih = 0;
	SDL_Texture *t = avatar_of(u, key, grp, &iw, &ih);
	if (t) { draw_image_circle(u->d, t, iw, ih, cx, cy, r); return; }
	draw_circle(u->d, cx, cy, r, col, 56);
	char ini[2] = {name && name[0] ? name[0] : '?', 0};
	float w = draw_measure(u->d, ini, 1, DRAW_REGULAR);
	draw_text(u->d, cx - w / 2, cy - draw_line_height(u->d) / 2, ini, 1, col, DRAW_REGULAR);
}

/* every row everywhere: circle, name, badge. text NULL draws no circle */
static void draw_row(struct ui *u, float x, float y, float w, const uint8_t *pk, int grp,
                     const char *name, const char *badge, int active, int indent)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), h = lh + 8;
	uint8_t col[3];
	memcpy(col, c->fg, 3);
	if (pk) idcol(u, pk, col);

	x += c->pad;                    /* the row box sits inside the pane's padding */
	w -= c->pad * 2;
	if (active == 1) {
		uint8_t soft[3] = {255, 255, 255};
		draw_rect(u->d, x, y, w, h, soft, 18, c->radius);
	}
	float tx = x + c->pad + (float)indent;
	if (pk) {
		float r = 11;
		circle(u, tx + r, y + h / 2, r, pk, grp, name, col);
		tx += r * 2 + c->gap;
	}
	float bw = badge ? draw_measure(u->d, badge, strlen(badge), DRAW_REGULAR) + c->gap : 0;
	draw_cut(u, tx, y + 4, name, strlen(name), x + w - c->pad - bw - tx,
	         active ? c->accent : col);
	if (badge) {
		bw -= c->gap;
		draw_text(u->d, x + w - c->pad - bw, y + 4, badge, strlen(badge), c->accent, DRAW_REGULAR);
	}
}

static void sidebar(struct ui *u, float x, float y, float w, float h)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), rh = lh + 8, y0 = y;
	size_t n = whimsy_group_count(u->w);

	for (int pass = 0; pass < 2; pass++) {
		const char *label = pass == 0 ? "direct" : "groups";
		int any = 0;
		for (size_t g = 0; g < n && y < y0 + h - rh; g++) {
			if (is_dm(u, g) != (pass == 0)) continue;
			if (!any++) {
				draw_text(u->d, x + c->pad * 2, y + 4, label, strlen(label), c->dim, DRAW_REGULAR);
				y += rh;
			}
			char name[128], badge[16];
			size_t un = group_unread(u, g), gm = 0;
			int muted = whimsy_muted(u->w, g);
			row_title(u, g, name, sizeof name);
			if (!name[0]) snprintf(name, sizeof name, "group");
			for (size_t ch = 0; ch < whimsy_channel_count(u->w, g); ch++)
				gm += chan_mentions(u, g, whimsy_channel_id(u->w, g, ch));
			snprintf(badge, sizeof badge, "%s%zu", gm ? "@" : "", un);
			if (muted) un = 0;      /* muted: not even how many are waiting */
			uint8_t key[WHIMSY_PK] = {0};   /* a group id is 16 bytes; the hue is the first one */
			memcpy(key, whimsy_group_id(u->w, g), WHIMSY_GID);
			draw_row(u, x, y, w, is_dm(u, g) ? peer(u, g) : key, !is_dm(u, g),
			         name, un ? badge : NULL, g == u->g, 0);
			hit(u, x, y, w, rh, H_GROUP, g, 0);
			y += rh;

			if (g != u->g) continue;
			for (size_t ch = 0; ch < whimsy_channel_count(u->w, g) && y < y0 + h - rh; ch++) {
				char cn[128], row[132];
				whimsy_channel_name(u->w, g, ch, cn, sizeof cn);
				snprintf(row, sizeof row, "#%s", cn);
				uint16_t cid = whimsy_channel_id(u->w, g, ch);
				size_t cu = whimsy_unread(u->w, g, cid);
				snprintf(badge, sizeof badge, "%s%zu", chan_mentions(u, g, cid) ? "@" : "", cu);
				if (muted) cu = 0;
				draw_row(u, x, y, w, NULL, 0, row, cu ? badge : NULL, ch == u->chan ? 2 : 0, 30);
				hit(u, x, y, w, rh, H_CHAN, g, ch);
				y += rh;
			}
		}
		if (any) y += c->gap;
	}
	if (y == y0) draw_text(u->d, x + c->pad * 2, y + 4, "no groups yet", 13, c->dim, DRAW_REGULAR);
}

static void members(struct ui *u, float x, float y, float w, float h)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), rh = lh + 8, y0 = y;
	char label[32];
	size_t n = whimsy_member_count(u->w, u->g);
	int ln = snprintf(label, sizeof label, "members %zu", n);
	draw_text(u->d, x + c->pad * 2, y + 4, label, (size_t)ln, c->dim, DRAW_REGULAR);
	if (u->memb_open) hit(u, x, y, w, rh, H_MEMB, 0, 0);   /* the label closes what a toggle opened */
	y += rh;
	for (size_t i = 0; i < n && y < y0 + h - rh; i++) {
		const uint8_t *pk = whimsy_member(u->w, u->g, i);
		char name[128];
		whimsy_petname(u->w, pk, name, sizeof name);
		int mine = !memcmp(pk, u->self, WHIMSY_PK);
		const char *badge = whimsy_verified(u->w, pk) ? "\xe2\x9c\x93" :
		                    (mine && whimsy_is_owner(u->w, u->g)) ? "owner" : NULL;
		draw_row(u, x, y, w, pk, 0, name, badge, 0, 0);
		hit(u, x, y, w, rh, H_PROF, i, 0);
		y += rh;
	}
}

/* ---- paint ---- */

void clip(struct ui *u, float x, float y, float w, float h, float scale)
{
	SDL_Rect r = {(int)(x * scale), (int)(y * scale), (int)(w * scale), (int)(h * scale)};
	SDL_SetRenderClipRect(u->r, &r);
}

void ui_paint(struct ui *u, float W, float H, float scale)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d);
	u->nhit = 0;
	u->scale = scale;

	if (u->cr_t) { crop_paint(u, W, H); return; }
	if (u->set_open) { settings(u, W, H); return; }

	if (!whimsy_group_count(u->w)) u->g = u->chan = 0;
	else if (u->g >= whimsy_group_count(u->w)) u->g = 0;

	int wide = W >= WIDE, mwide = W >= MEMB_AT;
	float sw, mw;
	u->win_w = W;
	panes(u, W, &sw, &mw);
	float mx = sw, mwid = W - sw - mw;
	if (sw) hit(u, sw - 3, 0, 6, H, H_SDRAG, 0, 0);
	if (mw) hit(u, W - mw - 3, 0, 6, H, H_MDRAG, 0, 0);

	/* header */
	float hh = lh + c->pad * 2;
	char name[128], cn[128];
	row_title(u, u->g, name, sizeof name);
	whimsy_channel_name(u->w, u->g, u->chan, cn, sizeof cn);
	float tx = mx + MIDPAD;
	if (!wide) {
		float bw = lh * 0.6f;
		for (int i = 0; i < 3; i++)
			draw_rule(u->d, tx, c->pad + lh / 2 + (float)(i - 1) * 4, bw, c->dim);
		tx += bw + c->gap * 2;
		hit(u, mx, 0, 40, hh, H_SIDE, 0, 0);
	}

	size_t q = whimsy_pending(u->w);
	int on = whimsy_online(u->w);
	const char *state = on ? "online" : u->neterr ? whimsy_strerror(u->neterr) : "offline";
	float rw = draw_measure(u->d, state, strlen(state), DRAW_REGULAR);
	char queued[32] = {0};
	if (q) {
		snprintf(queued, sizeof queued, " \xc2\xb7 %zu queued", q);
		rw += draw_measure(u->d, queued, strlen(queued), DRAW_REGULAR);
	}
	int show_state = mwide || !u->memb_open;
	float avail = mwid - MIDPAD - (tx - mx) - (show_state ? rw + c->gap * 2 : 0);

	if (cn[0]) {
		char lead[136], chan[132];
		int ln = snprintf(lead, sizeof lead, "%s / ", name);
		int chn = snprintf(chan, sizeof chan, "#%s", cn);
		float cw = draw_measure(u->d, chan, (size_t)chn, DRAW_REGULAR);
		if (cw > avail * 0.6f) cw = avail * 0.6f;   /* the channel keeps most of the room */
		tx = draw_cut(u, tx, c->pad, lead, (size_t)ln, avail - cw, c->dim);
		draw_cut(u, tx, c->pad, chan, (size_t)chn, mx + mwid - MIDPAD - tx -
		         (show_state ? rw + c->gap * 2 : 0), c->fg);
	} else {
		draw_cut(u, tx, c->pad, name, strlen(name), avail, c->fg);
	}

	if (show_state) {
		float sx = mx + mwid - MIDPAD - rw;
		sx = draw_text(u->d, sx, c->pad, state, strlen(state), on ? ok_col : bad_col, DRAW_REGULAR);
		if (q) draw_text(u->d, sx, c->pad, queued, strlen(queued), c->dim, DRAW_REGULAR);
		hit(u, mx + mwid - MIDPAD - rw, 0, rw, hh, H_MEMB, 0, 0);
	}
	draw_rule(u->d, mx, hh, mwid, c->line);

	/* composer, then the log gets what is left */
	clip(u, mx, hh, mwid, H - hh, scale);
	float cy = H - MIDPAD - composer(u, 0, 0, mwid - MIDPAD * 2, 0);
	composer(u, mx + MIDPAD, cy, mwid - MIDPAD * 2, 1);
	if (u->err[0]) {
		cy -= lh;
		draw_text(u->d, mx + MIDPAD, cy, u->err, strlen(u->err), c->accent, DRAW_REGULAR);
	}
	if (u->comp.n + 200 >= FIELD_MAX) {
		char left[32];
		int n = snprintf(left, sizeof left, "%zu", FIELD_MAX - 1 - u->comp.n);
		float lw = draw_measure(u->d, left, (size_t)n, DRAW_REGULAR);
		draw_text(u->d, mx + mwid - MIDPAD - lw, cy - lh, left, (size_t)n,
		          u->comp.n + 50 >= FIELD_MAX ? bad_col : c->dim, DRAW_REGULAR);
	}
	{
		uint8_t who[4 * WHIMSY_PK];
		size_t nt = whimsy_typers(u->w, u->g, chan_id(u), who, 4);
		if (nt) {
			char line[256];
			int at = 0;
			for (size_t i = 0; i < nt; i++) {
				char pn[WHIMSY_MAX_PET + 1];
				whimsy_petname(u->w, who + i * WHIMSY_PK, pn, sizeof pn);
				at += snprintf(line + at, sizeof line - (size_t)at, "%s%s",
				               i ? ", " : "", pn);
				if (at >= (int)sizeof line) { at = (int)sizeof line - 1; break; }
			}
			at += snprintf(line + at, sizeof line - (size_t)at, nt > 1 ? " are typing" : " is typing");
			if (at > (int)sizeof line - 1) at = (int)sizeof line - 1;
			cy -= lh;
			draw_text(u->d, mx + MIDPAD, cy, line, (size_t)at, c->dim, DRAW_REGULAR);
		}
	}
	cy -= popup(u, mx + MIDPAD, cy, mwid - MIDPAD * 2);
	float ch = H - cy + c->pad;

	float ly = hh + c->pad, lhgt = H - ch - ly;
	clip(u, mx, ly, mwid, lhgt > 0 ? lhgt : 0, scale);
	if (lhgt > 0) log_pane(u, mx, ly, mwid, lhgt);

	if (sw) {
		clip(u, 0, 0, sw, H, scale);
		sidebar(u, 0, c->pad, sw, H - c->pad);
		SDL_SetRenderClipRect(u->r, NULL);
		draw_rect(u->d, sw, 0, 1, H, c->line, 255, 0);
	}
	if (mw) {
		clip(u, W - mw, 0, mw, H, scale);
		members(u, W - mw, c->pad, mw, H - c->pad);
		SDL_SetRenderClipRect(u->r, NULL);
		draw_rect(u->d, W - mw, 0, 1, H, c->line, 255, 0);
	}
	SDL_SetRenderClipRect(u->r, NULL);

	if (u->over) file_overlay(u, W, H);
	if (u->card) profile_card(u, W, H);

	/* seen only moves with the channel showing, the window focused and the log at the bottom */
	if (u->scroll == 0 && whimsy_msg_count(u->w, u->g) &&
	    (SDL_GetWindowFlags(SDL_GetRenderWindow(u->r)) & SDL_WINDOW_INPUT_FOCUS))
		whimsy_seen(u->w, u->g, chan_id(u), whimsy_msg_count(u->w, u->g) - 1);
}

