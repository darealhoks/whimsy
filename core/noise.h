#ifndef WHIMSY_NOISE_H
#define WHIMSY_NOISE_H

#include <stddef.h>
#include <stdint.h>

/* Noise_IK_25519_ChaChaPoly_BLAKE2b, empty prologue, empty handshake payloads.
 * The initiator's static is its x25519 identity; the responder's static is pinned
 * from the invite. The responder learns the initiator's static from message 1 (rs),
 * which is what tells it whether the caller already has an account. */

#define NOISE_MSG1 96           /* e[32] | enc(s)[48] | tag[16] */
#define NOISE_MSG2 48           /* e[32] | tag[16] */
#define NOISE_TAG  16

enum noise_err { NOISE_OK = 0, NOISE_EBAD = -1 };

struct noise {
	uint8_t ck[64], h[64], k[32];
	uint64_t n;                     /* handshake cipher counter */
	uint8_t e_sk[32], e_pk[32];
	uint8_t s_sk[32], s_pk[32];
	uint8_t re[32], rs[32];
	uint8_t send[32], recv[32];
	uint64_t ns, nr;
};

int noise_client_hello(struct noise *s, const uint8_t sk[32], const uint8_t pk[32],
                       const uint8_t rs[32], uint8_t out[NOISE_MSG1]);
int noise_client_done(struct noise *s, const uint8_t in[NOISE_MSG2]);
int noise_server_hello(struct noise *s, const uint8_t sk[32], const uint8_t pk[32],
                       const uint8_t in[NOISE_MSG1]);
int noise_server_done(struct noise *s, uint8_t out[NOISE_MSG2]);

/* ct holds n + NOISE_TAG bytes; ct and pt may be the same buffer */
void noise_encrypt(struct noise *s, uint8_t *ct, const uint8_t *pt, size_t n);
int  noise_decrypt(struct noise *s, uint8_t *pt, const uint8_t *ct, size_t ct_n);

void noise_wipe(struct noise *s);

#endif
