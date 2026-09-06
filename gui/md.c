#include "md.h"

#include <string.h>

struct emit {
	struct md_run *out;
	int n, cap;
};

static void push(struct emit *e, size_t at, size_t end, uint16_t style)
{
	if (end <= at || e->n >= e->cap) return;
	if (e->n && e->out[e->n - 1].style == style &&
	    e->out[e->n - 1].at + e->out[e->n - 1].n == at) {
		e->out[e->n - 1].n = end - e->out[e->n - 1].at;
		return;
	}
	e->out[e->n].at = at;
	e->out[e->n].n = end - at;
	e->out[e->n].style = style;
	e->n++;
}

static const struct {
	const char *mark;
	size_t len;
	uint16_t style;
} delim[] = {
	{"**", 2, MD_BOLD}, {"~~", 2, MD_STRIKE}, {"||", 2, MD_SPOILER},
	{"`", 1, MD_CODE}, {"*", 1, MD_ITALIC}
};

static size_t closer(const char *s, size_t i, size_t e, const char *mark, size_t len)
{
	for (size_t j = i; j + len <= e; j++)
		if (!memcmp(s + j, mark, len)) return j;
	return e;
}

static int url_at(const char *s, size_t i, size_t e)
{
	size_t n = e - i;
	return (n >= 7 && !memcmp(s + i, "http://", 7)) ||
	       (n >= 8 && !memcmp(s + i, "https://", 8));
}

/* one line's content; a delimiter pair never crosses the line end */
static void inl(struct emit *em, const char *s, size_t i, size_t e, uint16_t base)
{
	size_t plain = i;
	while (i < e) {
		if (url_at(s, i, e) && (i == plain || s[i - 1] == ' ')) {
			size_t j = i;
			while (j < e && s[j] != ' ') j++;
			while (j > i && s[j - 1] && strchr(".,;:!?)", s[j - 1])) j--;
			push(em, plain, i, base);
			push(em, i, j, base | MD_LINK);
			plain = i = j;
			continue;
		}
		size_t k = 0;
		for (; k < sizeof delim / sizeof *delim; k++) {
			size_t len = delim[k].len;
			if (i + 2 * len > e || memcmp(s + i, delim[k].mark, len)) continue;
			size_t c = closer(s, i + len, e, delim[k].mark, len);
			if (c == e || c == i + len) continue;
			push(em, plain, i, base);
			push(em, i, c + len, base | delim[k].style);
			plain = i = c + len;
			break;
		}
		if (k == sizeof delim / sizeof *delim) i++;
	}
	push(em, plain, e, base);
}

int md_scan(const char *s, size_t n, struct md_run *out, int cap)
{
	struct emit em = {out, 0, cap};
	int fence = 0;

	for (size_t i = 0; i < n && em.n < cap; ) {
		const char *nl = memchr(s + i, '\n', n - i);
		size_t e = nl ? (size_t)(nl - s) : n;
		size_t next = nl ? e + 1 : n;
		uint16_t base = 0;
		size_t c = i;

		if (e - i >= 3 && !memcmp(s + i, "```", 3)) {
			fence = !fence;
			push(&em, i, next, MD_CODE | MD_BLOCK);
			i = next;
			continue;
		}
		if (fence) {
			push(&em, i, next, MD_CODE | MD_BLOCK);
			i = next;
			continue;
		}
		if (c < e && s[c] == '>') {
			base = MD_QUOTE;
		} else if (c < e && s[c] == '#') {
			size_t h = c;
			while (h < e && s[h] == '#') h++;
			if (h < e && s[h] == ' ') base = h - c == 1 ? MD_H1 : MD_H2;
		}
		inl(&em, s, c, e, base);
		push(&em, e, next, base);
		i = next;
	}
	return em.n;
}
