#ifndef WHIMSY_FIELD_H
#define WHIMSY_FIELD_H

#include <SDL3/SDL.h>

#include <stddef.h>
#include <string.h>

/* A text buffer with a caret and one selection anchor. The unlock screens use it as
 * one line (secret = 1 stars it out); the composer is the same thing with newlines. */

#define FIELD_MAX 8192          /* WHIMSY_MAX_TEXT, one message */

struct field {
	char buf[FIELD_MAX];
	size_t n, cur, anc;
	int secret;
};

static inline void field_sel(const struct field *f, size_t *from, size_t *to)
{
	*from = f->cur < f->anc ? f->cur : f->anc;
	*to   = f->cur < f->anc ? f->anc : f->cur;
}

static inline void field_erase(struct field *f, size_t from, size_t to)
{
	if (from >= to) return;
	memmove(f->buf + from, f->buf + to, f->n - to);
	f->n -= to - from;
	f->cur = f->anc = from;
	f->buf[f->n] = 0;
}

static inline void field_cut_sel(struct field *f)
{
	size_t a, b;
	field_sel(f, &a, &b);
	field_erase(f, a, b);
}

static inline void field_insert(struct field *f, const char *s)
{
	field_cut_sel(f);
	size_t n = strlen(s);
	if (f->n + n >= sizeof f->buf) return;
	memmove(f->buf + f->cur + n, f->buf + f->cur, f->n - f->cur);
	memcpy(f->buf + f->cur, s, n);
	f->n += n;
	f->cur = f->anc = f->cur + n;
	f->buf[f->n] = 0;
}

/* a step never lands inside a utf-8 sequence */
static inline size_t field_back(const struct field *f)
{
	size_t i = f->cur;
	while (i && ((unsigned char)f->buf[i - 1] & 0xc0) == 0x80) i--;
	return i ? i - 1 : 0;
}

static inline size_t field_fwd(const struct field *f)
{
	size_t i = f->cur + 1;
	while (i < f->n && ((unsigned char)f->buf[i] & 0xc0) == 0x80) i++;
	return i > f->n ? f->n : i;
}

static inline void field_copy(const struct field *f)
{
	size_t a, b;
	field_sel(f, &a, &b);
	if (a == b || f->secret) return;
	char *t = SDL_malloc(b - a + 1);
	if (!t) return;
	memcpy(t, f->buf + a, b - a);
	t[b - a] = 0;
	SDL_SetClipboardText(t);
	SDL_free(t);
}

/* one_line strips newlines out of a paste; 1 for every field but the composer */
static inline void field_paste(struct field *f, int one_line)
{
	char *t = SDL_GetClipboardText();
	if (!t) return;
	for (char *p = t; *p; p++)
		if (*p == '\r' || *p == '\t' || (one_line && *p == '\n')) *p = ' ';
	field_insert(f, t);
	SDL_free(t);
}

/* 1 when the key was the field's. one_line as in field_paste */
static inline int field_key(struct field *f, SDL_Keycode k, SDL_Keymod mod, int one_line)
{
	int ctrl = (mod & SDL_KMOD_CTRL) != 0, shift = (mod & SDL_KMOD_SHIFT) != 0;
	if (ctrl) {
		switch (k) {
		case SDLK_V: field_paste(f, one_line); return 1;
		case SDLK_C: field_copy(f); return 1;
		case SDLK_X: field_copy(f); field_cut_sel(f); return 1;
		case SDLK_A: f->anc = 0; f->cur = f->n; return 1;
		case SDLK_U: field_erase(f, 0, f->n); return 1;
		}
		return 0;
	}
	size_t cur = f->cur;
	switch (k) {
	case SDLK_BACKSPACE:
		if (f->cur != f->anc) field_cut_sel(f);
		else field_erase(f, field_back(f), f->cur);
		return 1;
	case SDLK_DELETE:
		if (f->cur != f->anc) field_cut_sel(f);
		else field_erase(f, f->cur, field_fwd(f));
		return 1;
	case SDLK_LEFT:  cur = field_back(f); break;
	case SDLK_RIGHT: cur = field_fwd(f); break;
	case SDLK_HOME:  cur = 0; break;
	case SDLK_END:   cur = f->n; break;
	default: return 0;
	}
	f->cur = cur;
	if (!shift) f->anc = cur;
	return 1;
}

#endif
