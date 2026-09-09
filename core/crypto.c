#include "crypto.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

#include "monocypher.h"

#ifdef _WIN32
void wc_random(void *out, size_t n)
{
	if (BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) abort();
}
#else
void wc_random(void *out, size_t n)
{
	uint8_t *p = out;
	while (n) {
		ssize_t r = getrandom(p, n, 0);
		if (r < 0) abort();
		p += r;
		n -= (size_t)r;
	}
}
#endif

void wc_wipe(void *p, size_t n) { crypto_wipe(p, n); }

int wc_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
	return d == 0;
}

void wc_sign_keypair(uint8_t sk[64], uint8_t pk[32], const uint8_t seed[32])
{
	uint8_t tmp[32];
	memcpy(tmp, seed, 32); /* monocypher wipes the seed it is handed */
	crypto_eddsa_key_pair(sk, pk, tmp);
}

void wc_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t *m, size_t n)
{
	crypto_eddsa_sign(sig, sk, m, n);
}

/* the seven small-order ed25519 encodings (libsodium's blacklist). a pubkey among them
 * makes [h]A vanish, so ([s]B || s) verifies against any message */
static const uint8_t small_order[7][32] = {
	{ 0 },
	{ 0x01 },
	{ 0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4, 0x89,
	  0xf2, 0xef, 0x98, 0xf0, 0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39,
	  0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05 },
	{ 0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b, 0x76,
	  0x0d, 0x10, 0x67, 0x0f, 0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6,
	  0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a },
	{ 0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
	{ 0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f },
	{ 0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f }
};

int wc_pk_ok(const uint8_t pk[32])
{
	for (int j = 0; j < 7; j++) {
		uint8_t d = 0;
		for (int i = 0; i < 31; i++) d |= pk[i] ^ small_order[j][i];
		d |= (pk[31] & 0x7f) ^ small_order[j][31]; /* the sign bit does not change the order */
		if (!d) return 0;
	}
	return 1;
}

int wc_verify(const uint8_t sig[64], const uint8_t pk[32], const uint8_t *m, size_t n)
{
	if (!wc_pk_ok(pk)) return 0;
	return crypto_eddsa_check(sig, pk, m, n) == 0;
}

void wc_x25519_keypair(uint8_t sk[32], uint8_t pk[32])
{
	wc_random(sk, 32);
	crypto_x25519_public_key(pk, sk);
}

void wc_x25519_pk(uint8_t pk[32], const uint8_t sk[32])
{
	crypto_x25519_public_key(pk, sk);
}

int wc_dh(uint8_t shared[32], const uint8_t sk[32], const uint8_t pk[32])
{
	uint8_t raw[32];
	int e = wc_dh_raw(raw, sk, pk);
	if (!e) crypto_blake2b(shared, 32, raw, 32);
	crypto_wipe(raw, sizeof raw);
	return e;
}

int wc_dh_raw(uint8_t out[32], const uint8_t sk[32], const uint8_t pk[32])
{
	crypto_x25519(out, sk, pk);
	uint8_t z = 0;
	for (int i = 0; i < 32; i++) z |= out[i];
	return z ? 0 : -1;
}

void wc_x25519_from_seed(uint8_t xsk[32], const uint8_t seed[32])
{
	uint8_t h[64];
	crypto_blake2b(h, 64, seed, 32);
	crypto_eddsa_trim_scalar(xsk, h);
	crypto_wipe(h, sizeof h);
}

void wc_x25519_from_sign_pk(uint8_t xpk[32], const uint8_t pk[32])
{
	crypto_eddsa_to_x25519(xpk, pk);
}

int wc_argon2(uint8_t out[32], const void *pass, size_t pass_n, const uint8_t salt[16],
              uint32_t blocks, uint32_t passes, uint32_t lanes)
{
	size_t work_n = (size_t)blocks * 1024;
	if (work_n / 1024 != blocks) return -1; /* wraps on a 32-bit size_t */
	void *work = malloc(work_n);
	if (!work) return -1;
	crypto_argon2_config cfg = { CRYPTO_ARGON2_ID, blocks, passes, lanes };
	crypto_argon2_inputs in = { pass, salt, (uint32_t)pass_n, 16 };
	crypto_argon2(out, 32, work, cfg, in, crypto_argon2_no_extras);
	free(work);
	return 0;
}

void wc_hash(uint8_t *out, size_t out_n, const uint8_t *m, size_t n)
{
	crypto_blake2b(out, out_n, m, n);
}

void wc_kdf(uint8_t out[32], const uint8_t key[32], const char *label)
{
	crypto_blake2b_keyed(out, 32, key, 32, (const uint8_t *)label, strlen(label));
}

void wc_seal(uint8_t *ct, const uint8_t key[32], const uint8_t nonce[WC_NONCE],
             const uint8_t *ad, size_t ad_n, const uint8_t *pt, size_t n)
{
	crypto_aead_lock(ct, ct + n, key, nonce, ad, ad_n, pt, n);
}

int wc_open(uint8_t *pt, const uint8_t key[32], const uint8_t nonce[WC_NONCE],
            const uint8_t *ad, size_t ad_n, const uint8_t *ct, size_t ct_n)
{
	if (ct_n < WC_MAC) return -1;
	size_t n = ct_n - WC_MAC;
	return crypto_aead_unlock(pt, ct + n, key, nonce, ad, ad_n, ct, n);
}

void wc_seal_ietf(uint8_t *ct, const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t *ad, size_t ad_n, const uint8_t *pt, size_t n)
{
	crypto_aead_ctx ctx;
	crypto_aead_init_ietf(&ctx, key, nonce);
	crypto_aead_write(&ctx, ct, ct + n, ad, ad_n, pt, n);
	crypto_wipe(&ctx, sizeof ctx); /* the ctx rekeys itself; one message per init */
}

int wc_open_ietf(uint8_t *pt, const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *ad, size_t ad_n, const uint8_t *ct, size_t ct_n)
{
	if (ct_n < WC_MAC) return -1;
	size_t n = ct_n - WC_MAC;
	crypto_aead_ctx ctx;
	crypto_aead_init_ietf(&ctx, key, nonce);
	int r = crypto_aead_read(&ctx, pt, ct + n, ad, ad_n, ct, n);
	crypto_wipe(&ctx, sizeof ctx);
	return r;
}
