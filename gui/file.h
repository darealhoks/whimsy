#ifndef WHIMSY_FILE_H
#define WHIMSY_FILE_H

#include <stddef.h>
#include <stdio.h>

/* The pure arithmetic behind the file rows, so tests/test_file.c can run it without SDL. */

/* iw x ih scaled to fit maxw, never enlarged */
static inline void file_fit(int iw, int ih, float maxw, float *ow, float *oh)
{
	float w = (float)iw, h = (float)ih;
	if (iw <= 0 || ih <= 0 || maxw <= 0) { *ow = *oh = 0; return; }
	if (w > maxw) { h = h * maxw / w; w = maxw; }
	*ow = w;
	*oh = h;
}

/* the first max newline separated lines of s; off[k]/len[k] exclude the newline */
static inline int file_lines(const char *s, size_t n, size_t *off, size_t *len, int max)
{
	int k = 0;
	size_t b = 0;
	for (size_t i = 0; i <= n && k < max; i++) {
		if (i == n) {
			if (i > b || !k) { off[k] = b; len[k++] = i - b; }
			break;
		}
		if (s[i] != '\n') continue;
		off[k] = b;
		len[k++] = i - b;
		b = i + 1;
	}
	return k;
}

static inline void file_human(size_t n, char *out, size_t cap)
{
	if (n < 1000) snprintf(out, cap, "%zu B", n);
	else if (n < 1000000) snprintf(out, cap, "%.0f kB", (double)n / 1000.0);
	else if (n < 1000000000) snprintf(out, cap, "%.4g MB", (double)n / 1000000.0);
	else snprintf(out, cap, "%.3g GB", (double)n / 1000000000.0);
}

#endif
