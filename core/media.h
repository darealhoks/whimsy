#ifndef WHIMSY_MEDIA_H
#define WHIMSY_MEDIA_H

#include <stddef.h>

/* The one bounded door to the vendored media decoders: stb_image, dr_mp3 and
 * stb_vorbis are compiled here and nowhere else, so remote bytes reach ~25 kLOC of
 * format parsing only through these two calls. Both read the container header first
 * and refuse the ceilings below before a decoder allocates anything. Fuzzed by
 * tests/fuzz_media.c. */

#define MEDIA_MAX_DIM 8192            /* STBI_MAX_DIMENSIONS, per side */
#define MEDIA_MAX_PX  (1 << 24)       /* decoded pixels, the product the dim cap misses */
#define MEDIA_MAX_PCM (192u << 20)    /* decoded pcm bytes: about 8 minutes of s16 stereo */

/* png/jpg/gif/bmp bytes to want-component pixels. *out is malloc'd on success (0) and
 * free()d by the caller, untouched on failure (-1) */
int media_image(const void *b, size_t n, int want, unsigned char **out, int *w, int *h);

/* "mp3" or "ogg" bytes to interleaved s16 pcm. *out is malloc'd on success (0),
 * *bytes is its length, untouched on failure (-1) */
int media_audio(const char *ext, const void *b, size_t n,
                short **out, size_t *bytes, int *ch, int *rate);

#endif
