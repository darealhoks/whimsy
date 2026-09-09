#ifndef WHIMSY_INT_H
#define WHIMSY_INT_H

#include "whimsy.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "crypto.h"
#include "group.h"
#include "identity.h"
#include "net.h"
#include "store.h"
#include "text.h"
#include "wire.h"

/* whimsy.c and its whimsy_*.c siblings share this; nothing outside core/ includes it */

struct idxv { size_t *p, n, cap; };

_Static_assert(WHIMSY_FP_LEN == ID_FP_LEN, "fingerprint length must track identity.h");

/* store record bodies, all little-endian. the store itself carries only kind + bytes.
 *   IDENTITY  seed[32]
 *   SERVER    u8 host_n | host | u8 port_n | port | server noise pk[32]
 *   GROUP     u16 rec_n | membership record | send chain | u8 nrecv | recv chains
 *             a chain is pk[32] | cid[32] | ck[32] | hk[32] | u32 base | seen[GROUP_SKIP/8]
 *   MSG       group[16] | time u64 | channel u16 | kind u8 | sender[32] | sanitized text
 *             kind bit 7 set: index u32 | reply id[36] (zeroed when not a reply) sit between
 *             sender and the text. clear on records written before replies existed, which
 *             carry no message id and so cannot be replied to. bit 6: the text is an edit's,
 *             bit 5: an old store's tombstone; such a record is not loaded
 *   VOID      nothing; a record dropped in place by whimsy_drop
 *   PETNAME   pk[32] | sanitized label; the newest one for a pk wins, empty clears
 *   REACT     group[16] | msgid[36] | sender[32] | sanitized text; the newest one for a
 *             group, message and sender wins,
 *             empty clears. append only: a reaction is not the message it names
 *   OUTBOX    group[16] | blob, or a bare u64 = the store index of an OUTBOX record delivered
 *   SETTING   u8 key | value text; the newest one for a key wins
 *   SEEN      gid[16] | channel[2] | high-water[8], the newest one for a pair wins
 *   VERIFY    pk[32]; a fingerprint compared out of band, presence is the whole record
 *   MUTE      gid[16] | u8 on, the newest one for a group wins
 *   LINK      who[32] | other[32] | u8 on, one direction of a device link, authenticated
 *             when it was stored: ours, or one a GROUP blob's signature bound to who.
 *             the newest one for a pair wins; without the byte it is an old record, an on
 *   AVATAR    pk[32] | encoded bytes; the newest one for a pk wins, empty clears. the
 *             previous one is voided, so an avatar replaced leaves the disk
 *   GAVATAR   gid[16] | encoded bytes, same rule; the group's, set by its owner
 *   ASENT     pk[32]; our current avatar has gone to that key. presence is the whole
 *             record, and setting a new avatar voids the lot
 *   GONE      gid[16] | u64 version; a group we left or were kicked from, at the record
 *             version we left at. a senderkey no newer than it is acked away, a newer
 *             one clears it and rejoins
 * a GROUP record is the whole group state; the newest one for an id wins. */
#define OUTHDR  (WHIMSY_GID + 8)                /* STORE_OUTBOX2: gid | u64 row store index */
#define MSGHDR  (WHIMSY_GID + 8 + 2 + 1 + WHIMSY_PK)
#define MSGEXT  (MSGHDR + 4 + WHIMSY_MSGID)     /* header of a record with kind bit 7 set */
/* flags in the record's kind byte, never on the wire. the wire kind is the low bits */
#define XFERHDR (WHIMSY_GID + WHIMSY_MSGID + 24)  /* gid | msgid | fid | total | idx */

#define MSGIDX  0x80                            /* the index + reply id header is present */
#define MSGEDIT 0x40                            /* the sender replaced the text */
#define MSGDEL  0x20                            /* only in a store written before deletes voided */
#define MSGFLAG (MSGIDX | MSGEDIT | MSGDEL)
/* the kind shares the byte with those flags: a wire kind reaching MSGDEL would make
 * every record of that kind read as an old tombstone and vanish at the next open */
_Static_assert(WIRE_K_UNLINK < MSGDEL, "a wire kind past MSGDEL reads as an old tombstone");
_Static_assert(WHIMSY_MSGID == WIRE_MSGID, "message id must be the wire's");
_Static_assert(WHIMSY_MAX_REACT == WIRE_MAX_REACT, "the reaction cap must be the wire's");
_Static_assert(WHIMSY_K_TEXT == WIRE_K_TEXT && WHIMSY_K_FILE == WIRE_K_FILE,
               "whimsy_msg.kind is the wire kind");
_Static_assert(WHIMSY_MAX_FILE == (long)WIRE_FILE_CHUNK * WIRE_FILE_CHUNKS,
               "the file cap is the wire's chunking");
#define CHAIN_ENC (4 * 32 + 4 + GROUP_SKIP / 8)

/* the encoded chain size is part of the store format: move it and every existing store
 * loses its groups and history at the next open */
_Static_assert(CHAIN_ENC == 196, "chain encoding changed: every existing store loses its groups");
/* not a format, a tripwire: a field added to the struct would otherwise go unencoded */
_Static_assert(sizeof(struct group_chain) == 196, "group_chain gained a field: encode it in put_chain");

struct grp {
	struct group g;
	size_t *msg;                    /* store indices, in arrival order */
	size_t nmsg, mcap;
	int stale;                      /* our chain rotated and is not sealed to the members yet */
	int bad;                        /* a GROUP record for it would not parse; dropped at the end of load */
	int relink;                     /* republish our link halves here once the fetch is done */
	int heal;                       /* a chain we cannot agree on: ask once the fetch is done */
	int seal;                       /* answer a HEAL: re-seal the chain we hold, which did not rotate */
	uint8_t healpk[WHIMSY_PK];      /* whose chain to forget before asking */
	time_t asked, healed;           /* last heal sent from here, last one acted on for a member */
	struct seenslot { uint16_t chan; uint64_t hw; size_t rec; } seen[WIRE_MAX_CHANNELS];
	size_t nseen;
	int notify;                     /* enum whimsy_notify */
	uint16_t evt[WIRE_MAX_CHANNELS];        /* channels that gained a message this poll */
	uint8_t ment[WIRE_MAX_CHANNELS];        /* one of those messages named us */
	size_t nevt;
	uint16_t typed_ch;              /* the channel typed_at belongs to: a switch resets the limit */
	time_t typed_at;                /* last typing blob sent from here */
};

/* a built blob still owed to the group's mailboxes; b is the wire bytes verbatim */
struct out {
	uint64_t id;                    /* store index of the record that queued it */
	size_t mrec;                    /* store index of the MSG row this blob carries, else SIZE_MAX */
	uint8_t gid[WHIMSY_GID];
	uint8_t *b;
	size_t n;
};

/* one blob the relay holds for one mailbox, named by the row that built it */
struct spool { size_t mrec; uint64_t seq; uint8_t mb[WHIMSY_PK]; int dead; };

/* a file being assembled, or one of ours held for re-opening. every chunk is also a
 * STORE_XFER record, so a restart mid-transfer does not lose what the server already
 * dropped on our ack. only the WHIMSY_HELD newest survive an open; prune_xfers voids
 * the rest */
struct xfer {
	uint8_t gid[WHIMSY_GID];
	uint8_t fid[16];
	uint8_t sender[WHIMSY_PK];
	size_t row;                     /* the message index in that group */
	uint32_t midx;                  /* the sender's chain index of that row; with sender, its msgid */
	uint32_t total, got;
	uint8_t seen[(WIRE_FILE_CHUNKS + 7) / 8];
	uint8_t *b;
	size_t n;                       /* bytes assembled, by the highest chunk seen */
	uint64_t *out;                  /* ours: the outbox id of each chunk; progress reads it */
};

struct pet {
	uint8_t pk[WHIMSY_PK];
	uint8_t n;
	char t[WHIMSY_MAX_PET];
};

/* one sender's live reaction on one message. the target need not be here: a reaction
 * that outran its message is kept so it shows when the message lands */
struct react {
	uint8_t gid[WHIMSY_GID], id[WHIMSY_MSGID], pk[WHIMSY_PK];
	uint8_t n;
	char t[WHIMSY_MAX_REACT];
};

#define REACTIONS    1024      /* live reactions held at once; a new one past this is dropped */
#define TYPERS       16         /* people composing tracked at once, oldest overwritten */
#define SPOOL_MAX   256         /* blobs the relay may still hold for us, per mailbox; oldest forgotten */
#define HOLDS        64         /* undecodable blobs awaited at once; past it we ack and lose them */
#define HOLD_TRIES    3         /* polls a blob is left on the server before we give up on it */
#define TYPING_EVERY 3          /* seconds between blobs from us, per group and channel */
#define TYPING_TTL   6          /* one goes quiet this long after the last one received */

/* someone composing, seen and never stored. one entry per group, channel and sender */
struct typer {
	uint8_t gid[WHIMSY_GID], pk[WHIMSY_PK];
	uint16_t chan;
	time_t at;
};

/* one direction: who says other is a device of theirs. a link holds when both are here */
struct link {
	uint8_t who[WHIMSY_PK], other[WHIMSY_PK];
};

/* the live avatar for one key: a pk when grp is 0, a group id when it is 1. rec is the
 * store record, so the bytes themselves are the store's and are never copied */
struct av {
	uint8_t key[WHIMSY_PK];
	size_t rec;
	int grp;
};

struct whimsy {
	struct store *st;
	char set[WHIMSY_NKEY][WHIMSY_MAX_VAL + 1];      /* empty: the default stands */
	struct identity id;
	struct net *n;
	struct grp **g;
	size_t ng, gcap;
	struct pet *pet;
	size_t npet, pcap;
	struct react *rea;
	size_t nrea, rcap;
	struct link *lnk;
	size_t nlnk, lcap;
	struct typer typ[TYPERS];
	size_t ntyp;
	struct { uint64_t seq; uint8_t tries; } hold[HOLDS];
	size_t nhold;
	uint8_t (*ver)[WHIMSY_PK];      /* keys verified out of band */
	size_t nver, vcap;
	uint8_t (*blk)[WHIMSY_PK];      /* keys blocked; inbound only, never sent */
	size_t nblk, bcap;
	struct av *av;
	size_t nav, avcap;
	uint8_t (*sent)[WHIMSY_PK];     /* keys our current avatar has reached */
	size_t nsent, scap;
	struct out *out;
	size_t nout, ocap;
	struct spool spool[SPOOL_MAX];
	size_t nspool;
	int instream;                   /* inside the fetch stream: no request may be sent */
	struct xfer *held[WHIMSY_HELD];
	size_t nheld;
	char *dir;                      /* what whimsy_open was given; only file_autosave reads it */
	char host[WIRE_MAX_HOST], port[6];
	uint8_t spk[32];
	int registered;                 /* the store holds a SERVER record: never REGISTER again */
	time_t redial;                  /* no dial before this while offline */
	uint8_t rbuf[NET_BUF];
	uint8_t blob[GROUP_MAX_BLOB];
	uint8_t scratch[GROUP_MAX_PTB];
	uint8_t pt[GROUP_MAX_PT];
};


static inline uint16_t ld16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void st16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline uint32_t ld32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline int zeroed(const uint8_t *p, size_t n)
{
	uint8_t v = 0;
	for (size_t i = 0; i < n; i++) v |= p[i];
	return !v;
}

static inline void st32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint64_t ld64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
	return v;
}
static inline void st64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}


struct grp *add_grp(struct whimsy *w);
int add_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
                    const uint8_t other[WHIMSY_PK]);
int add_msg(struct whimsy *w, struct grp *gr, const uint8_t sender[32], uint64_t t,
                   uint16_t channel, uint8_t kind, uint32_t index, const uint8_t *reply,
                   const void *text, size_t n);
int add_ver(struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
void del_ver(struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
int apply_change(struct whimsy *w, struct grp *gr, const uint8_t sender[32],
                        const uint8_t *id, const void *text, size_t n, int del);
int apply_purge(struct whimsy *w, struct grp *gr, uint16_t channel, uint64_t before);
int av_load(struct whimsy *w, const uint8_t *key, size_t kn, int grp, size_t rec);
int av_set(struct whimsy *w, const uint8_t *key, size_t kn, int grp,
                  const void *b, size_t n);
int blk_set(struct whimsy *w, const uint8_t pk[WHIMSY_PK], int on);
int chan_known(const struct grp *gr, uint16_t id);
void clear_typer(struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                        const uint8_t pk[WHIMSY_PK]);
int dead_xfers(struct whimsy *w, struct idxv *v);
int declared(const struct whimsy *w, const uint8_t who[WHIMSY_PK],
                    const uint8_t other[WHIMSY_PK]);
void drop_grp(struct whimsy *w, size_t i);
void drop_last(struct whimsy *w);
void drop_out(struct whimsy *w, uint64_t id);
int drop_row(struct whimsy *w, struct grp *gr, size_t i);
int drop_row_v(struct whimsy *w, struct grp *gr, size_t i, struct idxv *v);
int emit_build(struct whimsy *w, struct grp *gr, uint16_t channel, uint8_t kind,
                      uint64_t t, const uint8_t *reply, const void *p, size_t n, size_t *len);
int emit_out(struct whimsy *w, struct grp *gr, size_t len, size_t mrec);
int erase_grp(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const struct grp *skip);
int fan_out(struct whimsy *w, const struct grp *gr, const uint8_t *b, size_t n,
                   size_t mrec);
int file_recv(struct whimsy *w, struct grp *gr, const struct group_msg *m);
struct grp *find_grp(const struct whimsy *w, const uint8_t id[WHIMSY_GID]);
size_t find_msgid(const struct whimsy *w, const struct grp *gr, const uint8_t *id);
struct pet *find_pet(const struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
int flush_out(struct whimsy *w);
int64_t gone_ver(const struct whimsy *w, const uint8_t gid[WHIMSY_GID]);
int gone_write(struct whimsy *w, const uint8_t gid[WHIMSY_GID], uint64_t version);
int heal_ask(struct whimsy *w, struct grp *gr);
int idxv_add(struct idxv *v, size_t i);
int load_group(struct whimsy *w, const uint8_t *b, size_t n, int *named);
int map_group(int e);
int map_store(int e);
void note_event(struct grp *gr, uint16_t chan, int mention);
int names_us(const struct whimsy *w, const void *b, size_t n);
void note_typer(struct whimsy *w, const uint8_t gid[WHIMSY_GID], uint16_t chan,
                       const uint8_t pk[WHIMSY_PK]);
void prune_xfers(struct whimsy *w);
int publish_links(struct whimsy *w, struct grp *gr);
int revoke_link(struct whimsy *w, struct grp *gr, const uint8_t pk[WHIMSY_PK]);
int push_avatars(struct whimsy *w);
int push_msg(struct grp *gr, size_t idx);
int push_out(struct whimsy *w, uint64_t id, const uint8_t gid[WHIMSY_GID],
                    const uint8_t *b, size_t n, size_t mrec);
int queue_out(struct whimsy *w, const struct grp *gr, const uint8_t *b, size_t n,
                     size_t mrec);
int rcv(struct whimsy *w, struct wire_frame *f);
int reseal(struct whimsy *w, struct grp *gr);
int save_chunk(struct whimsy *w, const struct xfer *x, uint32_t idx,
                      const uint8_t *b, size_t n);
int save_group(struct whimsy *w, const struct grp *gr);
void del_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
              const uint8_t other[WHIMSY_PK]);
int drop_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
              const uint8_t other[WHIMSY_PK]);
int save_link(struct whimsy *w, const uint8_t who[WHIMSY_PK],
                     const uint8_t other[WHIMSY_PK]);
int save_react(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const uint8_t *id,
                      const uint8_t pk[WHIMSY_PK], const void *text, size_t n);
uint64_t seen_hw(const struct grp *gr, uint16_t chan);
struct seenslot *seen_slot(struct grp *gr, uint16_t chan);
int seen_write(struct whimsy *w, struct grp *gr, struct seenslot *sl,
                      uint64_t hw, struct idxv *v);
int sent_add(struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
int set_pet(struct whimsy *w, const uint8_t pk[WHIMSY_PK], const char *t, size_t n);
int set_react(struct whimsy *w, const uint8_t gid[WHIMSY_GID], const uint8_t *id,
                     const uint8_t pk[WHIMSY_PK], const char *t, size_t n);
int snd(struct whimsy *w, const struct wire_frame *f);
void spool_flush(struct whimsy *w);
size_t term(char *out, size_t cap, size_t w);

int map_store(int e);
int void_batch(struct whimsy *w, struct idxv *v);
struct xfer *xfer_add(struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                             const uint8_t fid[16], const uint8_t sender[WHIMSY_PK],
                             uint32_t midx, size_t row, uint32_t total);
struct xfer *xfer_find(const struct whimsy *w, const uint8_t gid[WHIMSY_GID],
                              const uint8_t fid[16], const uint8_t sender[WHIMSY_PK]);
void xfer_free(struct xfer *x);
struct xfer *xfer_row(const struct whimsy *w, const uint8_t gid[WHIMSY_GID], size_t row);

int send_gavatar(struct whimsy *w, struct grp *gr);

#define HEAL_EVERY 60   /* seconds, per group: an ask is remote-triggerable work for the owner */

#endif
