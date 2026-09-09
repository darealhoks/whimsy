#ifndef WHIMSY_AUDIO_H
#define WHIMSY_AUDIO_H

#include <stddef.h>

/* One sound at a time, owned by the row (g, i) that started it. wav decodes through
 * SDL, mp3 and ogg vorbis through core/media.c. Bytes reach a
 * decoder only on a click, never on paint. */

enum { AUDIO_OFF, AUDIO_PAUSED, AUDIO_PLAYING };

/* AUDIO_OFF unless (g, i) owns the player; *frac is 0..1 played, may be NULL */
int  audio_state(size_t g, size_t i, float *frac);
/* start (g, i), or pause and resume it once it holds the player. ext picks the decoder
 * and is what audio_ext returned. returns an error string, NULL when it worked */
const char *audio_toggle(size_t g, size_t i, const char *ext, const void *b, size_t n);
/* "wav", "mp3" or "ogg" for a name we play, NULL otherwise */
const char *audio_ext(const char *name, size_t n);
/* seconds of what the player holds, 0 when nothing is loaded */
float audio_total(void);
void audio_seek(float frac);
void audio_stop(void);

#endif
