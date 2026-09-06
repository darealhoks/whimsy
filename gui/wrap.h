#ifndef WHIMSY_WRAP_H
#define WHIMSY_WRAP_H

#include <stddef.h>

/* Line breaker, no SDL and no draw: measure is a callback so tests/test_wrap.c can
 * run it. Breaks on the last space that fits, on a hard newline, and mid-word only
 * when one word cannot fit alone. out gets the byte offset each line starts at,
 * plus a final entry at n; returns the line count, at most cap - 1. */

typedef float (*wrap_measure)(void *u, const char *s, size_t n);

static inline size_t wrap_next(const char *s, size_t n, size_t i)
{
	size_t j = i + 1;
	while (j < n && ((unsigned char)s[j] & 0xc0) == 0x80) j++;
	return j;
}

static inline int wrap_lines(const char *s, size_t n, float maxw,
                             wrap_measure measure, void *u, size_t *out, int cap)
{
	int nl = 0;
	float w = 0;
	size_t brk = 0;                 /* start of the run after the last space, 0 = none */
	if (cap < 2) return 0;
	out[0] = 0;
	for (size_t i = 0; i < n; ) {
		size_t j = wrap_next(s, n, i);
		if (s[i] == '\n') {
			if (nl + 2 >= cap) break;
			out[++nl] = j;
			w = 0;
			brk = 0;
			i = j;
			continue;
		}
		float cw = measure(u, s + i, j - i);
		/* a trailing space hangs past the edge rather than starting the next line */
		if (s[i] != ' ' && w + cw > maxw && i > out[nl]) {
			if (nl + 2 >= cap) break;
			size_t at = brk > out[nl] ? brk : i;
			out[++nl] = at;
			w = 0;
			brk = 0;
			i = at;
			continue;
		}
		w += cw;
		if (s[i] == ' ') brk = j;
		i = j;
	}
	out[nl + 1] = n;
	return nl + 1;
}

#endif
