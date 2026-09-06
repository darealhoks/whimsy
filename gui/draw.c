#include "draw.h"

#define IMG_IMPLEMENTATION
#include "img.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ATLAS 1024
#define SLOTS 1024              /* open-addressed, power of two, never grows */

struct glyph {
	uint32_t key;               /* cp << 2 | style, +1 so 0 is empty */
	float u, v, w, h;           /* atlas px */
	float bx, by, adv;          /* bearing and advance, px */
	int colour;                 /* rgba in the atlas: draw it unmodulated */
};

struct draw {
	SDL_Renderer *r;
	FT_Library ft;
	FT_Face face[3];            /* regular, bold, italic; bold/italic may be NULL */
	FT_Face emoji;              /* fallback for what face[] has no glyph for, may be NULL */
	int emoji_px;               /* the strike FT_Select_Size picked, 0 if scalable */
	int px;
	SDL_Texture *atlas;
	int pen_x, pen_y, row_h;
	struct glyph slot[SLOTS];
	float scale, lineh, ascent;
};

static size_t step(const char *s, size_t n, uint32_t *cp)
{
	unsigned char c = (unsigned char)s[0];
	size_t need = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 :
	              (c & 0xf8) == 0xf0 ? 4 : 0;
	if (!need || need > n) { *cp = 0xfffd; return 1; }
	uint32_t v = need == 1 ? c : c & (0x7f >> need);
	for (size_t i = 1; i < need; i++) {
		if (((unsigned char)s[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; }
		v = v << 6 | ((unsigned char)s[i] & 0x3f);
	}
	*cp = v;
	return need;
}

/* the font dirs, most specific first */
static const char *const FONT_DIRS[] = {
	"~/.local/share/fonts", "~/.fonts",
	"/usr/local/share/fonts", "/usr/share/fonts", NULL
};
static const char *const FALLBACK[] = {
	"MapleMono-NF-Regular", "MapleMono-Regular", "DejaVuSansMono",
	"LiberationMono-Regular", "NotoSansMono-Regular", "mono", NULL
};
static const char *const EMOJI[] = {
	"NotoColorEmoji", "TwemojiMozilla", "Twitter Color Emoji", "OpenMoji",
	"EmojiOne", "Symbola", "NotoEmoji-Regular", NULL
};

static int ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
		if (i == n) return 1;
	}
	return 0;
}

static int scan(const char *dir, const char *name, char *out, size_t cap, int depth)
{
	if (depth > 6) return 0;
	DIR *d = opendir(dir);
	if (!d) return 0;
	int found = 0;
	struct dirent *e;
	while (!found && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		const char *ext = strrchr(e->d_name, '.');
		if (ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf"))) {
			if (ci_has(e->d_name, name)) { snprintf(out, cap, "%s", p); found = 1; }
		} else if (!ext || strchr(e->d_name, '.') == NULL) {
			found = scan(p, name, out, cap, depth + 1);
		}
	}
	closedir(d);
	return found;
}

/* FONT_DIRS[i] with a leading ~ expanded */
static void font_dir(int i, char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (FONT_DIRS[i][0] == '~')
		snprintf(out, cap, "%s%s", home ? home : "", FONT_DIRS[i] + 1);
	else
		snprintf(out, cap, "%s", FONT_DIRS[i]);
}

static int find_font(const char *name, char *out, size_t cap)
{
	if (strchr(name, '/')) { snprintf(out, cap, "%s", name); return !access(out, R_OK); }
	for (int i = 0; FONT_DIRS[i]; i++) {
		char dir[512];
		font_dir(i, dir, sizeof dir);
		if (scan(dir, name, out, cap, 0)) return 1;
	}
	return 0;
}

/* scan, but collecting every match's basename instead of stopping at the first */
static int collect(const char *dir, const char *needle, char out[][DRAW_FONT_MAX],
                   int max, int n, int depth)
{
	if (depth > 6 || n >= max) return n;
	DIR *d = opendir(dir);
	if (!d) return n;
	struct dirent *e;
	while (n < max && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		const char *ext = strrchr(e->d_name, '.');
		if (!ext) { n = collect(p, needle, out, max, n, depth + 1); continue; }
		if (strcasecmp(ext, ".ttf") && strcasecmp(ext, ".otf")) continue;
		if (!ci_has(e->d_name, needle)) continue;
		size_t len = (size_t)(ext - e->d_name);
		if (len >= DRAW_FONT_MAX) len = DRAW_FONT_MAX - 1;
		int dup = 0;
		for (int i = 0; i < n; i++)
			if (!strncmp(out[i], e->d_name, len) && !out[i][len]) dup = 1;
		if (dup) continue;
		memcpy(out[n], e->d_name, len);
		out[n++][len] = 0;
	}
	closedir(d);
	return n;
}

int draw_fonts(const char *needle, char out[][DRAW_FONT_MAX], int max)
{
	int n = 0;
	for (int i = 0; FONT_DIRS[i] && n < max; i++) {
		char dir[512];
		font_dir(i, dir, sizeof dir);
		n = collect(dir, needle, out, max, n, 0);
	}
	return n;
}

static FT_Face load(struct draw *d, const char *name, int px)
{
	char path[1024];
	if (!*name) return NULL;
	if (!find_font(name, path, sizeof path)) return NULL;
	FT_Face f = NULL;
	if (FT_New_Face(d->ft, path, 0, &f)) return NULL;
	if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px) && f->num_fixed_sizes <= 0) {
		FT_Done_Face(f);
		return NULL;
	}
	return f;
}

static void drop_atlas(struct draw *d)
{
	if (d->atlas) SDL_DestroyTexture(d->atlas);
	d->atlas = NULL;
	memset(d->slot, 0, sizeof d->slot);
	d->pen_x = d->pen_y = d->row_h = 0;
}

static void drop_faces(struct draw *d)
{
	if (d->emoji) FT_Done_Face(d->emoji);
	d->emoji = NULL;
	for (int i = 0; i < 3; i++) {
		if (d->face[i]) FT_Done_Face(d->face[i]);
		d->face[i] = NULL;
	}
}

/* everything new is built first; the live faces and atlas only go once it all came up */
int draw_reload(struct draw *d, const struct conf *c, float scale)
{
	int px = (int)lround(c->size * scale);
	if (px < 6) px = 6;

	FT_Face nf[3] = {NULL, NULL, NULL};
	nf[DRAW_REGULAR] = load(d, c->font, px);
	for (int i = 0; FALLBACK[i] && !nf[DRAW_REGULAR]; i++)
		nf[DRAW_REGULAR] = load(d, FALLBACK[i], px);
	if (!nf[DRAW_REGULAR]) return -1;
	nf[DRAW_BOLD] = load(d, c->font_bold, px);
	nf[DRAW_ITALIC] = load(d, c->font_italic, px);
	FT_Face ne = load(d, c->font_emoji, px);
	for (int i = 0; EMOJI[i] && !ne && !*c->font_emoji; i++) ne = load(d, EMOJI[i], px);

	SDL_Texture *at = SDL_CreateTexture(d->r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
	                                    ATLAS, ATLAS);
	if (!at) {
		for (int i = 0; i < 3; i++) if (nf[i]) FT_Done_Face(nf[i]);
		if (ne) FT_Done_Face(ne);
		return -1;
	}
	SDL_SetTextureBlendMode(at, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(at, SDL_SCALEMODE_NEAREST);

	drop_atlas(d);
	drop_faces(d);
	memcpy(d->face, nf, sizeof nf);
	d->emoji = ne;
	d->emoji_px = 0;
	d->px = px;
	if (ne && ne->num_fixed_sizes > 0) {
		int best = 0;
		for (int i = 1; i < ne->num_fixed_sizes; i++)
			if (abs(ne->available_sizes[i].height - px) <
			    abs(ne->available_sizes[best].height - px)) best = i;
		if (!FT_Select_Size(ne, best)) d->emoji_px = ne->available_sizes[best].height;
	}
	d->atlas = at;
	d->scale = scale;
	d->lineh = (float)(c->size * c->line_height);
	d->ascent = (float)(nf[DRAW_REGULAR]->size->metrics.ascender >> 6) / scale;
	return 0;
}

int draw_open(struct draw **out, SDL_Renderer *r, const struct conf *c, float scale)
{
	struct draw *d = calloc(1, sizeof *d);
	if (!d) return -1;
	d->r = r;
	if (FT_Init_FreeType(&d->ft)) { free(d); return -1; }
	if (draw_reload(d, c, scale)) { FT_Done_FreeType(d->ft); free(d); return -1; }
	*out = d;
	return 0;
}

void draw_close(struct draw *d)
{
	if (!d) return;
	drop_atlas(d);
	drop_faces(d);
	FT_Done_FreeType(d->ft);
	free(d);
}

float draw_line_height(const struct draw *d) { return d->lineh; }
float draw_ascent(const struct draw *d) { return d->ascent; }

/* the coverage box for w by h px of rgba at pen, or -1 if the atlas is full.
 * it never evicts: a full one stops caching, glyphs already in it keep working */
static int place(struct draw *d, int w, int h)
{
	if (w > ATLAS) return -1;
	if (d->pen_x + w + 1 > ATLAS) {
		d->pen_x = 0;
		d->pen_y += d->row_h + 1;
		d->row_h = 0;
	}
	return d->pen_y + h + 1 > ATLAS ? -1 : 0;
}

static void took(struct draw *d, int w, int h)
{
	d->pen_x += w + 1;
	if (h > d->row_h) d->row_h = h;
}

/* a colour bitmap glyph: freetype hands over premultiplied bgra at a fixed strike
 * size, so it is resized to the text size and un-premultiplied for BLENDMODE_BLEND */
static struct glyph *rasterize_colour(struct draw *d, uint32_t cp, int style, struct glyph *g)
{
	FT_Face f = d->emoji;
	if (FT_Load_Char(f, cp, FT_LOAD_COLOR | FT_LOAD_RENDER)) return NULL;
	FT_Bitmap *b = &f->glyph->bitmap;
	if (b->pixel_mode != FT_PIXEL_MODE_BGRA || !b->width || !b->rows) return NULL;

	/* strike size to text size; the metrics scale by the same factor */
	float k = d->emoji_px ? (float)d->px / (float)d->emoji_px : 1;
	int w = (int)lroundf((float)b->width * k), h = (int)lroundf((float)b->rows * k);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (place(d, w, h)) return NULL;

	unsigned char *src = malloc((size_t)b->width * b->rows * 4);
	unsigned char *dst = malloc((size_t)w * h * 4);
	if (!src || !dst) { free(src); free(dst); return NULL; }
	for (unsigned y = 0; y < b->rows; y++)
		for (unsigned x = 0; x < b->width; x++) {
			const unsigned char *p = b->buffer + y * (size_t)b->pitch + x * 4;
			unsigned char *q = src + (y * (size_t)b->width + x) * 4;
			q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = p[3];
		}
	if (!stbir_resize_uint8_srgb(src, (int)b->width, (int)b->rows, 0, dst, w, h, 0,
	                             STBIR_RGBA_PM)) {
		free(src); free(dst);
		return NULL;
	}
	for (int i = 0; i < w * h; i++) {
		unsigned a = dst[i * 4 + 3];
		if (!a) continue;
		for (int c = 0; c < 3; c++) {
			unsigned v = dst[i * 4 + c] * 255u / a;
			dst[i * 4 + c] = (unsigned char)(v > 255 ? 255 : v);
		}
	}
	SDL_UpdateTexture(d->atlas, &(SDL_Rect){d->pen_x, d->pen_y, w, h}, dst, w * 4);
	free(src);
	free(dst);

	g->key = (cp << 2 | (uint32_t)style) + 1;
	g->u = (float)d->pen_x; g->v = (float)d->pen_y;
	g->w = (float)w; g->h = (float)h;
	g->bx = (float)f->glyph->bitmap_left * k;
	g->by = (float)f->glyph->bitmap_top * k;
	g->adv = (float)(f->glyph->advance.x >> 6) * k;
	g->colour = 1;
	took(d, w, h);
	return g;
}

static struct glyph *rasterize(struct draw *d, uint32_t cp, int style, struct glyph *g)
{
	FT_Face f = d->face[style] ? d->face[style] : d->face[DRAW_REGULAR];
	int synth = !d->face[style];
	if (!FT_Get_Char_Index(f, cp) && d->emoji && FT_Get_Char_Index(d->emoji, cp)) {
		struct glyph *c = rasterize_colour(d, cp, style, g);
		if (c) return c;
		f = d->emoji;
		synth = 0;
	}
	if (FT_Load_Char(f, cp, FT_LOAD_DEFAULT)) return NULL;
	if (synth && style == DRAW_BOLD) FT_GlyphSlot_Embolden(f->glyph);
	if (synth && style == DRAW_ITALIC) FT_GlyphSlot_Oblique(f->glyph);
	if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL)) return NULL;

	FT_Bitmap *b = &f->glyph->bitmap;
	if (b->pixel_mode != FT_PIXEL_MODE_GRAY && b->width && b->rows) return NULL;
	if (place(d, (int)b->width, (int)b->rows)) return NULL;

	if (b->width && b->rows) {
		uint32_t *px = malloc((size_t)b->width * b->rows * 4);
		if (!px) return NULL;
		for (unsigned y = 0; y < b->rows; y++)
			for (unsigned x = 0; x < b->width; x++)
				px[y * b->width + x] = 0x00ffffffu | ((uint32_t)b->buffer[y * (unsigned)b->pitch + x] << 24);
		SDL_UpdateTexture(d->atlas, &(SDL_Rect){d->pen_x, d->pen_y, (int)b->width, (int)b->rows},
		                  px, (int)b->width * 4);
		free(px);
	}

	g->key = (cp << 2 | (uint32_t)style) + 1;
	g->u = (float)d->pen_x; g->v = (float)d->pen_y;
	g->w = (float)b->width; g->h = (float)b->rows;
	g->bx = (float)f->glyph->bitmap_left;
	g->by = (float)f->glyph->bitmap_top;
	g->adv = (float)(f->glyph->advance.x >> 6);

	took(d, (int)b->width, (int)b->rows);
	return g;
}

static struct glyph *glyph(struct draw *d, uint32_t cp, int style)
{
	uint32_t key = (cp << 2 | (uint32_t)style) + 1;
	size_t i = (key * 2654435761u) & (SLOTS - 1);
	for (size_t n = 0; n < SLOTS; n++, i = (i + 1) & (SLOTS - 1)) {
		if (d->slot[i].key == key) return &d->slot[i];
		if (!d->slot[i].key) return rasterize(d, cp, style, &d->slot[i]);
	}
	return NULL;
}

static float run(struct draw *d, float x, float y, const char *s, size_t n,
                 const uint8_t rgb[3], int style, int paint)
{
	if (paint) SDL_SetTextureAlphaMod(d->atlas, 255);
	int modded = -1;
	float px = x * d->scale, base = (y + d->ascent) * d->scale;
	for (size_t i = 0; i < n; ) {
		uint32_t cp;
		i += step(s + i, n - i, &cp);
		struct glyph *g = glyph(d, cp, style);
		if (!g) continue;
		if (paint && g->colour != modded) {
			modded = g->colour;
			if (modded) SDL_SetTextureColorMod(d->atlas, 255, 255, 255);
			else SDL_SetTextureColorMod(d->atlas, rgb[0], rgb[1], rgb[2]);
		}
		if (paint && g->w && g->h)
			SDL_RenderTexture(d->r, d->atlas, &(SDL_FRect){g->u, g->v, g->w, g->h},
			                  &(SDL_FRect){roundf(px + g->bx), roundf(base - g->by), g->w, g->h});
		px += g->adv;
	}
	return px / d->scale;
}

/* offset from a baseline box's top that puts one glyph's ink centre on it, logical px */
float draw_ink_mid(struct draw *d, uint32_t cp, int style)
{
	struct glyph *g = glyph(d, cp, style);
	if (!g || !g->h) return d->ascent;
	return d->ascent - (g->by - g->h / 2) / d->scale;
}

float draw_measure(struct draw *d, const char *s, size_t n, int style)
{
	return run(d, 0, 0, s, n, NULL, style, 0);
}

float draw_wrap(void *d, const char *s, size_t n)
{
	return draw_measure(d, s, n, DRAW_REGULAR);
}

float draw_text(struct draw *d, float x, float y, const char *s, size_t n,
                const uint8_t rgb[3], int style)
{
	return run(d, x, y, s, n, rgb, style, 1);
}

static void colour(struct draw *d, const uint8_t rgb[3], uint8_t a)
{
	SDL_SetRenderDrawBlendMode(d->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(d->r, rgb[0], rgb[1], rgb[2], a);
}

/* every hand-drawn shape is one convex outline of points with an outward unit
 * normal each: the fill is that outline pulled in half a pixel, the edge a
 * one-pixel strip fading to alpha 0. all coordinates here are device px */
struct pt { float x, y, nx, ny; };

#define ARC_MAX 24              /* steps per quarter turn */
#define PT_MAX  (4 * ARC_MAX)

/* enough steps that the chord never sags more than a third of a pixel */
static int arcn(float r)
{
	int n = (int)ceilf((float)(M_PI / 2) / (2.0f * acosf(1.0f - 0.33f / (r > 0.5f ? r : 0.5f))));
	if (n < 2) n = 2;
	if (n > ARC_MAX) n = ARC_MAX;
	return n;
}

static int arc(struct pt *p, float cx, float cy, float r, float a0, int steps)
{
	for (int i = 0; i < steps; i++) {
		float a = a0 + (float)i * (float)(M_PI / 2) / (float)(steps - 1);
		float nx = cosf(a), ny = sinf(a);
		p[i] = (struct pt){cx + nx * r, cy + ny * r, nx, ny};
	}
	return steps;
}

static void shape(struct draw *d, const struct pt *p, int n, const uint8_t rgb[3], uint8_t a)
{
	SDL_FColor c = {rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f, a / 255.f};
	SDL_FColor c0 = c;
	c0.a = 0;
	SDL_Vertex v[1 + 2 * PT_MAX];
	int idx[3 * (PT_MAX + 2 * PT_MAX)], k = 0, m = 0;
	float ox = 0, oy = 0;
	for (int i = 0; i < n; i++) { ox += p[i].x; oy += p[i].y; }
	v[m++] = (SDL_Vertex){{ox / n, oy / n}, c, {0, 0}};
	for (int i = 0; i < n; i++) {
		v[m++] = (SDL_Vertex){{p[i].x - p[i].nx * 0.5f, p[i].y - p[i].ny * 0.5f}, c, {0, 0}};
		v[m++] = (SDL_Vertex){{p[i].x + p[i].nx * 0.5f, p[i].y + p[i].ny * 0.5f}, c0, {0, 0}};
	}
	for (int i = 0; i < n; i++) {
		int a1 = 1 + 2 * i, b1 = 1 + 2 * ((i + 1) % n);
		idx[k++] = 0;  idx[k++] = a1;     idx[k++] = b1;
		idx[k++] = a1; idx[k++] = a1 + 1; idx[k++] = b1 + 1;
		idx[k++] = a1; idx[k++] = b1 + 1; idx[k++] = b1;
	}
	SDL_SetRenderDrawBlendMode(d->r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(d->r, NULL, v, m, idx, k);
}

void draw_rect2(struct draw *d, float x, float y, float w, float h,
                const uint8_t rgb[3], uint8_t a, float left, float right)
{
	x *= d->scale; y *= d->scale; w *= d->scale; h *= d->scale;
	float rl = left * d->scale, rr = right * d->scale;
	float cap = w / 2 < h / 2 ? w / 2 : h / 2;
	if (rl > cap) rl = cap;
	if (rr > cap) rr = cap;
	if (rl < 0.5f && rr < 0.5f) {
		colour(d, rgb, a);
		SDL_RenderFillRect(d->r, &(SDL_FRect){x, y, w, h});
		return;
	}
	int sl = arcn(rl), sr = arcn(rr);
	struct pt p[PT_MAX];
	int n = 0;
	n += arc(p + n, x + w - rr, y + h - rr, rr, 0, sr);
	n += arc(p + n, x + rl,     y + h - rl, rl, (float)M_PI / 2, sl);
	n += arc(p + n, x + rl,     y + rl,     rl, (float)M_PI, sl);
	n += arc(p + n, x + w - rr, y + rr,     rr, (float)(3 * M_PI / 2), sr);
	shape(d, p, n, rgb, a);
}

void draw_rect(struct draw *d, float x, float y, float w, float h,
               const uint8_t rgb[3], uint8_t a, float radius)
{
	draw_rect2(d, x, y, w, h, rgb, a, radius, radius);
}

void draw_rule(struct draw *d, float x, float y, float w, const uint8_t rgb[3])
{
	float t = roundf(d->scale);
	if (t < 1) t = 1;
	colour(d, rgb, 255);
	SDL_RenderFillRect(d->r, &(SDL_FRect){x * d->scale, roundf(y * d->scale), w * d->scale, t});
}

void draw_circle(struct draw *d, float cx, float cy, float r, const uint8_t rgb[3], uint8_t a)
{
	cx *= d->scale; cy *= d->scale; r *= d->scale;
	int s = arcn(r);
	struct pt p[PT_MAX];
	int n = 0;
	for (int q = 0; q < 4; q++)
		n += arc(p + n, cx, cy, r, (float)q * (float)(M_PI / 2), s);
	shape(d, p, n, rgb, a);
}

SDL_Texture *draw_image(struct draw *d, const void *bytes, size_t n, int *w, int *h)
{
	int iw, ih, comp;
	if (n > INT_MAX) return NULL;
	unsigned char *px = stbi_load_from_memory(bytes, (int)n, &iw, &ih, &comp, 4);
	if (!px) return NULL;
	if ((long long)iw * ih > IMG_MAX_PX) { stbi_image_free(px); return NULL; }
	SDL_Surface *s = SDL_CreateSurfaceFrom(iw, ih, SDL_PIXELFORMAT_RGBA32, px, iw * 4);
	SDL_Texture *t = s ? SDL_CreateTextureFromSurface(d->r, s) : NULL;
	SDL_DestroySurface(s);
	stbi_image_free(px);
	if (!t) return NULL;
	SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
	if (w) *w = iw;
	if (h) *h = ih;
	return t;
}

void draw_image_circle(struct draw *d, SDL_Texture *t, int iw, int ih,
                       float cx, float cy, float r)
{
	cx *= d->scale; cy *= d->scale; r *= d->scale;
	int s = arcn(r), n = 0;
	struct pt p[PT_MAX];
	for (int q = 0; q < 4; q++)
		n += arc(p + n, cx, cy, r, (float)q * (float)(M_PI / 2), s);

	float uw = 1, vh = 1;
	if (iw > ih && iw > 0) uw = (float)ih / (float)iw;
	else if (ih > iw && ih > 0) vh = (float)iw / (float)ih;
	float u0 = (1 - uw) / 2, v0 = (1 - vh) / 2;

	SDL_FColor white = {1, 1, 1, 1};
	SDL_Vertex v[1 + PT_MAX];
	int idx[3 * PT_MAX], k = 0;
	v[0] = (SDL_Vertex){{cx, cy}, white, {u0 + uw / 2, v0 + vh / 2}};
	for (int i = 0; i < n; i++)
		v[i + 1] = (SDL_Vertex){{p[i].x, p[i].y}, white,
		                        {u0 + uw * (p[i].nx + 1) / 2, v0 + vh * (p[i].ny + 1) / 2}};
	for (int i = 0; i < n; i++) {
		idx[k++] = 0;
		idx[k++] = 1 + i;
		idx[k++] = 1 + (i + 1) % n;
	}
	SDL_SetRenderDrawBlendMode(d->r, SDL_BLENDMODE_BLEND);
	SDL_RenderGeometry(d->r, t, v, n + 1, idx, k);
}

void draw_image_free(SDL_Texture *t)
{
	SDL_DestroyTexture(t);
}

void draw_image_at(struct draw *d, SDL_Texture *t, float x, float y, float w, float h)
{
	SDL_RenderTexture(d->r, t, NULL,
	                  &(SDL_FRect){x * d->scale, y * d->scale, w * d->scale, h * d->scale});
}
