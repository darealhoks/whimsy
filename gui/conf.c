#include "conf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char DEFAULT_FILE[] =
"# whimsy. one key per line, `key = value`. lines starting with # are ignored.\n"
"\n"
"# font file, or a family name looked up under the font dirs. empty picks a mono face.\n"
"font =\n"
"# left empty, bold is a second draw offset 1px and italic is a shear\n"
"font_bold =\n"
"font_italic =\n"
"# colour emoji face. empty finds NotoColorEmoji or another emoji font under the font dirs\n"
"font_emoji =\n"
"\n"
"size = 14\n"
"line_height = 1.4\n"
"# window opacity. the compositor's wallpaper is the picture behind it\n"
"alpha = 0.84\n"
"\n"
"bg = #070b14\n"
"fg = #dce4f0\n"
"# labels, timestamps, everything secondary\n"
"dim = #6d7b92\n"
"# the one accent: active row, caret, buttons\n"
"accent = #7fa3d4\n"
"# the only border there is\n"
"line = #26314a\n"
"# the band under the selected message\n"
"sel = #24405e\n"
"\n"
"radius = 8\n"
"pad = 8\n"
"gap = 6\n"
"\n"
"# the 8 emoji the hover strip offers\n"
"reacts = \xf0\x9f\x91\x8d \xe2\x9d\xa4 \xf0\x9f\x98\x82 \xf0\x9f\x8e\x89 \xf0\x9f\x98\xae \xf0\x9f\x98\xa2 \xf0\x9f\x99\x8f \xf0\x9f\x94\xa5\n"
"\n"
"# ask before deleting a message; the confirm's `a` answer clears this\n"
"confirm_delete = 1\n"
"\n"
"# auto | software\n"
"renderer = auto\n"
"\n"
"# show avatars, and send yours to the keys you have verified\n"
"avatars = 1\n"
"\n"
"# quit | hide. hide keeps polling with no window; `whimsy` raises it, `whimsy quit` ends it\n"
"close = quit\n"
"# run for every message that arrives. %g group, %c channel, %s sender, %t text,\n"
"# %a the cached avatar path. text only reaches it when you put %t here\n"
"notify = notify-send \"%g #%c\"\n"
"\n"
"# pane layout, rewritten by the gui when you drag a splitter or toggle a pane\n"
"side_w = 220\n"
"memb_w = 240\n"
"side_open = 0\n"
"memb_open = 1\n";

static void defaults(struct conf *c)
{
	memset(c, 0, sizeof *c);
	c->size = 14; c->line_height = 1.4; c->alpha = 0.84;
	c->radius = 8; c->pad = 8; c->gap = 6;
	c->side_w = 220; c->memb_w = 240;
	c->confirm_delete = 1; c->memb_open = 1;
	memcpy(c->bg,     (uint8_t[]){0x07, 0x0b, 0x14}, 3);
	memcpy(c->fg,     (uint8_t[]){0xdc, 0xe4, 0xf0}, 3);
	memcpy(c->dim,    (uint8_t[]){0x6d, 0x7b, 0x92}, 3);
	memcpy(c->accent, (uint8_t[]){0x7f, 0xa3, 0xd4}, 3);
	memcpy(c->line,   (uint8_t[]){0x26, 0x31, 0x4a}, 3);
	memcpy(c->sel,    (uint8_t[]){0x24, 0x40, 0x5e}, 3);
	snprintf(c->renderer, sizeof c->renderer, "auto");
	snprintf(c->close, sizeof c->close, "quit");
	snprintf(c->notify, sizeof c->notify, "notify-send \"%%g #%%c\"");
	c->avatars = 1;
	snprintf(c->reacts, sizeof c->reacts, "\xf0\x9f\x91\x8d \xe2\x9d\xa4 \xf0\x9f\x98\x82 \xf0\x9f\x8e\x89 \xf0\x9f\x98\xae \xf0\x9f\x98\xa2 \xf0\x9f\x99\x8f \xf0\x9f\x94\xa5");
}

static int hexv(int ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

void conf_colour(uint8_t out[3], const char *v)
{
	if (*v == '#') v++;
	if (strlen(v) != 6) return;
	uint8_t t[3];
	for (int i = 0; i < 3; i++) {
		int hi = hexv(v[2 * i]), lo = hexv(v[2 * i + 1]);
		if (hi < 0 || lo < 0) return;
		t[i] = (uint8_t)(hi * 16 + lo);
	}
	memcpy(out, t, 3);
}

static void num(double *out, const char *v, double lo, double hi)
{
	char *end;
	double d = strtod(v, &end);
	if (end == v || d < lo || d > hi) return;
	*out = d;
}

static void str(char *out, size_t cap, const char *v)
{
	snprintf(out, cap, "%s", v);
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
	return s;
}

static void apply(struct conf *c, char *key, char *val)
{
	if      (!strcmp(key, "font"))        str(c->font, sizeof c->font, val);
	else if (!strcmp(key, "font_bold"))   str(c->font_bold, sizeof c->font_bold, val);
	else if (!strcmp(key, "font_italic")) str(c->font_italic, sizeof c->font_italic, val);
	else if (!strcmp(key, "font_emoji"))  str(c->font_emoji, sizeof c->font_emoji, val);
	else if (!strcmp(key, "size"))        num(&c->size, val, 6, 72);
	else if (!strcmp(key, "line_height")) num(&c->line_height, val, 1, 3);
	else if (!strcmp(key, "alpha"))       num(&c->alpha, val, 0.1, 1);
	else if (!strcmp(key, "radius"))      num(&c->radius, val, 0, 32);
	else if (!strcmp(key, "pad"))         num(&c->pad, val, 0, 64);
	else if (!strcmp(key, "gap"))         num(&c->gap, val, 0, 64);
	else if (!strcmp(key, "bg"))          conf_colour(c->bg, val);
	else if (!strcmp(key, "fg"))          conf_colour(c->fg, val);
	else if (!strcmp(key, "dim"))         conf_colour(c->dim, val);
	else if (!strcmp(key, "accent"))      conf_colour(c->accent, val);
	else if (!strcmp(key, "line"))        conf_colour(c->line, val);
	else if (!strcmp(key, "sel"))         conf_colour(c->sel, val);
	else if (!strcmp(key, "renderer"))    str(c->renderer, sizeof c->renderer, val);
	else if (!strcmp(key, "notify"))      str(c->notify, sizeof c->notify, val);
	else if (!strcmp(key, "avatars"))     num(&c->avatars, val, 0, 1);
	/* only on a value it knows, so conf_set's accepts() rejects anything else */
	else if (!strcmp(key, "close") && (!strcmp(val, "quit") || !strcmp(val, "hide")))
		str(c->close, sizeof c->close, val);
	else if (!strcmp(key, "reacts"))      str(c->reacts, sizeof c->reacts, val);
	else if (!strcmp(key, "confirm_delete")) num(&c->confirm_delete, val, 0, 1);
	else if (!strcmp(key, "side_w"))      num(&c->side_w, val, 0, 4096);
	else if (!strcmp(key, "memb_w"))      num(&c->memb_w, val, 0, 4096);
	else if (!strcmp(key, "side_open"))   num(&c->side_open, val, 0, 1);
	else if (!strcmp(key, "memb_open"))   num(&c->memb_open, val, 0, 1);
}

/* fgets split an over-long line; without this its tail parses as a fresh key = value.
   copies the rest through when out is given */
static int overlong(FILE *in, const char *ln, size_t cap, FILE *out)
{
	if (strlen(ln) != cap - 1 || strchr(ln, '\n')) return 0;
	int ch;
	if (out) fputs(ln, out);
	while ((ch = fgetc(in)) != EOF && ch != '\n')
		if (out) fputc(ch, out);
	if (out && ch == '\n') fputc('\n', out);
	return 1;
}

static void parse(struct conf *c)
{
	FILE *f = fopen(c->path, "r");
	if (!f) return;
	char ln[1024];
	while (fgets(ln, sizeof ln, f)) {
		if (overlong(f, ln, sizeof ln, NULL)) continue;
		char *s = trim(ln);
		if (!*s || *s == '#') continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		apply(c, trim(s), trim(eq + 1));
	}
	fclose(f);

	struct stat st;
	if (!stat(c->path, &st)) c->mtime = st.st_mtime;
}

static void mkparents(const char *path)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *p = buf + 1; *p; p++)
		if (*p == '/') { *p = 0; mkdir(buf, 0700); *p = '/'; }
}

void conf_load(struct conf *c, const char *path)
{
	char keep[512];
	snprintf(keep, sizeof keep, "%s", path);
	defaults(c);
	snprintf(c->path, sizeof c->path, "%s", keep);

	if (access(c->path, F_OK)) {
		mkparents(c->path);
		FILE *f = fopen(c->path, "w");
		if (f) { fputs(DEFAULT_FILE, f); fclose(f); }
	}
	parse(c);
}

int conf_reload(struct conf *c)
{
	struct stat st;
	if (stat(c->path, &st) || st.st_mtime == c->mtime) return 0;
	char keep[512];
	snprintf(keep, sizeof keep, "%s", c->path);
	defaults(c);
	snprintf(c->path, sizeof c->path, "%s", keep);
	parse(c);
	return 1;
}

const char *const conf_keys[] = {
	"font", "font_bold", "font_italic", "font_emoji", "size", "line_height", "alpha",
	"bg", "fg", "dim", "accent", "line", "sel", "radius", "pad", "gap", "renderer", "reacts",
	"avatars", "confirm_delete", "close", "notify",
	"side_w", "memb_w", "side_open", "memb_open", NULL
};

static int iskey(const char *key)
{
	for (int i = 0; conf_keys[i]; i++) if (!strcmp(conf_keys[i], key)) return 1;
	return 0;
}

size_t conf_getstr(const struct conf *c, const char *key, char *out, size_t cap)
{
	const uint8_t *col = NULL;
	const double *n = NULL;
	const char *s = NULL;

	if      (!strcmp(key, "font"))        s = c->font;
	else if (!strcmp(key, "font_bold"))   s = c->font_bold;
	else if (!strcmp(key, "font_italic")) s = c->font_italic;
	else if (!strcmp(key, "font_emoji"))  s = c->font_emoji;
	else if (!strcmp(key, "renderer"))    s = c->renderer;
	else if (!strcmp(key, "close"))       s = c->close;
	else if (!strcmp(key, "notify"))      s = c->notify;
	else if (!strcmp(key, "avatars"))     n = &c->avatars;
	else if (!strcmp(key, "reacts"))      s = c->reacts;
	else if (!strcmp(key, "size"))        n = &c->size;
	else if (!strcmp(key, "line_height")) n = &c->line_height;
	else if (!strcmp(key, "alpha"))       n = &c->alpha;
	else if (!strcmp(key, "radius"))      n = &c->radius;
	else if (!strcmp(key, "pad"))         n = &c->pad;
	else if (!strcmp(key, "gap"))         n = &c->gap;
	else if (!strcmp(key, "confirm_delete")) n = &c->confirm_delete;
	else if (!strcmp(key, "side_w"))      n = &c->side_w;
	else if (!strcmp(key, "memb_w"))      n = &c->memb_w;
	else if (!strcmp(key, "side_open"))   n = &c->side_open;
	else if (!strcmp(key, "memb_open"))   n = &c->memb_open;
	else if (!strcmp(key, "bg"))          col = c->bg;
	else if (!strcmp(key, "fg"))          col = c->fg;
	else if (!strcmp(key, "dim"))         col = c->dim;
	else if (!strcmp(key, "accent"))      col = c->accent;
	else if (!strcmp(key, "line"))        col = c->line;
	else if (!strcmp(key, "sel"))         col = c->sel;
	else return 0;

	if (s)   return (size_t)snprintf(out, cap, "%s", s);
	if (n)   return (size_t)snprintf(out, cap, "%g", *n);
	return (size_t)snprintf(out, cap, "#%02x%02x%02x", col[0], col[1], col[2]);
}

/* apply() only writes on a value it accepts, so a poisoned scratch tells validity apart
   from a no-op */
static int accepts(const char *key, const char *value)
{
	struct conf a, b;
	char k[64], v[256];
	memset(&a, 0xaa, sizeof a);
	memcpy(&b, &a, sizeof b);
	snprintf(k, sizeof k, "%s", key);
	snprintf(v, sizeof v, "%s", value);
	apply(&a, k, v);
	return memcmp(&a, &b, sizeof a) != 0;
}

int conf_set(struct conf *c, const char *key, const char *value)
{
	char k[64], v[256];
	/* the file gets exactly what memory gets: no truncation split, no injected line */
	if (snprintf(v, sizeof v, "%s", value) >= (int)sizeof v) return 0;
	for (const unsigned char *p = (const unsigned char *)v; *p; p++)
		if (*p < 0x20 || *p == 0x7f) return 0;
	if (!iskey(key) || !accepts(key, v)) return 0;

	char tmp[520];
	snprintf(tmp, sizeof tmp, "%s.tmp", c->path);
	FILE *in = fopen(c->path, "r");
	FILE *out = fopen(tmp, "w");
	if (!out) { if (in) fclose(in); return 0; }

	char ln[1024], scratch[1024];
	int done = 0;
	while (in && fgets(ln, sizeof ln, in)) {
		if (overlong(in, ln, sizeof ln, out)) continue;
		snprintf(scratch, sizeof scratch, "%s", ln);
		char *s = trim(scratch), *eq = strchr(s, '=');
		if (!done && *s && *s != '#' && eq) {
			*eq = 0;
			if (!strcmp(trim(s), key)) {
				fprintf(out, "%s = %s\n", key, v);
				done = 1;
				continue;
			}
		}
		fputs(ln, out);
	}
	if (in) fclose(in);
	if (!done) fprintf(out, "%s = %s\n", key, v);
	if (fclose(out) || rename(tmp, c->path)) { unlink(tmp); return 0; }

	snprintf(k, sizeof k, "%s", key);
	apply(c, k, v);
	struct stat st;
	if (!stat(c->path, &st)) c->mtime = st.st_mtime;
	return 1;
}
