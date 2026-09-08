#ifndef WHIMSY_MD_H
#define WHIMSY_MD_H

#include <stddef.h>
#include <stdint.h>

/* The markdown subset of .map/gui.md, over already sanitized text. Markers stay
 * visible: runs are contiguous byte ranges of the input, in order, covering all of
 * it, so a caller wraps the original text and looks the style up per range. */

#define MD_BOLD    0x01
#define MD_ITALIC  0x02
#define MD_CODE    0x04
#define MD_STRIKE  0x08
#define MD_SPOILER 0x10
#define MD_LINK    0x20
#define MD_QUOTE   0x40
#define MD_H1      0x80
#define MD_H2      0x100
#define MD_BLOCK   0x200        /* fenced code, with MD_CODE */
#define MD_MENTION 0x400        /* '@' and six hex: whimsy_mention's token */

struct md_run {
	size_t at, n;
	uint16_t style;
};

/* returns the run count, at most cap; stops early when full, so the tail of the
 * text keeps whatever style the last run gave it */
int md_scan(const char *s, size_t n, struct md_run *out, int cap);

#endif
