#include "identity.h"

#include "crypto.h"

void identity_from_seed(struct identity *id, const uint8_t seed[32])
{
	for (int i = 0; i < 32; i++) id->seed[i] = seed[i];
	wc_sign_keypair(id->sk, id->pk, id->seed);
	wc_x25519_from_seed(id->xsk, id->seed);
	wc_x25519_from_sign_pk(id->xpk, id->pk);
}

void identity_generate(struct identity *id)
{
	uint8_t seed[32];
	wc_random(seed, 32);
	identity_from_seed(id, seed);
	wc_wipe(seed, sizeof seed);
}

void identity_wipe(struct identity *id) { wc_wipe(id, sizeof *id); }

void identity_fingerprint(char out[ID_FP_LEN], const uint8_t pk[32])
{
	static const char hex[] = "0123456789abcdef";
	char *p = out;
	for (int i = 0; i < 32; i++) {
		if (i && i % 3 == 0) *p++ = ' ';
		*p++ = hex[pk[i] >> 4];
		*p++ = hex[pk[i] & 15];
	}
	*p = 0;
}
