#ifndef WHIMSY_REACT_H
#define WHIMSY_REACT_H

#include <string.h>

#include "whimsy.h"

/* Reactions come out of core one per sender; the pills under a row are one per
 * distinct emoji. Pure, so tests/test_react.c runs it. */

struct pill {
	const char *text;
	size_t text_n;
	int n;                          /* senders with this one */
	int mine;
};

/* first-arrival order, at most cap distinct emoji */
static inline int react_pills(const struct whimsy_react *r, size_t n,
                              const uint8_t self[WHIMSY_PK], struct pill *out, int cap)
{
	int np = 0;
	for (size_t i = 0; i < n; i++) {
		int k = 0;
		for (; k < np; k++)
			if (out[k].text_n == r[i].text_n && !memcmp(out[k].text, r[i].text, r[i].text_n))
				break;
		if (k == np) {
			if (np == cap) continue;
			out[np].text = r[i].text;
			out[np].text_n = r[i].text_n;
			out[np].n = 0;
			out[np].mine = 0;
			np++;
		}
		out[k].n++;
		if (!memcmp(r[i].sender, self, WHIMSY_PK)) out[k].mine = 1;
	}
	return np;
}

/* the i'th space separated token of s, NULL past the last; *n is its length */
static inline const char *react_nth(const char *s, int i, size_t *n)
{
	while (*s == ' ') s++;
	for (; i > 0 && *s; i--) {
		while (*s && *s != ' ') s++;
		while (*s == ' ') s++;
	}
	if (!*s) return NULL;
	const char *e = s;
	while (*e && *e != ' ') e++;
	*n = (size_t)(e - s);
	return s;
}

#endif
