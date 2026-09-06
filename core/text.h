#ifndef WHIMSY_TEXT_H
#define WHIMSY_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Trust boundary. Message bytes reach a frontend only through text_sanitize.
 * Output is valid utf-8: an invalid sequence becomes U+FFFD, controls and escapes
 * are gone, tab is a space, newline survives, markdown markers are left alone.
 * Truncation at cap never splits a sequence. Zero-width codepoints are dropped
 * outright, marks capped per base, so text_width is exact on the output.
 * Nothing here allocates and nothing NUL-terminates; lengths carry everywhere. */

#define TEXT_MAX_OUT(n) ((n) * 3)       /* worst case: every byte becomes U+FFFD */
#define TEXT_MAX_MARKS 8                /* zero-width marks kept per base codepoint */

/* snprintf-style: returns the length the sanitized text needs. writes only what
 * fits in cap, so the result is complete only when it is <= cap. */
size_t text_sanitize(void *out, size_t cap, const void *in, size_t n);

/* one codepoint from n bytes (n > 0); returns bytes consumed, never 0.
 * *cp is U+FFFD for an invalid sequence. */
size_t text_step(const void *s, size_t n, uint32_t *cp);

int text_cp_width(uint32_t cp);         /* terminal cells: 0, 1 or 2 */
int text_width(const void *s, size_t n);

#endif
