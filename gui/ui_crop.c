#include "ui_int.h"

#include <stdio.h>
#include <stdlib.h>

#include "img.h"

/* ---- the avatar crop screen ---- */

#define CR_TEX  512             /* the circle's render target, px */
#define CR_MAXZ 4.0f            /* the slider's far end */

/* logical px per source px, for a circle of radius 1: the shorter side fills the
 * circle at zoom 1. offsets are kept in radii, so no geometry survives a frame */
static float cr_scale(struct ui *u)
{
	int m = u->cr_w < u->cr_h ? u->cr_w : u->cr_h;
	return m > 0 ? 2 * u->cr_zoom / (float)m : 0;
}

/* the picture may never leave a gap inside the circle */
static void cr_clamp(struct ui *u)
{
	float s = cr_scale(u);
	float mx = (float)u->cr_w * s / 2 - 1, my = (float)u->cr_h * s / 2 - 1;
	if (mx < 0) mx = 0;
	if (my < 0) my = 0;
	if (u->cr_ox > mx) u->cr_ox = mx;
	if (u->cr_ox < -mx) u->cr_ox = -mx;
	if (u->cr_oy > my) u->cr_oy = my;
	if (u->cr_oy < -my) u->cr_oy = -my;
}

void crop_close(struct ui *u)
{
	if (u->cr_t) draw_image_free(u->cr_t);
	if (u->cr_dst) draw_image_free(u->cr_dst);
	u->cr_t = u->cr_dst = NULL;
	u->cr_drag = 0;
}

/* the picked file becomes the screen; a file that will not decode stays an error */
void crop_open(struct ui *u, const char *path, int grp)
{
	size_t n;
	void *b = SDL_LoadFile(path, &n);
	int w, h;
	SDL_Texture *t;
	if (!b) { ui_err(u, "%s", SDL_GetError()); return; }
	t = draw_image(u->d, b, n, &w, &h);
	SDL_free(b);
	if (!t) { ui_err(u, "that is not a picture i can read"); return; }
	crop_close(u);
	u->cr_t = t;
	u->cr_w = w;
	u->cr_h = h;
	u->cr_grp = grp;
	u->cr_zoom = 1;
	u->cr_ox = u->cr_oy = 0;
	snprintf(u->cr_path, sizeof u->cr_path, "%s", path);
}

/* the source square the circle is showing, re-encoded at AVATAR_PX and handed to core */
static void cr_confirm(struct ui *u)
{
	float s = cr_scale(u), side = s > 0 ? 2 / s : 0;
	void *src, *out;
	size_t n, out_n;
	int e;

	if (side < 1) { crop_close(u); return; }
	if (!(src = SDL_LoadFile(u->cr_path, &n))) { ui_err(u, "%s", SDL_GetError()); return; }
	e = img_avatar_rect(src, n, (float)u->cr_w / 2 - u->cr_ox / s - side / 2,
	                    (float)u->cr_h / 2 - u->cr_oy / s - side / 2, side,
	                    AVATAR_PX, WHIMSY_MAX_AVATAR, &out, &out_n);
	SDL_free(src);
	if (e) { ui_err(u, "that image will not re-encode"); return; }
	e = u->cr_grp ? whimsy_group_avatar_set(u->w, u->g, out, out_n)
	              : whimsy_avatar_set(u->w, out, out_n);
	free(out);
	crop_close(u);
	done(u, e);
}

/* the picture panned and zoomed into a square target, which the circle fan then
 * samples: SDL has no round clip */
static void cr_face(struct ui *u, float cx, float cy, float r)
{
	if (!u->cr_dst)
		u->cr_dst = SDL_CreateTexture(u->r, SDL_PIXELFORMAT_RGBA32,
		                              SDL_TEXTUREACCESS_TARGET, CR_TEX, CR_TEX);
	if (!u->cr_dst) return;
	SDL_SetTextureBlendMode(u->cr_dst, SDL_BLENDMODE_BLEND);
	float s = cr_scale(u) * (CR_TEX / 2.0f);        /* target px per source px */
	SDL_FRect d = {CR_TEX / 2.0f + u->cr_ox * (CR_TEX / 2.0f) - (float)u->cr_w * s / 2,
	               CR_TEX / 2.0f + u->cr_oy * (CR_TEX / 2.0f) - (float)u->cr_h * s / 2,
	               (float)u->cr_w * s, (float)u->cr_h * s};
	SDL_SetRenderTarget(u->r, u->cr_dst);
	SDL_SetRenderDrawBlendMode(u->r, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(u->r, 0, 0, 0, 0);
	SDL_RenderClear(u->r);
	SDL_SetTextureBlendMode(u->cr_t, SDL_BLENDMODE_NONE);
	SDL_RenderTexture(u->r, u->cr_t, NULL, &d);
	SDL_SetRenderTarget(u->r, NULL);
	SDL_SetRenderDrawBlendMode(u->r, SDL_BLENDMODE_BLEND);
	draw_image_circle(u->d, u->cr_dst, CR_TEX, CR_TEX, cx, cy, r);
}

static void button(struct ui *u, float x, float y, float w, float h, const char *s, int fill)
{
	struct conf *c = u->c;
	draw_rect(u->d, x, y, w, h, fill ? c->accent : c->line, fill ? 255 : 120, c->radius);
	float tw = draw_measure(u->d, s, strlen(s), DRAW_REGULAR);
	draw_text(u->d, x + (w - tw) / 2, y + (h - draw_line_height(u->d)) / 2, s, strlen(s),
	          fill ? c->bg : c->fg, DRAW_REGULAR);
}

void crop_paint(struct ui *u, float W, float H)
{
	struct conf *c = u->c;
	float lh = draw_line_height(u->d), bh = lh + c->pad * 2, bw = 110;
	float r = (W < H ? W : H) * 0.22f;
	if (r > 140) r = 140;
	if (r < 50) r = 50;

	float cx = W / 2, cy = H / 2 - bh * 2;
	const char *title = u->cr_grp ? "group picture" : "your picture";
	float tw = draw_measure(u->d, title, strlen(title), DRAW_REGULAR);
	draw_text(u->d, cx - tw / 2, cy - r - lh * 2, title, strlen(title), c->dim, DRAW_REGULAR);

	cr_clamp(u);
	draw_circle(u->d, cx, cy, r + 1, c->line, 90);
	cr_face(u, cx, cy, r);
	hit(u, cx - r, cy - r, r * 2, r * 2, H_CDRAG, 0, 0);

	/* the zoom slider */
	u->cr_sw = r * 2;
	u->cr_sx = cx - r;
	float sy = cy + r + bh;
	draw_rect(u->d, u->cr_sx, sy + 5, u->cr_sw, 4, c->line, 160, 2);
	draw_circle(u->d, u->cr_sx + u->cr_sw * (u->cr_zoom - 1) / (CR_MAXZ - 1), sy + 7, 7,
	            c->accent, 255);
	hit(u, u->cr_sx - 8, sy - 6, u->cr_sw + 16, 26, H_CZDRAG, 0, 0);

	float by = sy + bh, bx = cx - bw - c->gap / 2;
	button(u, bx, by, bw, bh, "confirm", 1);
	hit(u, bx, by, bw, bh, H_COK, 0, 0);
	bx = cx + c->gap / 2;
	button(u, bx, by, bw, bh, "cancel", 0);
	hit(u, bx, by, bw, bh, H_CCANCEL, 0, 0);

	if (u->err[0]) {
		float ew = draw_measure(u->d, u->err, strlen(u->err), DRAW_REGULAR);
		draw_text(u->d, cx - ew / 2, by + bh + c->gap, u->err, strlen(u->err),
		          c->accent, DRAW_REGULAR);
	}
	u->cr_r = r;
}

static void cr_slide(struct ui *u, float mx)
{
	float t = u->cr_sw > 0 ? (mx - u->cr_sx) / u->cr_sw : 0;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	u->cr_zoom = 1 + t * (CR_MAXZ - 1);
	cr_clamp(u);
}

/* the crop screen owns every event while it stands */
int crop_event(struct ui *u, const SDL_Event *e, float scale)
{
	float k = SDL_GetWindowPixelDensity(SDL_GetRenderWindow(u->r)) / scale;

	switch (e->type) {
	case SDL_EVENT_MOUSE_BUTTON_DOWN: {
		float mx = e->button.x * k, my = e->button.y * k;
		u->err[0] = 0;
		for (int i = 0; i < u->nhit; i++) {
			struct hit *h = &u->hit[i];
			if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
			if (h->kind == H_COK) { cr_confirm(u); return 1; }
			if (h->kind == H_CCANCEL) { crop_close(u); return 1; }
			if (h->kind == H_CZDRAG) { u->cr_drag = 2; cr_slide(u, mx); return 1; }
			if (h->kind == H_CDRAG) { u->cr_drag = 1; return 1; }
		}
		return 1;
	}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		u->cr_drag = 0;
		return 1;
	case SDL_EVENT_MOUSE_MOTION: {
		float mx = e->motion.x * k;
		if (u->cr_drag == 2) { cr_slide(u, mx); return 1; }
		if (u->cr_drag != 1 || u->cr_r <= 0) return 0;
		u->cr_ox += e->motion.xrel * k / u->cr_r;
		u->cr_oy += e->motion.yrel * k / u->cr_r;
		cr_clamp(u);
		return 1;
	}
	case SDL_EVENT_MOUSE_WHEEL:
		u->cr_zoom += e->wheel.y * 0.1f;
		if (u->cr_zoom < 1) u->cr_zoom = 1;
		if (u->cr_zoom > CR_MAXZ) u->cr_zoom = CR_MAXZ;
		cr_clamp(u);
		return 1;
	case SDL_EVENT_KEY_DOWN:
		u->err[0] = 0;
		if (e->key.key == SDLK_ESCAPE) { crop_close(u); return 1; }
		if (e->key.key == SDLK_RETURN || e->key.key == SDLK_KP_ENTER) {
			cr_confirm(u);
			return 1;
		}
		return 1;
	}
	return 0;
}
