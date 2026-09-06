#ifndef WHIMSY_IDENTITY_H
#define WHIMSY_IDENTITY_H

#include <stdint.h>

#define ID_FP_LEN 75            /* the pubkey in hex, 6 chars a group, space separated, NUL */

struct identity {
	uint8_t seed[32];
	uint8_t sk[64];         /* eddsa */
	uint8_t pk[32];         /* eddsa; also the account id and the mailbox id */
	uint8_t xsk[32];
	uint8_t xpk[32];
};

void identity_from_seed(struct identity *id, const uint8_t seed[32]);
void identity_generate(struct identity *id);
void identity_wipe(struct identity *id);

void identity_fingerprint(char out[ID_FP_LEN], const uint8_t pk[32]);

#endif
