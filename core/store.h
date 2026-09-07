#ifndef WHIMSY_STORE_H
#define WHIMSY_STORE_H

#include <stddef.h>
#include <stdint.h>

/* Trust boundary: <dir>/store is untrusted bytes until each record's aead opens.
 * Append-only; the whole thing is decrypted into RAM at open and stays there.
 *
 * STORE_VER gates the container only -- header layout, record framing, aead. Record
 * bodies are whimsy.c's, and it skips one it cannot read, so a new kind or a changed
 * body never needs a bump and never costs anyone their identity. Bump this only when
 * these bytes change shape. */

#define STORE_VER      6
#define STORE_HDR      84
#define STORE_MAX_REC  (65536 + 512)

/* argon2id floor, not a default: a header asking for less is refused */
#define STORE_ARGON2_BLOCKS 65536       /* 64 MiB */
#define STORE_ARGON2_PASSES 3
#define STORE_ARGON2_LANES  1
/* ceilings on the same three, read from an unauthenticated header */
#define STORE_ARGON2_MAX_BLOCKS 1048576 /* 1 GiB */
#define STORE_ARGON2_MAX_PASSES 16
#define STORE_ARGON2_MAX_LANES  64

enum store_err {
	STORE_OK       =  0,
	STORE_EIO      = -1,
	STORE_EFORMAT  = -2,    /* not a whimsy store, or argon2 params under the floor */
	STORE_EKEY     = -3,    /* wrong passphrase, wrong keyfile, or none given */
	STORE_ECORRUPT = -4,    /* fewer records than the header seals, or none opened */
	STORE_ENOMEM   = -5,
	STORE_EBIG     = -6     /* append: record over STORE_MAX_REC */
};

enum store_kind { STORE_IDENTITY = 1, STORE_SERVER, STORE_GROUP, STORE_MSG, STORE_PETNAME,
                  STORE_OUTBOX, STORE_LINK, STORE_SETTING, STORE_SEEN,
                  STORE_VOID,     /* a record dropped in place; whimsy.c skips it */
                  STORE_VERIFY, STORE_MUTE, STORE_REACT, STORE_XFER,
                  STORE_AVATAR, STORE_GAVATAR, STORE_ASENT,
                  STORE_GONE,     /* a group left or kicked from; gid[16] | u64 version */
                  STORE_OUTBOX2,  /* an OUTBOX carrying the store index of its row */
                  STORE_BLOCK     /* a key whose messages are hidden; pk[32] | u8 on */ };

struct store;

/* dir is created 0700 if missing. pass NULL uses <dir>/key, created 0600 if missing.
 * a tail that does not open is dropped: *bad (when not NULL) is then set to how many
 * records survived, and the file is cut back to them by the next store_append, not
 * before, so a damaged store can still be read. that is only for bytes past the
 * record count the header seals; losing a sealed record is STORE_ECORRUPT. */
int store_open(struct store **out, const char *dir, const char *pass, size_t *bad);
void store_close(struct store *s);

int store_append(struct store *s, uint8_t kind, const void *rec, size_t n);

/* reseals every record under a key derived from newpass (NULL: the keyfile, created
 * if missing, and an existing <dir>/key is removed when moving to a passphrase).
 * STORE_EKEY when oldpass is not what the store is open under. all or nothing:
 * a failure before the new file is in place leaves the old password working. */
int store_rekey(struct store *s, const char *oldpass, const char *newpass);

/* replaces record i, then rewrites the whole file under the same key: the old bytes
 * leave the disk, which is the point -- an edit or a delete may not keep the old text.
 * the record index stays valid, so nothing above holds a stale one. */
int store_replace(struct store *s, size_t i, uint8_t kind, const void *rec, size_t n);

/* voids the n records idx names in one rewrite -- the cost of a single store_replace,
 * not n of them. duplicate indices and any order are fine; an index past the end is
 * STORE_EBIG and nothing is voided. all or nothing: a failure leaves every record as
 * it was and the old key working. record indices survive, as with store_replace. */
int store_void_many(struct store *s, const size_t *idx, size_t n);

size_t store_count(const struct store *s);
/* NULL when i is out of range; the bytes live until store_close, or until the next
 * store_replace or store_void_many, either of which frees the bodies it swaps out */
const uint8_t *store_get(const struct store *s, size_t i, uint8_t *kind, size_t *n);

#endif
