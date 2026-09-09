#include "media.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#ifndef __clang__
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wcomma"
#pragma GCC diagnostic ignored "-Wextra-semi-stmt"
#pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif

/* stb_image shifts signed ints past the sign bit; the dev build is ubsan */
#ifdef __clang__
#pragma clang attribute push(__attribute__((no_sanitize("undefined"))), apply_to = function)
#endif
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS MEDIA_MAX_DIM
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#ifdef __clang__
#pragma clang attribute pop
#endif

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

int media_image(const void *b, size_t n, int want, unsigned char **out, int *w, int *h)
{
	int iw, ih, comp;
	if (n > INT_MAX) return -1;
	/* the header alone, no allocation: STBI_MAX_DIMENSIONS bounds each side but not
	 * their product, and 8192x8192 is a 268 MB decode out of an 8 kB avatar */
	if (!stbi_info_from_memory(b, (int)n, &iw, &ih, &comp)) return -1;
	if (iw < 1 || ih < 1 || (long long)iw * ih > MEDIA_MAX_PX) return -1;
	if (!(*out = stbi_load_from_memory(b, (int)n, &iw, &ih, &comp, want))) return -1;
	if (iw < 1 || ih < 1 || (long long)iw * ih > MEDIA_MAX_PX) {
		free(*out);
		return -1;
	}
	*w = iw;
	*h = ih;
	return 0;
}

static int mp3(const void *b, size_t n, short **out, size_t *bytes, int *ch, int *rate)
{
	drmp3 d;
	drmp3_uint64 frames, got;
	size_t want;
	if (!drmp3_init_memory(&d, b, n, NULL)) return -1;
	/* counts by decoding and seeking back: bounded by n, allocates nothing */
	frames = drmp3_get_pcm_frame_count(&d);
	want = (size_t)frames * d.channels * 2;
	if (!frames || d.channels < 1 || d.sampleRate < 1 || want > MEDIA_MAX_PCM ||
	    !(*out = malloc(want))) {
		drmp3_uninit(&d);
		return -1;
	}
	got = drmp3_read_pcm_frames_s16(&d, frames, *out);
	*bytes = (size_t)got * d.channels * 2;
	*ch = (int)d.channels;
	*rate = (int)d.sampleRate;
	drmp3_uninit(&d);
	if (!*bytes) { free(*out); return -1; }
	return 0;
}

static int ogg(const void *b, size_t n, short **out, size_t *bytes, int *ch, int *rate)
{
	int err = 0, got;
	stb_vorbis *v;
	stb_vorbis_info in;
	unsigned len;
	size_t want;
	if (n > INT_MAX || !(v = stb_vorbis_open_memory(b, (int)n, &err, NULL))) return -1;
	in = stb_vorbis_get_info(v);
	len = stb_vorbis_stream_length_in_samples(v);
	want = (size_t)len * (size_t)in.channels * 2;
	if (!len || in.channels < 1 || in.sample_rate < 1 || want > MEDIA_MAX_PCM ||
	    !(*out = malloc(want))) {
		stb_vorbis_close(v);
		return -1;
	}
	got = stb_vorbis_get_samples_short_interleaved(v, in.channels, *out, (int)(want / 2));
	stb_vorbis_close(v);
	*bytes = (size_t)(got < 0 ? 0 : got) * (size_t)in.channels * 2;
	*ch = in.channels;
	*rate = (int)in.sample_rate;
	if (!*bytes) { free(*out); return -1; }
	return 0;
}

int media_audio(const char *ext, const void *b, size_t n,
                short **out, size_t *bytes, int *ch, int *rate)
{
	if (!strcmp(ext, "mp3")) return mp3(b, n, out, bytes, ch, rate);
	if (!strcmp(ext, "ogg")) return ogg(b, n, out, bytes, ch, rate);
	return -1;
}
