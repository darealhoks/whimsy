#ifndef WHIMSY_DRAW_H
#define WHIMSY_DRAW_H

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>

#include "conf.h"

/* One font at one size, one glyph atlas per scale. Every coordinate handed in is
 * logical px; draw multiplies by the display scale. */

#define DRAW_REGULAR 0
#define DRAW_BOLD    1
#define DRAW_ITALIC  2
#define DRAW_H1      3          /* bold, 1.5x the text size */
#define DRAW_H2      4          /* bold, 1.25x */
#define DRAW_FONT_MAX 64        /* a font basename in draw_fonts */

struct draw;

int  draw_open(struct draw **out, SDL_Renderer *r, const struct conf *c, float scale);
void draw_close(struct draw *d);
/* after a conf reload or a scale change; drops the atlas */
int  draw_reload(struct draw *d, const struct conf *c, float scale);

/* font file basenames under the font dirs whose name contains needle, deduped.
 * walks the dirs on every call */
int  draw_fonts(const char *needle, char out[][DRAW_FONT_MAX], int max);

/* the size multiplier of a style, for line heights: 1 unless a header */
float draw_style_scale(int style);
float draw_line_height(const struct draw *d);   /* logical px */
float draw_ascent(const struct draw *d);
/* logical px of s (n bytes, utf-8), nothing drawn */
float draw_ink_mid(struct draw *d, uint32_t cp, int style);
float draw_measure(struct draw *d, const char *s, size_t n, int style);
/* 1 when the text face carries cp, for icon-or-word fallbacks */
int   draw_has(struct draw *d, uint32_t cp);
/* wrap_measure over draw_measure at DRAW_REGULAR, for wrap.h */
float draw_wrap(void *d, const char *s, size_t n);
/* x, y is the left of the baseline box's top. returns the x after the run */
float draw_text(struct draw *d, float x, float y, const char *s, size_t n,
                const uint8_t rgb[3], int style);

void draw_rect2(struct draw *d, float x, float y, float w, float h,
                const uint8_t rgb[3], uint8_t a, float left, float right);
void draw_rect(struct draw *d, float x, float y, float w, float h,
               const uint8_t rgb[3], uint8_t a, float radius);
void draw_rect_over(struct draw *d, float x, float y, float w, float h,
                    const uint8_t rgb[3], uint8_t a, float radius);
void draw_rule(struct draw *d, float x, float y, float w, const uint8_t rgb[3]);
void draw_circle(struct draw *d, float cx, float cy, float r, const uint8_t rgb[3], uint8_t a);

/* png/jpg/gif/bmp bytes -> texture, NULL if they do not decode. w, h are the
 * decoded pixel size, may be NULL. free with draw_image_free. */
SDL_Texture *draw_image(struct draw *d, const void *bytes, size_t n, int *w, int *h);
void draw_image_free(SDL_Texture *t);
/* logical px rect, scaled like every other coordinate */
void draw_image_at(struct draw *d, SDL_Texture *t, float x, float y, float w, float h);
/* the same fan draw_circle uses, textured: the centred square of the source, so a
 * picture that is not square crops rather than squashes. iw, ih are draw_image's */
void draw_image_circle(struct draw *d, SDL_Texture *t, int iw, int ih,
                       float cx, float cy, float r);

#endif
