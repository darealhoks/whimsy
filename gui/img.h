#ifndef WHIMSY_IMG_H
#define WHIMSY_IMG_H

#include <stddef.h>

/* decoded-image ceiling, shared by img_* and draw_image: attacker-controlled bytes
 * declare these dimensions and the decode allocates w*h*comp before anyone looks */
#define IMG_MAX_DIM 8192
#define IMG_MAX_PX  (1 << 24)

/* The stb single headers live here. Exactly one .c defines IMG_IMPLEMENTATION and
 * gets stbi_* / stbir_* / stbi_write_* along with it; that TU is gui/draw.c. */

/* Decode src, downscale and re-encode as jpeg until the result is <= cap bytes.
 * *out is malloc'd on success (0), untouched on failure (-1). */
int img_shrink(const void *src, size_t n, size_t cap, void **out, size_t *out_n);

/* the same, for an avatar: the square of src at sx, sy of side source px, scaled to
 * px by px, jpeg under cap. side <= 0 takes the centred square; a rect off the edge is
 * clamped back in */
int img_avatar_rect(const void *src, size_t n, float sx, float sy, float side,
                    int px, size_t cap, void **out, size_t *out_n);
int img_avatar(const void *src, size_t n, int px, size_t cap, void **out, size_t *out_n);

#endif

#ifdef IMG_IMPLEMENTATION
#ifndef IMG_IMPLEMENTED
#define IMG_IMPLEMENTED

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wconversion"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wcomma"
#pragma GCC diagnostic ignored "-Wextra-semi-stmt"
#pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#endif

/* stb's jpeg writer shifts signed ints past the sign bit; the dev build is ubsan */
#ifdef __clang__
#pragma clang attribute push(__attribute__((no_sanitize("undefined"))), apply_to = function)
#endif

#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS IMG_MAX_DIM
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef __clang__
#pragma clang attribute pop
#endif

#pragma GCC diagnostic pop

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct img_buf { unsigned char *p; size_t n, cap; int bad; };

static void img_sink(void *ctx, void *data, int len)
{
	struct img_buf *b = ctx;
	if (b->bad) return;
	if (b->n + (size_t)len > b->cap) {
		size_t want = (b->n + (size_t)len) * 2;
		unsigned char *q = realloc(b->p, want);
		if (!q) { b->bad = 1; return; }
		b->p = q; b->cap = want;
	}
	memcpy(b->p + b->n, data, (size_t)len);
	b->n += (size_t)len;
}

/* px rgb pixels out to jpeg, quality dropping until it fits. 1 on failure */
static int img_jpeg(unsigned char *px, int w, int h, size_t cap, void **out, size_t *out_n)
{
	for (int q = 90; q >= 20; q -= 10) {
		struct img_buf b = {0};
		if (!stbi_write_jpg_to_func(img_sink, &b, w, h, 3, px, q) || b.bad) {
			free(b.p);
			return 1;
		}
		if (b.n <= cap) { *out = b.p; *out_n = b.n; return 0; }
		free(b.p);
	}
	return 1;
}

int img_avatar_rect(const void *src, size_t n, float sx, float sy, float side,
                    int px, size_t cap, void **out, size_t *out_n)
{
	int w, h, comp;
	if (n > INT_MAX || px < 1) return -1;
	unsigned char *in = stbi_load_from_memory(src, (int)n, &w, &h, &comp, 3);
	if (!in) return -1;
	if ((long long)w * h > IMG_MAX_PX) { free(in); return -1; }

	int max = w < h ? w : h;
	int s = side > 0 ? (int)(side + 0.5f) : max, x, y;
	if (s > max) s = max;
	if (s < 1) s = 1;
	x = side > 0 ? (int)(sx + 0.5f) : (w - s) / 2;
	y = side > 0 ? (int)(sy + 0.5f) : (h - s) / 2;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + s > w) x = w - s;
	if (y + s > h) y = h - s;

	unsigned char *crop = in + ((size_t)y * (size_t)w + (size_t)x) * 3;
	unsigned char *small = malloc((size_t)px * (size_t)px * 3);
	if (!small || !stbir_resize_uint8_srgb(crop, s, s, w * 3, small, px, px, px * 3,
	                                       STBIR_RGB)) {
		free(small);
		free(in);
		return -1;
	}
	free(in);
	int e = img_jpeg(small, px, px, cap, out, out_n);
	free(small);
	return e ? -1 : 0;
}

int img_avatar(const void *src, size_t n, int px, size_t cap, void **out, size_t *out_n)
{
	return img_avatar_rect(src, n, 0, 0, 0, px, cap, out, out_n);
}

int img_shrink(const void *src, size_t n, size_t cap, void **out, size_t *out_n)
{
	int w, h, comp;
	if (n > INT_MAX) return -1;
	unsigned char *px = stbi_load_from_memory(src, (int)n, &w, &h, &comp, 3);
	if (!px) return -1;
	if ((long long)w * h > IMG_MAX_PX) { free(px); return -1; }

	int q = 85;
	for (int step = 0; step < 12; step++) {
		struct img_buf b = {0};
		if (!stbi_write_jpg_to_func(img_sink, &b, w, h, 3, px, q) || b.bad) {
			free(b.p); free(px); return -1;
		}
		if (b.n <= cap) {
			free(px);
			*out = b.p; *out_n = b.n;
			return 0;
		}
		free(b.p);
		if (q > 60) { q -= 15; continue; }
		int nw = w * 3 / 4, nh = h * 3 / 4;
		if (nw < 1 || nh < 1) break;
		unsigned char *small = stbir_resize_uint8_srgb(px, w, h, 0, NULL, nw, nh, 0, STBIR_RGB);
		if (!small) break;
		free(px);
		px = small; w = nw; h = nh;
	}
	free(px);
	return -1;
}

#endif
#endif
