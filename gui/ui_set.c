#include "ui_int.h"



/* ---- settings ---- */

enum { C_LOOK, C_DO, C_ACCT };
static const char *const SET_CAT[] = {"appearance", "behaviour", "account", NULL};

/* the conf keys the screen lists, in order. side_w, memb_w and the two open flags are
 * the gui's own scratch, not settings */
static const struct { const char *key, *desc; int kind, cat; } SET[] = {
	{"font",           "every glyph comes from this one face",          S_FONT,   C_LOOK},
	{"font_bold",      "empty draws bold as a second offset pass",      S_FONT,   C_LOOK},
	{"font_italic",    "empty shears the regular face instead",         S_FONT,   C_LOOK},
	{"font_emoji",     "colour emoji face, empty finds one",            S_FONT,   C_LOOK},
	{"size",           "point size before the display scale",           S_TEXT,   C_LOOK},
	{"line_height",    "row pitch, a multiple of the size",             S_TEXT,   C_LOOK},
	{"alpha",          "window opacity, the wallpaper shows through",   S_TEXT,   C_LOOK},
	{"bg",             "the window ground",                             S_COLOUR, C_LOOK},
	{"fg",             "message text",                                  S_COLOUR, C_LOOK},
	{"dim",            "chrome, times, everything secondary",           S_COLOUR, C_LOOK},
	{"accent",         "links, the active row, anything to click",      S_COLOUR, C_LOOK},
	{"line",           "rules and pane edges",                          S_COLOUR, C_LOOK},
	{"sel",            "the hover and selection band",                  S_COLOUR, C_LOOK},
	{"radius",         "corner radius",                                 S_TEXT,   C_LOOK},
	{"pad",            "inset inside a pane",                           S_TEXT,   C_LOOK},
	{"gap",            "space between rows",                            S_TEXT,   C_LOOK},
	{"renderer",       "auto | software",                               S_TEXT,   C_LOOK},
	{"reacts",         "the emoji the hover strip offers",              S_TEXT,   C_LOOK},
	{"confirm_delete", "ask before a delete goes through",              S_BOOL,   C_DO},
};
#define NSET ((int)(sizeof SET / sizeof *SET))

/* the two 0|1 entries in core's key table, core/whimsy.c */
/* the conf keys the toggle draws, alongside the two in core's key table */
static int store_bool(const char *k)
{
	return !strcmp(k, "typing") || !strcmp(k, "file_autosave");
}

/* rows past the table are the store keys, in whimsy_key_name order */
static const char *set_name(int i) { return i < NSET ? SET[i].key : whimsy_key_name(i - NSET); }
static int set_cat(int i) { return i < NSET ? SET[i].cat : C_ACCT; }
static const char *set_help(int i) { return i < NSET ? SET[i].desc : whimsy_key_help(i - NSET); }

int set_kind(int i)
{
	const char *n;
	if (i < NSET) return SET[i].kind;
	n = whimsy_key_name(i - NSET);
	return n && store_bool(n) ? S_BOOL : S_TEXT;
}

static void set_val(struct ui *u, int i, char *v, size_t cap)
{
	v[0] = 0;
	if (i < NSET) conf_getstr(u->c, SET[i].key, v, cap);
	else whimsy_get(u->w, i - NSET, v, cap);
}

int set_on(struct ui *u, int i)
{
	char v[WHIMSY_MAX_VAL + 1];
	set_val(u, i, v, sizeof v);
	return v[0] == '1';
}

/* ponytail: walks the font dirs on every keystroke, cache if it ever drags */
void set_suggest(struct ui *u)
{
	u->set_nfont = u->set_fsel = 0;
	if (u->set_row < 0 || set_kind(u->set_row) != S_FONT) return;
	u->set_nfont = draw_fonts(u->set_f.buf, u->set_font, SET_FONTS);
}

/* seed the in-place editor on row i */
void set_edit(struct ui *u, int i)
{
	char v[WHIMSY_MAX_VAL + 1];
	set_val(u, i, v, sizeof v);
	memset(&u->set_f, 0, sizeof u->set_f);
	field_insert(&u->set_f, v);
	u->set_row = i;
	set_suggest(u);
}


void set_apply(struct ui *u, const char *val)
{
	set_key(u, set_name(u->set_row), val);
	u->set_row = -1;
	u->set_nfont = 0;
}

/* the mockup's pill: accent when on, the knob rides to the other end */
static void toggle(struct ui *u, float x, float y, int on)
{
	struct conf *c = u->c;
	draw_rect(u->d, x, y, 26, 14, on ? c->accent : c->line, 255, 7);
	draw_circle(u->d, x + (on ? 19 : 7), y + 7, 5, on ? c->bg : c->dim, 255);
}

static float set_rowh(struct ui *u, int i, float lh)
{
	float h = lh + 8;
	if (i == u->set_row && u->set_nfont)
		h += (float)u->set_nfont * (lh + 4) + u->c->gap;
	return h;
}

/* the settings screen: it owns the window. categories on the left, the chosen
 * category's keys on the right, click a row to change it */
void settings(struct ui *u, float W, float H)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), rh = lh + 8;
	int nrow = 0;
	while (set_name(nrow)) nrow++;

	float bw = W - c->pad * 4;
	if (bw > SET_W) bw = SET_W;
	float x = (W - bw) / 2, y = c->pad * 2;
	float cw = bw > SET_CATS * 3 ? SET_CATS : bw / 3;   /* the left column, narrow windows share */
	float rx = x + cw, rw = bw - cw;

	const char *cat = SET_CAT[u->set_cat];
	float tx = draw_text(u->d, x + c->pad, y + 4, "settings", 8, c->dim, DRAW_REGULAR);
	tx = draw_text(u->d, tx, y + 4, " \xc2\xb7 ", 5, c->dim, DRAW_REGULAR);
	draw_text(u->d, tx, y + 4, cat, strlen(cat), c->fg, DRAW_REGULAR);
	float ew = draw_measure(u->d, "esc", 3, DRAW_REGULAR);
	draw_text(u->d, x + bw - c->pad - ew, y + 4, "esc", 3, c->dim, DRAW_REGULAR);
	y += rh;
	draw_rule(u->d, x, y, bw, c->line);
	y += c->pad;

	float cy = y;
	for (int i = 0; SET_CAT[i]; i++, cy += rh) {
		int on = i == u->set_cat;
		if (on) draw_rect(u->d, x, cy, cw - c->gap, rh, c->sel, 40, c->radius);
		draw_cut(u, x + c->pad, cy + 4, SET_CAT[i], strlen(SET_CAT[i]),
		         cw - c->pad * 2, on ? c->accent : c->fg);
		hit(u, x, cy, cw - c->gap, rh, H_SCAT, (size_t)i, 0);
	}
	draw_cut(u, x + c->pad, cy + c->pad, c->path, strlen(c->path), cw - c->pad * 2, c->dim);

	float top = y, bot = H - c->pad * 2;
	if (u->err[0]) {
		bot -= lh + c->gap;
		draw_cut(u, rx + c->pad, bot, u->err, strlen(u->err), rw - c->pad * 2, c->accent);
	}
	float content = 0;
	for (int i = 0; i < nrow; i++)
		if (set_cat(i) == u->set_cat) content += set_rowh(u, i, lh);
	float over = content - (bot - top);
	if (u->set_scroll > over) u->set_scroll = over > 0 ? over : 0;
	if (u->set_scroll < 0) u->set_scroll = 0;
	y -= u->set_scroll;

	float kx = rx + c->pad, vx = rx + c->pad + SET_KEY;
	clip(u, rx, top, rw, bot - top, u->scale);
	for (int i = 0; i < nrow; i++) {
		const char *k = set_name(i), *help = set_help(i);
		int kind = set_kind(i);
		if (set_cat(i) != u->set_cat) continue;
		float h = set_rowh(u, i, lh);
		if (y + h < top || y > bot) { y += h; continue; }
		if (u->my >= y && u->my < y + rh && u->mx >= rx && u->mx < rx + rw)
			draw_rect(u->d, rx, y, rw, rh, c->sel, 40, c->radius);
		draw_cut(u, kx, y + 4, k, strlen(k), SET_KEY - c->gap * 2, c->dim);
		hit(u, rx, y, rw, rh, H_SROW, (size_t)i, 0);

		float nx = vx;
		char v[WHIMSY_MAX_VAL + 1];
		set_val(u, i, v, sizeof v);
		if (kind == S_COLOUR) {
			uint8_t col[3] = {0};
			conf_colour(col, v);
			/* while editing, the swatch follows the field, keeping the last colour
			 * that parsed */
			if (i == u->set_row) conf_colour(col, u->set_f.buf);
			draw_rect(u->d, vx, y + 4 + (lh - 12) / 2, 16, 12, col, 255, 3);
			draw_rect(u->d, vx, y + 4 + (lh - 12) / 2, 16, 12, c->line, 90, 3);
			nx = vx + 16 + c->gap;
		}
		if (kind == S_BOOL) {
			toggle(u, nx, y + 4 + (lh - 14) / 2, v[0] == '1');
			nx += 26;
		} else if (i == u->set_row) {
			float cx = nx + draw_measure(u->d, u->set_f.buf, u->set_f.cur, DRAW_REGULAR);
			nx = draw_cut(u, nx, y + 4, u->set_f.buf, u->set_f.n,
			              rx + rw - c->pad - nx, c->accent);
			if ((SDL_GetTicks() / 500) % 2 == 0)
				draw_rect(u->d, cx, y + 4, 2, lh, c->fg, 255, 0);
		} else {
			nx = draw_cut(u, nx, y + 4, v[0] ? v : "unset", v[0] ? strlen(v) : 5,
			              rx + rw - c->pad - nx, v[0] ? c->fg : c->dim);
		}
		if (help)
			draw_cut(u, nx + c->gap * 2, y + 4, help, strlen(help),
			         rx + rw - c->pad - (nx + c->gap * 2), c->dim);

		/* the font suggestions, under the field they are filtering */
		if (i == u->set_row) {
			float sy = y + rh;
			for (int f = 0; f < u->set_nfont; f++, sy += lh + 4) {
				const char *nm = u->set_font[f];
				if (f == u->set_fsel)
					draw_rect(u->d, vx - c->gap, sy, rw - (vx - rx), lh + 4,
					          c->sel, 40, c->radius);
				draw_cut(u, vx, sy + 2, nm, strlen(nm), rx + rw - c->pad - vx,
				         f == u->set_fsel ? c->accent : c->fg);
				hit(u, vx - c->gap, sy, rw - (vx - rx), lh + 4, H_SFONT, (size_t)f, 0);
			}
		}
		y += h;
	}
	SDL_SetRenderClipRect(u->r, NULL);
}

