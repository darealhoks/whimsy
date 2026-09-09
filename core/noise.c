#include "noise.h"

#include <string.h>

#include "crypto.h"

#define HASH  64
#define BLOCK 128       /* blake2b block, for hmac padding */

static const char NAME[] = "Noise_IK_25519_ChaChaPoly_BLAKE2b";

/* hmac-blake2b, the noise spec's prf. key is always HASH bytes and m at most HASH + 1 */
static void hmac(uint8_t out[HASH], const uint8_t key[HASH], const uint8_t *m, size_t n)
{
	uint8_t buf[BLOCK + HASH + 1], inner[HASH];
	if (n > HASH + 1) { memset(out, 0, HASH); return; }   /* buf is exact-fit; a garbage prf is loud */
	memset(buf, 0x36, BLOCK);
	for (int i = 0; i < HASH; i++) buf[i] ^= key[i];
	if (n) memcpy(buf + BLOCK, m, n);
	wc_hash(inner, HASH, buf, BLOCK + n);
	memset(buf, 0x5c, BLOCK);
	for (int i = 0; i < HASH; i++) buf[i] ^= key[i];
	memcpy(buf + BLOCK, inner, HASH);
	wc_hash(out, HASH, buf, BLOCK + HASH);
	wc_wipe(buf, sizeof buf);
	wc_wipe(inner, sizeof inner);
}

static void hkdf(uint8_t o1[HASH], uint8_t o2[HASH], const uint8_t ck[HASH],
                 const uint8_t *ikm, size_t ikm_n)
{
	uint8_t tk[HASH], buf[HASH + 1];
	hmac(tk, ck, ikm, ikm_n);
	buf[0] = 1;
	hmac(o1, tk, buf, 1);
	memcpy(buf, o1, HASH);
	buf[HASH] = 2;
	hmac(o2, tk, buf, HASH + 1);
	wc_wipe(tk, sizeof tk);
	wc_wipe(buf, sizeof buf);
}

static void mix_hash(struct noise *s, const uint8_t *p, size_t n)
{
	uint8_t buf[HASH + 64];
	_Static_assert(32 + NOISE_TAG <= 64, "mix_hash buf too small for enc(s)");
	if (n > sizeof buf - HASH) return;       /* skipping the mix breaks the handshake, not the stack */
	memcpy(buf, s->h, HASH);
	if (n) memcpy(buf + HASH, p, n);
	wc_hash(s->h, HASH, buf, HASH + n);
}

static void mix_key(struct noise *s, const uint8_t ikm[32])
{
	uint8_t ck[HASH], tk[HASH];
	hkdf(ck, tk, s->ck, ikm, 32);
	memcpy(s->ck, ck, HASH);
	memcpy(s->k, tk, 32);
	s->n = 0;
	wc_wipe(ck, sizeof ck);
	wc_wipe(tk, sizeof tk);
}

static void nonce12(uint8_t out[12], uint64_t n)
{
	memset(out, 0, 4);
	for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(n >> (8 * i));
}

static void enc_hash(struct noise *s, uint8_t *out, const uint8_t *pt, size_t n)
{
	uint8_t nn[12];
	nonce12(nn, s->n++);
	wc_seal_ietf(out, s->k, nn, s->h, HASH, pt, n);
	mix_hash(s, out, n + NOISE_TAG);
}

static int dec_hash(struct noise *s, uint8_t *out, const uint8_t *ct, size_t ct_n)
{
	uint8_t nn[12];
	nonce12(nn, s->n++);
	if (wc_open_ietf(out, s->k, nn, s->h, HASH, ct, ct_n)) return NOISE_EBAD;
	mix_hash(s, ct, ct_n);
	return NOISE_OK;
}

static int dh_mix(struct noise *s, const uint8_t sk[32], const uint8_t pk[32])
{
	uint8_t dh[32];
	int bad = wc_dh_raw(dh, sk, pk);
	if (!bad) mix_key(s, dh);
	wc_wipe(dh, sizeof dh);
	return bad ? NOISE_EBAD : NOISE_OK;
}

static void begin(struct noise *s, const uint8_t sk[32], const uint8_t pk[32])
{
	memset(s, 0, sizeof *s);
	memcpy(s->h, NAME, sizeof NAME - 1);    /* shorter than HASH, so zero padded */
	memcpy(s->ck, s->h, HASH);
	mix_hash(s, NULL, 0);                   /* empty prologue, still hashed */
	memcpy(s->s_sk, sk, 32);
	memcpy(s->s_pk, pk, 32);
}

static void split(struct noise *s, int initiator)
{
	uint8_t k1[HASH], k2[HASH];
	hkdf(k1, k2, s->ck, NULL, 0);
	memcpy(initiator ? s->send : s->recv, k1, 32);
	memcpy(initiator ? s->recv : s->send, k2, 32);
	s->ns = s->nr = 0;
	wc_wipe(k1, sizeof k1);
	wc_wipe(k2, sizeof k2);
	wc_wipe(s->ck, sizeof s->ck);
	wc_wipe(s->k, sizeof s->k);
	wc_wipe(s->e_sk, sizeof s->e_sk);
	wc_wipe(s->s_sk, sizeof s->s_sk);
}

int noise_client_hello(struct noise *s, const uint8_t sk[32], const uint8_t pk[32],
                       const uint8_t rs[32], uint8_t out[NOISE_MSG1])
{
	uint8_t empty[1] = { 0 };
	begin(s, sk, pk);
	memcpy(s->rs, rs, 32);
	mix_hash(s, s->rs, 32);                 /* pre-message: the responder's static */

	wc_x25519_keypair(s->e_sk, s->e_pk);
	memcpy(out, s->e_pk, 32);
	mix_hash(s, s->e_pk, 32);
	if (dh_mix(s, s->e_sk, s->rs)) return NOISE_EBAD;        /* es */
	enc_hash(s, out + 32, s->s_pk, 32);
	if (dh_mix(s, s->s_sk, s->rs)) return NOISE_EBAD;        /* ss */
	enc_hash(s, out + 80, empty, 0);
	return NOISE_OK;
}

int noise_client_done(struct noise *s, const uint8_t in[NOISE_MSG2])
{
	uint8_t empty[1];
	memcpy(s->re, in, 32);
	mix_hash(s, s->re, 32);
	if (dh_mix(s, s->e_sk, s->re)) return NOISE_EBAD;        /* ee */
	if (dh_mix(s, s->s_sk, s->re)) return NOISE_EBAD;        /* se */
	if (dec_hash(s, empty, in + 32, NOISE_TAG)) return NOISE_EBAD;
	split(s, 1);
	return NOISE_OK;
}

int noise_server_hello(struct noise *s, const uint8_t sk[32], const uint8_t pk[32],
                       const uint8_t in[NOISE_MSG1])
{
	uint8_t empty[1];
	begin(s, sk, pk);
	mix_hash(s, s->s_pk, 32);

	memcpy(s->re, in, 32);
	mix_hash(s, s->re, 32);
	if (dh_mix(s, s->s_sk, s->re)) return NOISE_EBAD;        /* es */
	if (dec_hash(s, s->rs, in + 32, 32 + NOISE_TAG)) return NOISE_EBAD;
	if (dh_mix(s, s->s_sk, s->rs)) return NOISE_EBAD;        /* ss */
	if (dec_hash(s, empty, in + 80, NOISE_TAG)) return NOISE_EBAD;
	return NOISE_OK;
}

int noise_server_done(struct noise *s, uint8_t out[NOISE_MSG2])
{
	uint8_t empty[1] = { 0 };
	wc_x25519_keypair(s->e_sk, s->e_pk);
	memcpy(out, s->e_pk, 32);
	mix_hash(s, s->e_pk, 32);
	if (dh_mix(s, s->e_sk, s->re)) return NOISE_EBAD;        /* ee */
	if (dh_mix(s, s->e_sk, s->rs)) return NOISE_EBAD;        /* se */
	enc_hash(s, out + 32, empty, 0);
	split(s, 0);
	return NOISE_OK;
}

void noise_encrypt(struct noise *s, uint8_t *ct, const uint8_t *pt, size_t n)
{
	uint8_t nn[12];
	nonce12(nn, s->ns++);
	wc_seal_ietf(ct, s->send, nn, NULL, 0, pt, n);
}

int noise_decrypt(struct noise *s, uint8_t *pt, const uint8_t *ct, size_t ct_n)
{
	uint8_t nn[12];
	nonce12(nn, s->nr);
	if (wc_open_ietf(pt, s->recv, nn, NULL, 0, ct, ct_n)) return NOISE_EBAD;
	s->nr++;
	return NOISE_OK;
}

void noise_wipe(struct noise *s) { wc_wipe(s, sizeof *s); }
