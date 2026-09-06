#ifndef WHIMSY_CRYPTO_H
#define WHIMSY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define WC_MAC   16
#define WC_NONCE 24

void wc_random(void *out, size_t n);
void wc_wipe(void *p, size_t n);
int  wc_equal(const uint8_t *a, const uint8_t *b, size_t n);
/* 0 for a small-order ed25519 encoding, which verifies against any message */
int  wc_pk_ok(const uint8_t pk[32]);

void wc_sign_keypair(uint8_t sk[64], uint8_t pk[32], const uint8_t seed[32]);
void wc_sign(uint8_t sig[64], const uint8_t sk[64], const uint8_t *m, size_t n);
int  wc_verify(const uint8_t sig[64], const uint8_t pk[32], const uint8_t *m, size_t n);

void wc_x25519_keypair(uint8_t sk[32], uint8_t pk[32]);
void wc_x25519_pk(uint8_t pk[32], const uint8_t sk[32]);
/* blake2b of the x25519 shared secret; -1 when the peer forced an all-zero one */
int  wc_dh(uint8_t shared[32], const uint8_t sk[32], const uint8_t pk[32]);
/* unhashed x25519, as noise wants it; -1 when the peer forced an all-zero result */
int  wc_dh_raw(uint8_t out[32], const uint8_t sk[32], const uint8_t pk[32]);

/* the x25519 half of an eddsa keypair: secret from the seed, public from the pubkey */
void wc_x25519_from_seed(uint8_t xsk[32], const uint8_t seed[32]);
void wc_x25519_from_sign_pk(uint8_t xpk[32], const uint8_t pk[32]);

/* argon2id over a blocks*1KiB work area; -1 when that will not allocate.
 * the caller checks the parameters: monocypher needs blocks >= 8 * lanes */
int  wc_argon2(uint8_t out[32], const void *pass, size_t pass_n, const uint8_t salt[16],
               uint32_t blocks, uint32_t passes, uint32_t lanes);

void wc_hash(uint8_t *out, size_t out_n, const uint8_t *m, size_t n);
void wc_kdf(uint8_t out[32], const uint8_t key[32], const char *label);

/* ct holds n + WC_MAC bytes: ciphertext then mac */
void wc_seal(uint8_t *ct, const uint8_t key[32], const uint8_t nonce[WC_NONCE],
             const uint8_t *ad, size_t ad_n, const uint8_t *pt, size_t n);
/* pt holds ct_n - WC_MAC bytes. -1 on a bad mac or ct_n < WC_MAC */
int  wc_open(uint8_t *pt, const uint8_t key[32], const uint8_t nonce[WC_NONCE],
             const uint8_t *ad, size_t ad_n, const uint8_t *ct, size_t ct_n);

/* rfc8439 chacha20-poly1305, one message per (key, nonce). noise transport uses these;
 * everything else uses the xchacha pair above. pt and ct may be the same buffer */
void wc_seal_ietf(uint8_t *ct, const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t *ad, size_t ad_n, const uint8_t *pt, size_t n);
int  wc_open_ietf(uint8_t *pt, const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t *ad, size_t ad_n, const uint8_t *ct, size_t ct_n);

#endif
