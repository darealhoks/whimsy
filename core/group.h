#ifndef WHIMSY_GROUP_H
#define WHIMSY_GROUP_H

#include <stddef.h>
#include <stdint.h>

#include "crypto.h"
#include "identity.h"
#include "wire.h"

/* Membership records, sender chains, message encrypt/decrypt. No allocation, no i/o.
 * A group is created by its owner, joined by opening a SEALED senderkey. Every
 * membership change regenerates the local send chain and drops every receive chain;
 * each member then seals a fresh chain to every other member. */

#define GROUP_MAX_MEMBERS WIRE_MAX_MEMBERS
#define GROUP_SKIP        512           /* out-of-order window, in message indices */
#define GROUP_MAX_JUMP    (1u << 13)    /* forward ratchet steps one blob may cost */

#define GROUP_MAX_REC   (16 + 4 + 32 + 1 + 255 + 1 + WIRE_MAX_CHANNELS * (2 + 1 + 255) \
                         + 2 + GROUP_MAX_MEMBERS * 32 + 64)
#define GROUP_MAX_INNER (1 + 16 + 32 + 32 + 32 + 4 + GROUP_MAX_REC)
#define GROUP_MAX_PT    16384           /* the pad that holds WIRE_SEAL_PT_HDR + GROUP_MAX_INNER */
#define GROUP_MAX_SEAL  (WIRE_BLOB_HDR + GROUP_MAX_PT + WC_MAC)
#define GROUP_MAX_PTB   WIRE_MAX_GROUP_PT
#define GROUP_MAX_BLOB  (WIRE_BLOB_HDR + GROUP_MAX_PTB + WC_MAC)

enum group_err {
	GROUP_OK       =  0,
	GROUP_EWIRE    = -1,    /* malformed bytes */
	GROUP_ESIG     = -2,    /* signature does not check */
	GROUP_ECRYPT   = -3,    /* aead did not open */
	GROUP_EMEMBER  = -4,    /* not a member, or not the owner, or self */
	GROUP_ENOCHAIN = -5,    /* no sender chain for this sender yet */
	GROUP_EOLD     = -6,    /* index below the window: dropped */
	GROUP_EREPLAY  = -7,
	GROUP_EFAR     = -8,    /* index further ahead than GROUP_MAX_JUMP */
	GROUP_EGROUP   = -9,    /* wrong group, wrong owner, or a stale record */
	GROUP_EFULL    = -10,
	GROUP_ESPACE   = -11,
	GROUP_ETYPE    = -12,   /* a sealed inner of another kind */
	GROUP_EKICKED  = -13    /* the record it carries is valid and no longer names us */
};

struct group_chain {
	uint8_t pk[32];                 /* sender; unused on the send chain */
	uint8_t cid[32];                /* per-epoch chain id, cleartext in every blob */
	uint8_t ck[32];                 /* chain key at index base */
	uint8_t hk[32];                 /* masks the blob index; fixed for the chain's life */
	uint32_t base;
	uint8_t seen[GROUP_SKIP / 8];   /* bit i: index base+i already accepted */
};

struct group {
	uint8_t id[16];
	uint8_t rec[GROUP_MAX_REC];     /* the owner-signed membership record, verbatim */
	size_t rec_n;
	struct wire_rec r;              /* decoded view of rec */
	struct group_chain send;
	struct group_chain recv[GROUP_MAX_MEMBERS];
	uint8_t nrecv;
};

struct group_msg {
	uint8_t sender[32];
	uint8_t kind;
	uint16_t channel;
	uint64_t time;
	uint32_t index;                 /* with sender, the message id */
	const uint8_t *reply;           /* WIRE_MSGID bytes into the scratch, NULL when not a reply */
	const uint8_t *payload;         /* into the caller's scratch */
	size_t payload_n;
};

/* the group id is bound to the owner key: squatting an id needs a 128-bit preimage */
void group_id(uint8_t id[16], const uint8_t owner[32], const uint8_t salt[16]);

int group_create(struct group *g, const struct identity *owner, const char *name,
                 const char *const *channels, uint8_t nchan);

/* owner only; bumps the version, re-signs, rotates */
int group_add(struct group *g, const struct identity *owner, const uint8_t pk[32]);
int group_kick(struct group *g, const struct identity *owner, const uint8_t pk[32]);
/* owner only; an empty name is legal, a DM has one */
int group_rename(struct group *g, const struct identity *owner, const char *name);
/* owner only; op 'a' appends a, 'd' drops it, 'r' renames it to b. the last one
 * cannot go, and a rename moves the channel to the end of the list */
int group_chan(struct group *g, const struct identity *owner, char op,
               const char *a, const char *b);

size_t group_member_count(const struct group *g);
const uint8_t *group_member_at(const struct group *g, size_t i);
int group_has(const struct group *g, const uint8_t pk[32]);

/* verifies the owner signature; on a zeroed g this joins the group named by the record */
int group_apply_rec(struct group *g, const uint8_t *rec, size_t n);

/* drop the receive chain held for pk, so the next senderkey from them installs whatever
 * cid it carries, at or past the last index we accepted from them. the way out of a
 * chain the two sides disagree about */
void group_forget(struct group *g, const uint8_t pk[32]);
/* receive chains that can open a blob; one group_forget dropped is not one */
uint8_t group_chain_count(const struct group *g);

/* anything sealed to one key rides these two. out needs GROUP_MAX_SEAL; inner starts
 * with a wire_inner kind byte. refuses to == ourselves */
int group_seal(const struct identity *me, const uint8_t to[32],
               const void *inner, size_t inner_n, uint8_t *out, size_t cap, size_t *len);
/* pt needs GROUP_MAX_PT; *inner points into pt */
int group_open(const struct identity *me, const uint8_t *blob, size_t n,
               uint8_t *pt, size_t cap, uint8_t sender[32],
               const uint8_t **inner, size_t *inner_n);

/* out needs GROUP_MAX_SEAL; refuses i == ourselves */
int group_seal_to(const struct group *g, const struct identity *me, size_t i,
                  uint8_t *out, size_t cap, size_t *len);
/* pt needs GROUP_MAX_PT; *sk points into pt. GROUP_ETYPE when the inner is not one */
int group_open_sealed(const struct identity *me, const uint8_t *blob, size_t n,
                      uint8_t *pt, size_t cap, uint8_t sender[32], struct wire_senderkey *sk);
/* applies the carried record, then installs sender's chain. g may be zeroed (join).
 * GROUP_EKICKED when a record newer than the one held checks out and drops us; g is
 * untouched, so the caller
 * still holds the chains and must erase the group itself. */
int group_recv_senderkey(struct group *g, const struct identity *me,
                         const uint8_t sender[32], const struct wire_senderkey *sk);

/* out needs GROUP_MAX_BLOB. reply is WIRE_MSGID bytes or NULL; the index this blob
 * goes out on is g->send.base before the call. */
int group_send(struct group *g, const struct identity *me, uint16_t channel, uint8_t kind,
               uint64_t time, const uint8_t *reply, const void *payload, size_t n,
               uint8_t *out, size_t cap, size_t *len);
/* scratch needs WIRE_MAX_BODY; m->payload points into it */
int group_recv(struct group *g, const uint8_t *blob, size_t n,
               uint8_t *scratch, size_t cap, struct group_msg *m);

#endif
