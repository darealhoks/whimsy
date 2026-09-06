#ifndef WHIMSY_WIRE_H
#define WHIMSY_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* Trust boundary. Every wire_decode_* takes untrusted bytes and must not read past n.
 * Decoders are zero-copy: the pointers in the out structs alias the input buffer,
 * which must outlive them. Every decoder demands the whole buffer, no trailing bytes.
 *
 * Signatures are always the last 64 bytes and cover everything before them. The
 * encoders write zeros there when the struct's sig is NULL, so the caller signs with
 *     wc_sign(buf + len - 64, sk, buf, len - 64);
 */

#define WIRE_VER          14
#define WIRE_MAX_PAYLOAD  (65536 + 512)
#define WIRE_MAX_CHANNELS 32          /* decoder limit, not a protocol limit */
#define WIRE_MAX_MEMBERS  128         /* GROUP_MAX_MEMBERS sizes the fixed arrays off this */
#define WIRE_FRAME_HDR    6
#define WIRE_HDR          4           /* u32 le length prefix on every noise message */
#define WIRE_BODY_HDR     16          /* kind 1 + channel 2 + time 8 + reply flag 1 + len 4 */
#define WIRE_MSGID        36          /* sender pk 32 + index 4; the index never resets, so the pair
                                       * names one message for the life of the group */
#define WIRE_MAX_REACT    32          /* a REACT payload, bytes; empty clears */
#define WIRE_MAX_AVATAR   8192        /* one avatar, encoded bytes; empty clears */
#define WIRE_MAX_GROUP_PT 65536       /* the largest pad; a GROUP blob's plaintext */
#define WIRE_GROUP_PT_HDR 137         /* kind 1 + cid 32 + index 4 + sender 32 + len 4 + sig 64 */
#define WIRE_SEAL_PT_HDR  133         /* kind 1 + sender 32 + to 32 + len 4 + sig 64 */
#define WIRE_BLOB_HDR     61          /* ver 1 + hdr 32 + aux 4 + nonce 24 */
#define WIRE_MAX_BODY     (WIRE_MAX_GROUP_PT - WIRE_GROUP_PT_HDR)
#define WIRE_FILE_HDR     25          /* fid 16 + idx 4 + total 4 + name len 1 */
/* one chunk's bytes. 137 + 16 + WIRE_FILE_HDR + 255 + this is 65457, so a chunk with
 * the longest name still lands on the 64k pad rung */
#define WIRE_FILE_CHUNK   65024
#define WIRE_FILE_CHUNKS  160         /* chunks one file may have; caps a receiver's buffer */
#define WIRE_MAX_HOST     256
#define WIRE_INVITE_MAX   401         /* whimsy:// + host:port + '/' + 64 + '/' + 64 + NUL */

enum wire_err {
	WIRE_OK      =  0,
	WIRE_ETRUNC  = -1,
	WIRE_EVER    = -2,
	WIRE_ETYPE   = -3,
	WIRE_ELEN    = -4,
	WIRE_EJUNK   = -5,
	WIRE_ESPACE  = -6   /* encode only: output buffer too small */
};

enum wire_ftype {
	WIRE_F_REGISTER = 1, WIRE_F_PUT, WIRE_F_FETCH, WIRE_F_BLOB,
	WIRE_F_ACK, WIRE_F_DONE, WIRE_F_PING, WIRE_F_PONG, WIRE_F_ERR,
	/* PUTOK answers a PUT with the seq the blob landed under; REVOKE unlinks a blob
	 * from a mailbox and only the account that put it may */
	WIRE_F_PUTOK, WIRE_F_REVOKE
};
/* the blob kind is the first byte of the plaintext, never on the wire: the relay sees
 * one shape for both, and both pad to the same ladder */
enum wire_bkind { WIRE_B_GROUP = 1, WIRE_B_SEALED };
/* EDIT and DELETE point at a prior message through the reply field: the target's id.
 * an EDIT carries the new text, a DELETE carries nothing.
 * TYPING is ephemeral: no reply, no payload, never stored.
 * REACT names its target the same way; its payload is the reaction text, empty to clear.
 * GAVATAR is the group's own avatar: owner only, payload the encoded bytes, empty
 * clears. a person's own avatar never rides here, it is sealed to one key.
 * PURGE is the owner's: payload is a u64 watermark, and every message in the body's
 * channel at or before that time goes. not a count -- row order is local receive
 * order, so a count would purge a different set on every device.
 * HEAL is a member's ask for a fresh record and a re-seal; no reply, no payload */
enum wire_kind  { WIRE_K_TEXT = 1, WIRE_K_FILE, WIRE_K_MEMBERSHIP, WIRE_K_LINK,
                  WIRE_K_EDIT, WIRE_K_DELETE, WIRE_K_TYPING, WIRE_K_REACT,
                  WIRE_K_GAVATAR, WIRE_K_PURGE, WIRE_K_HEAL };
/* the first byte of a sealed inner. AVATAR's rest is the encoded bytes, empty clears */
enum wire_inner { WIRE_I_SENDERKEY = 1, WIRE_I_AVATAR };
enum wire_ecode { WIRE_E_BADREQ = 1, WIRE_E_BADTOKEN, WIRE_E_NOACCOUNT, WIRE_E_NOMAILBOX,
                  WIRE_E_INTERNAL, WIRE_E_FULL };

struct wire_frame {
	uint8_t type;
	union {
		struct { const uint8_t *token, *pk, *sig; } reg;
		struct { const uint8_t *mailbox, *blob; size_t blob_n; } put;
		struct { uint64_t seq; const uint8_t *blob; size_t blob_n; } blob;
		struct { uint64_t seq; } ack;
		struct { uint64_t seq; } putok;
		struct { const uint8_t *mailbox; uint64_t seq; } revoke;
		struct { uint8_t code; } err;
	};
};

struct wire_blob {
	/* hdr is a per-epoch random chain id on a GROUP blob and the ephemeral x25519 pk
	 * on a SEALED one; aux is the masked message index on a GROUP blob and random on a
	 * SEALED one. neither says which, and the ct is padded the same either way */
	const uint8_t *hdr, *nonce, *ct;
	uint32_t aux;
	size_t ct_n;
};

struct wire_body {
	uint8_t kind;
	uint16_t channel;
	uint64_t time;
	/* the message this one answers, TEXT only: sender pk then its index on that
	 * sender's chain, little-endian. NULL when this is not a reply */
	const uint8_t *reply;
	const uint8_t *payload;
	size_t payload_n;
};

/* one FILE body's payload. every chunk carries the name, so any chunk that arrives
 * first can open the transfer; the chunks before the last are all WIRE_FILE_CHUNK
 * bytes, so idx alone says where a chunk lands */
struct wire_file {
	const uint8_t *fid;             /* 16 bytes, drawn per file by the sender */
	const uint8_t *name, *bytes;
	uint32_t idx, total;
	uint8_t name_n;
	size_t bytes_n;
};

struct wire_rec {                       /* membership record */
	/* salt is not the group id: the id is group_id(owner, salt), so an id cannot be
	 * claimed by anyone but the holder of the owner key */
	const uint8_t *salt, *owner, *name, *members, *sig;
	uint32_t version;
	size_t name_n;
	uint8_t nchan;
	/* id is what a message body's channel field names: stable across renames and
	 * across a delete renumbering the list */
	struct { const uint8_t *p; uint8_t n; uint16_t id; } chan[WIRE_MAX_CHANNELS];
	uint16_t nmemb;                 /* members points at nmemb * 32 bytes */
	const uint8_t *signed_from; size_t signed_n;
};

struct wire_group_pt {                  /* the plaintext inside a GROUP blob */
	/* cid repeats the cleartext header and index is only here, so the signature binds both */
	const uint8_t *cid, *sender, *body, *sig;
	uint32_t index;
	size_t body_n;
	const uint8_t *signed_from; size_t signed_n;
};

struct wire_sealed_body {               /* the plaintext inside a SEALED blob */
	const uint8_t *sender, *to, *inner, *sig;   /* to is signed: no re-wrap to a third member */
	size_t inner_n;
	const uint8_t *signed_from; size_t signed_n;
};

struct wire_senderkey {                 /* one shape of sealed inner */
	const uint8_t *group, *cid, *ck, *hk;   /* hk masks the index of every blob on the chain */
	uint32_t index;
	struct wire_rec rec;
};

void wire_encode_len(uint8_t out[WIRE_HDR], size_t n);
/* the only parse of pre-aead socket bytes; min/max are the caller's noise bounds */
int wire_decode_len(const uint8_t b[WIRE_HDR], size_t min, size_t max, size_t *n);

int wire_encode_frame(uint8_t *out, size_t cap, size_t *len, const struct wire_frame *f);
int wire_decode_frame(const uint8_t *b, size_t n, struct wire_frame *f);

int wire_encode_blob(uint8_t *out, size_t cap, size_t *len, const struct wire_blob *b);
int wire_decode_blob(const uint8_t *b, size_t n, struct wire_blob *o);

/* the smallest of 256/1k/4k/16k/64k that holds need, 0 if none does. a GROUP and a
 * SEALED plaintext both pad to it, so blob sizes do not say which one this is */
size_t wire_pad(size_t need);
int wire_encode_group_pt(uint8_t *out, size_t cap, size_t *len, const struct wire_group_pt *p);
int wire_decode_group_pt(const uint8_t *b, size_t n, struct wire_group_pt *o);

int wire_encode_body(uint8_t *out, size_t cap, size_t *len, const struct wire_body *b);
int wire_decode_body(const uint8_t *b, size_t n, struct wire_body *o);

int wire_encode_file(uint8_t *out, size_t cap, size_t *len, const struct wire_file *f);
int wire_decode_file(const uint8_t *b, size_t n, struct wire_file *o);

int wire_encode_rec(uint8_t *out, size_t cap, size_t *len, const struct wire_rec *r);
int wire_decode_rec(const uint8_t *b, size_t n, struct wire_rec *o);

int wire_encode_sealed_body(uint8_t *out, size_t cap, size_t *len, const struct wire_sealed_body *s);
int wire_decode_sealed_body(const uint8_t *b, size_t n, struct wire_sealed_body *o);

struct wire_invite {
	char host[WIRE_MAX_HOST];       /* no ipv6 literals: ':' ends the host */
	char port[6];
	uint8_t spk[32];                /* server noise static */
	uint8_t token[32];
};

int wire_encode_senderkey(uint8_t *out, size_t cap, size_t *len, const struct wire_senderkey *s);
int wire_decode_senderkey(const uint8_t *b, size_t n, struct wire_senderkey *o);

/* out holds WIRE_INVITE_MAX; the decoder only accepts the canonical lowercase form */
int wire_encode_invite(char *out, size_t cap, const struct wire_invite *v);
int wire_decode_invite(const char *s, struct wire_invite *o);

void wire_hex(char *out, const uint8_t *b, size_t n);   /* writes 2n chars and a NUL */
int  wire_unhex(uint8_t *out, const char *s, size_t n); /* n bytes from 2n lowercase chars */

#endif
