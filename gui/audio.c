#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "audio.h"

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
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

/* the ceiling is on the decoded pcm, not the file, because that is what the allocation
 * follows: ten minutes of s16 stereo is about 100 MB from a 10 MB mp3 */
#define PCM_MAX (192u << 20)

static struct {
	size_t g, i;
	int loaded;
	SDL_AudioStream *st;
	uint8_t *pcm;
	size_t n;                       /* decoded bytes */
	int frame;                      /* bytes per sample frame */
	int rate;
	int sdlmem;                     /* pcm is SDL_LoadWAV_IO's, not malloc's */
} A;

void audio_stop(void)
{
	if (A.st) SDL_DestroyAudioStream(A.st);
	if (A.pcm) { if (A.sdlmem) SDL_free(A.pcm); else free(A.pcm); }
	memset(&A, 0, sizeof A);
}

const char *audio_ext(const char *name, size_t n)
{
	char e[5];
	const char *d = NULL;
	size_t k = 0;
	for (size_t i = 0; i < n; i++) if (name[i] == '.') d = name + i + 1;
	if (!d) return NULL;
	for (const char *p = d; p < name + n && k < 4; p++, k++)
		e[k] = (*p >= 'A' && *p <= 'Z') ? (char)(*p + 32) : *p;
	e[k] = 0;
	if (!strcmp(e, "wav")) return "wav";
	if (!strcmp(e, "mp3")) return "mp3";
	if (!strcmp(e, "ogg") || !strcmp(e, "oga")) return "ogg";
	return NULL;
}

static const char *decode(const char *ext, const void *b, size_t n, SDL_AudioSpec *spec)
{
	if (n > INT32_MAX) return "too big";
	if (!strcmp(ext, "wav")) {
		Uint32 len = 0;
		SDL_IOStream *io = SDL_IOFromConstMem(b, n);
		if (!io) return "out of memory";
		if (!SDL_LoadWAV_IO(io, true, spec, &A.pcm, &len)) return "will not decode";
		A.sdlmem = 1;
		A.n = len;
	} else if (!strcmp(ext, "mp3")) {
		drmp3_config cfg;
		drmp3_uint64 frames = 0;
		drmp3_int16 *pcm;
		memset(&cfg, 0, sizeof cfg);
		if (!(pcm = drmp3_open_memory_and_read_pcm_frames_s16(b, n, &cfg, &frames, NULL)))
			return "will not decode";
		A.pcm = (uint8_t *)pcm;
		A.n = (size_t)frames * cfg.channels * 2;
		spec->format = SDL_AUDIO_S16;
		spec->channels = (int)cfg.channels;
		spec->freq = (int)cfg.sampleRate;
	} else {
		int ch = 0, rate = 0, samples;
		short *pcm = NULL;
		if ((samples = stb_vorbis_decode_memory(b, (int)n, &ch, &rate, &pcm)) < 0 || !pcm)
			return "will not decode";
		A.pcm = (uint8_t *)pcm;
		A.n = (size_t)samples * (size_t)ch * 2;
		spec->format = SDL_AUDIO_S16;
		spec->channels = ch;
		spec->freq = rate;
	}
	if (!A.n || spec->channels <= 0 || spec->freq <= 0) return "empty";
	if (A.n > PCM_MAX) return "too long to play";
	A.frame = spec->channels * 2;
	A.rate = spec->freq;
	return NULL;
}

const char *audio_toggle(size_t g, size_t i, const char *ext, const void *b, size_t n)
{
	SDL_AudioSpec spec;
	const char *err;

	if (A.loaded && A.g == g && A.i == i) {
		if (SDL_AudioStreamDevicePaused(A.st)) SDL_ResumeAudioStreamDevice(A.st);
		else SDL_PauseAudioStreamDevice(A.st);
		return NULL;
	}
	audio_stop();
	/* main.c inits video only: an idle client never opens the audio backend */
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return "no audio device";
	if ((err = decode(ext, b, n, &spec))) { audio_stop(); return err; }
	if (!(A.st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL))) {
		audio_stop();
		return "no audio device";
	}
	SDL_PutAudioStreamData(A.st, A.pcm, (int)A.n);
	SDL_FlushAudioStream(A.st);
	SDL_ResumeAudioStreamDevice(A.st);
	A.g = g;
	A.i = i;
	A.loaded = 1;
	return NULL;
}

int audio_state(size_t g, size_t i, float *frac)
{
	if (!A.loaded || A.g != g || A.i != i) return AUDIO_OFF;
	if (frac) {
		int q = SDL_GetAudioStreamQueued(A.st);
		float f = A.n ? 1.0f - (float)(q < 0 ? 0 : q) / (float)A.n : 1;
		*frac = f < 0 ? 0 : f > 1 ? 1 : f;
	}
	return SDL_AudioStreamDevicePaused(A.st) ? AUDIO_PAUSED : AUDIO_PLAYING;
}

float audio_total(void)
{
	if (!A.loaded || !A.frame || !A.rate) return 0;
	return (float)(A.n / (size_t)A.frame) / (float)A.rate;
}

void audio_seek(float frac)
{
	size_t off;
	if (!A.loaded) return;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	off = (size_t)(frac * (float)A.n);
	off -= off % (size_t)A.frame;
	if (off >= A.n) return;
	SDL_ClearAudioStream(A.st);
	SDL_PutAudioStreamData(A.st, A.pcm + off, (int)(A.n - off));
	SDL_FlushAudioStream(A.st);
}
