#ifndef WHIMSY_H
#define WHIMSY_H

#include <stddef.h>
#include <stdint.h>

/* The whole surface a frontend gets. No key material crosses it, and every
 * string handed out has been through text_sanitize, so a frontend renders what
 * it is given and never parses message bytes itself.
 *
 * Pointers returned point into the open store and stay valid only until the next
 * call that writes it: an edit, a delete, a leave, an avatar set or a whimsy_poll
 * frees the record they came from. Copy what you keep. Group and message indices
 * only ever grow, so an index kept across a poll still names the same thing. */

#define WHIMSY_FP_LEN   75      /* fingerprint text, NUL included */
#define WHIMSY_PK       32
#define WHIMSY_GID      16
#define WHIMSY_MAX_TEXT 8192    /* one message, bytes before sanitizing */
#define WHIMSY_MAX_PET  64      /* one petname, bytes after sanitizing */
#define WHIMSY_MAX_REACT 32     /* one reaction, bytes after sanitizing */
#define WHIMSY_MSGID    36      /* sender pk + its index on that sender's chain */
#define WHIMSY_MAX_LINKS 8      /* devices one key may declare; a decode bound, per sender */
#define WHIMSY_MAX_FILE 10403840 /* one file, bytes: 160 chunks of the wire's chunk size */
#define WHIMSY_HELD     4       /* files held in memory at once, oldest evicted */
#define WHIMSY_MAX_AVATAR 8192  /* one avatar, encoded bytes; the frontend encodes it */

/* whimsy_msg.kind. a FILE row's text is the sender's file name, sanitized and with
 * any directory stripped; the bytes are whimsy_file_open's, never the text */
#define WHIMSY_K_TEXT 1
#define WHIMSY_K_FILE 2

enum whimsy_err {
	WHIMSY_OK       =  0,
	WHIMSY_EIO      = -1,
	WHIMSY_EKEY     = -2,        /* wrong passphrase or keyfile */
	WHIMSY_EFORMAT  = -3,
	WHIMSY_ECORRUPT = -4,
	WHIMSY_ENOMEM   = -5,
	WHIMSY_ENET     = -6,        /* offline, or the connection died */
	WHIMSY_EPROTO   = -7,        /* the server or a peer said something wrong */
	WHIMSY_EARG     = -8,
	WHIMSY_ESTATE   = -9         /* no server saved, not the owner, not a member */
};

struct whimsy;

struct whimsy_msg {
	const uint8_t *sender;          /* WHIMSY_PK bytes */
	const char *text;               /* sanitized utf-8, not NUL terminated */
	size_t text_n;
	uint64_t time;                  /* unix seconds, off the sender's clock */
	uint16_t channel;               /* channel id, see whimsy_channel_id */
	uint8_t kind;
	int mine;
	int blocked;                    /* the sender is blocked: the text is stored, not shown */
	int edited;                     /* the sender replaced the text; the old one is gone */
	/* WHIMSY_MSGID bytes each. id is what whimsy_send_reply takes; it is NULL on a
	 * message stored before replies existed, which nothing can answer. reply is the
	 * message this one answers, NULL when it answers none */
	const uint8_t *id;
	const uint8_t *reply;
};

/* dir is created 0700 if missing; pass NULL uses <dir>/key. A store with no
 * identity gets a fresh one. *lost (when not NULL) is set to 1 when anything was
 * dropped -- a torn tail, or a record this build cannot read -- for the frontend to
 * tell the user history was lost. Reported once: the open marks what it dropped, so
 * running an older build again does not repeat the same loss. Only an unreadable identity fails the open: a build
 * that no longer understands a record kind loses those records, never the store.
 * see store_open in core/store.h */
int  whimsy_open(struct whimsy **out, const char *dir, const char *pass, int *lost);
void whimsy_close(struct whimsy *w);
/* change what the store is encrypted under, keeping every record. WHIMSY_EKEY when
 * oldpass is not the current one; an empty newpass (NULL) means the keyfile.
 * see store_rekey in core/store.h */
int  whimsy_rekey(struct whimsy *w, const char *oldpass, const char *newpass);
const char *whimsy_strerror(int e);

/* the same sanitizer every string out of this header has been through, for bytes a
 * frontend got elsewhere -- a text file it wants to preview. snprintf-style. */
size_t whimsy_sanitize(const void *in, size_t n, void *out, size_t cap);

void whimsy_self(const struct whimsy *w, uint8_t out[WHIMSY_PK]);
/* the pubkey itself in hex, grouped: not a hash, so a fragment of it is a prefix
 * of what whimsy_pk_hex gives */
void whimsy_fingerprint(char out[WHIMSY_FP_LEN], const uint8_t pk[WHIMSY_PK]);
void whimsy_pk_hex(char out[2 * WHIMSY_PK + 1], const uint8_t pk[WHIMSY_PK]);
int  whimsy_pk_parse(uint8_t out[WHIMSY_PK], const char *hex);

/* Petnames are local: never sent, never seen by the server or by any member, and
 * so unspoofable. name NULL or empty clears one. */
int whimsy_set_petname(struct whimsy *w, const uint8_t pk[WHIMSY_PK], const char *name);
/* the petname when one is set, else a linked key's petname, else the first group of
 * the fingerprint, so a frontend can render this alone and never branch. snprintf-style. */
size_t whimsy_petname(const struct whimsy *w, const uint8_t pk[WHIMSY_PK], char *out, size_t cap);

/* Device links. Two keys are one person once each has declared the other, so both
 * halves are consent; nothing about the pair travels but the two declarations, and no
 * name does -- the petname stays local and covers every key linked to the one it names.
 * A frontend merges linked members into one row and still attributes each message to
 * the key that signed it. */
/* Out-of-band verification: a record that this user compared pk's fingerprint with
 * its owner somewhere else. Local, never sent, and final -- there is no unverify. A
 * linked device is its own key and is verified on its own. */
int whimsy_verify(struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
int whimsy_verified(const struct whimsy *w, const uint8_t pk[WHIMSY_PK]);

/* Blocking is inbound only, local, and never sent. A blocked key's messages still
 * arrive and are still stored -- whimsy_msg marks them blocked and a frontend draws a
 * placeholder -- so unblocking shows them again with no re-fetch. Everything else the
 * key sends is dropped: files, edits, deletes, reactions, typing, links, purges, unread
 * counts and events. Their heal ask is the one exception and is answered like anyone's,
 * so a block makes no outbound difference a blocked peer could measure.
 * An add from a blocked owner is refused; a group already joined is not
 * touched. It covers every device linked with pk, resolved at the time of the check.
 * Outbound is not blocking and is not offered: one blob goes to every mailbox on one
 * chain, so skipping a mailbox only suppresses delivery -- any member can relay the
 * bytes and the blocked member's chain still opens them. Only an owner kick stops
 * someone reading you. A DM has no third party, so nothing is sent there. */
int whimsy_block(struct whimsy *w, const uint8_t pk[WHIMSY_PK], int on);
int whimsy_blocked(const struct whimsy *w, const uint8_t pk[WHIMSY_PK]);

int whimsy_link(struct whimsy *w, const uint8_t pk[WHIMSY_PK]);
/* keys mutually linked with pk, at most cap of them, WHIMSY_PK bytes each; returns
 * how many were written */
size_t whimsy_links(const struct whimsy *w, const uint8_t pk[WHIMSY_PK],
                    uint8_t *out, size_t cap);

/* invite is a whimsy:// url; NULL redials the server the store remembers */
int whimsy_connect(struct whimsy *w, const char *invite);
int whimsy_online(const struct whimsy *w);
/* messages built but not yet handed to the server; they go out oldest first on the
 * next whimsy_poll, so a frontend can say how many are still waiting */
size_t whimsy_pending(const struct whimsy *w);

size_t whimsy_group_count(const struct whimsy *w);
const uint8_t *whimsy_group_id(const struct whimsy *w, size_t g);
/* snprintf-style: writes what fits, returns the length the sanitized name needs */
size_t whimsy_group_name(const struct whimsy *w, size_t g, char *out, size_t cap);
size_t whimsy_channel_count(const struct whimsy *w, size_t g);
size_t whimsy_channel_name(const struct whimsy *w, size_t g, size_t c, char *out, size_t cap);
/* the id a message's channel field carries for row c; 0 when c is out of range.
 * ids survive a rename and a delete renumbering the rows */
uint16_t whimsy_channel_id(const struct whimsy *w, size_t g, size_t c);
size_t whimsy_member_count(const struct whimsy *w, size_t g);
const uint8_t *whimsy_member(const struct whimsy *w, size_t g, size_t i);
int whimsy_is_owner(const struct whimsy *w, size_t g);

int whimsy_group_new(struct whimsy *w, const char *name,
                     const char *const *channels, uint8_t nchan);
/* owner only; rotates every chain and seals a fresh one to every member */
int whimsy_group_add(struct whimsy *w, size_t g, const uint8_t pk[WHIMSY_PK]);
int whimsy_group_kick(struct whimsy *w, size_t g, const uint8_t pk[WHIMSY_PK]);
/* owner only; an empty name is legal, a DM has one */
int whimsy_group_rename(struct whimsy *w, size_t g, const char *name);
/* the owner kicks everyone first, then the group and its history go from this store.
 * indices above g shift down. nothing is told to the relay: a member sees it stop */
int whimsy_group_leave(struct whimsy *w, size_t g);
/* owner only; op 'a' adds channel a, 'd' drops it, 'r' renames it to b. indices
 * shift when a channel goes or is renamed, so a frontend re-reads names after this */
int whimsy_channel(struct whimsy *w, size_t g, char op, const char *a, const char *b);

/* WHIMSY_OK once the message is in local history: offline, or with anything still
 * queued ahead of it, it is delivered by a later whimsy_poll and never reordered */
int whimsy_send(struct whimsy *w, size_t g, uint16_t channel, const char *text, size_t n);
/* the same, answering the message whose id is to (WHIMSY_MSGID bytes). one hop, no
 * thread: the answered message may itself be a reply and nothing chains them. an id
 * naming a message this client never saw still sends -- the frontend shows the quote
 * only when it holds the message */
int whimsy_send_reply(struct whimsy *w, size_t g, uint16_t channel,
                      const uint8_t *to, const char *text, size_t n);
/* channel is an id from whimsy_channel_id, not a row: an unknown one is WHIMSY_EARG */

/* both name a message of ours by its index and reach every member. an edit replaces
 * the text and the old one leaves the disk. a delete is for everyone: the record and
 * its row go, here and on every member's device, and nothing marks that they were there.
 * WHIMSY_ESTATE when the message is not ours, or is too old to carry an id. */
int whimsy_edit(struct whimsy *w, size_t g, size_t i, const char *text, size_t n);
int whimsy_delete(struct whimsy *w, size_t g, size_t i);
/* the newest n messages of ours in the group, oldest of them deleted first. channel is
 * an id from whimsy_channel_id, or 0 for every channel; n past how many we have takes
 * them all. Same reach as whimsy_delete, one DELETE per message and one store rewrite. */
int whimsy_purge(struct whimsy *w, size_t g, uint16_t channel, size_t n);
/* Owner only: the oldest n messages in the channel, everyone's, gone for every member.
 * What travels is the timestamp of the n-th oldest row here, not the count, so a member
 * whose rows arrived in another order still drops the same set. channel is an id from
 * whimsy_channel_id and is required. WHIMSY_ESTATE when we are not the owner. */
int whimsy_purge_all(struct whimsy *w, size_t g, uint16_t channel, size_t n);

/* Reactions ride the group chain like any message and reach every member. The latest
 * one from a sender replaces theirs, an empty one clears it. A reaction is not a
 * message: no row, no unread, no event, and one on a message this client never
 * received is kept in case it arrives later. */
/* emoji NULL or n == 0 clears ours. id is WHIMSY_MSGID bytes, from whimsy_msg. text
 * that sanitizes to nothing, to more than one grapheme, or to over WHIMSY_MAX_REACT
 * bytes is WHIMSY_EARG. */
int whimsy_react(struct whimsy *w, size_t g, const uint8_t *id, const char *emoji, size_t n);
struct whimsy_react {
	const uint8_t *sender;          /* WHIMSY_PK bytes */
	const char *text;               /* sanitized utf-8, not NUL terminated */
	size_t text_n;
};
/* one entry per sender with a live reaction on message i, in the order they arrived;
 * the frontend counts them itself. snprintf-style. entries are valid until the next
 * whimsy_react or whimsy_poll. */
size_t whimsy_reactions(const struct whimsy *w, size_t g, size_t i,
                        struct whimsy_react *out, size_t cap);

/* local and final: any message, ours or not, leaves this device's store. nothing is
 * sent, and a copy still on the relay comes back. */
int whimsy_drop(struct whimsy *w, size_t g, size_t i);

/* Self-purge, local and final. Deletes every message of ours everywhere it can reach,
 * leaves every group, drops every petname, link, verification, avatar and sent mark,
 * and revokes every blob the relay still holds unfetched. What a member already
 * fetched is theirs and this cannot touch it. The identity, the server and the
 * settings survive, so the same key opens the same store afterwards; nothing else
 * does. Best effort: offline it still erases, and returns the first error. */
int whimsy_nuke(struct whimsy *w);

size_t whimsy_msg_count(const struct whimsy *w, size_t g);
/* Unread: a per-group-and-channel high-water mark of the last message index the user
 * looked at. Local, never sent, never a message. whimsy_seen only ever moves it
 * forward; channel is an id from whimsy_channel_id. Own messages never count unread. */
int    whimsy_seen(struct whimsy *w, size_t g, uint16_t channel, size_t i);
size_t whimsy_unread(const struct whimsy *w, size_t g, uint16_t channel);
/* index of the first unread message in the channel, or whimsy_msg_count when none */
size_t whimsy_first_unread(const struct whimsy *w, size_t g, uint16_t channel);
int whimsy_msg(const struct whimsy *w, size_t g, size_t i, struct whimsy_msg *m);
/* message indices in g whose sanitized text contains needle, oldest first, ascii
 * case-insensitive. snprintf-style: writes at most cap, returns how many matched. */
size_t whimsy_search(const struct whimsy *w, size_t g, const char *needle,
                     size_t *out, size_t cap);

/* Files. A file is chunked over the same group chain as any message and shows up as a
 * message row whose text is its name. Nothing is written to disk on its own: a received
 * file is assembled in memory and only leaves it when whimsy_file_save is called, or
 * when the file_autosave knob is on. Only WHIMSY_HELD files are held; an older one is
 * evicted, so a row can outlive its bytes. The held ones, complete or still arriving,
 * survive a restart: they are in the local store. */
int whimsy_send_file(struct whimsy *w, size_t g, uint16_t channel, const char *path);
/* the assembled bytes of message i, or NULL when it is not a file, is still arriving,
 * or is no longer held. *n is the length */
const uint8_t *whimsy_file_open(const struct whimsy *w, size_t g, size_t i, size_t *n);
/* writes those bytes to path, which must not exist. WHIMSY_ESTATE when nothing is held */
int whimsy_file_save(const struct whimsy *w, size_t g, size_t i, const char *path);
/* chunks done and chunks total for message i's transfer, both 0 when none is held.
 * done == total is a file whimsy_file_open will hand over */
void whimsy_file_progress(const struct whimsy *w, size_t g, size_t i,
                          size_t *done, size_t *total);

/* Typing indicators. The knob controls sending only: a client always renders one it
 * receives. Sending hands the server keystroke-rate timing, which is why it is asked
 * for rather than assumed. whimsy_typing is called while composing and rate-limits
 * itself; nothing is stored, here or on the receiver. */
int whimsy_typing(struct whimsy *w, size_t g, uint16_t channel);
/* who is composing in that channel right now, WHIMSY_PK bytes each, ourselves left
 * out; an entry ages out a few seconds after the last one received. snprintf-style */
size_t whimsy_typers(const struct whimsy *w, size_t g, uint16_t channel,
                     uint8_t *out, size_t cap);

/* Settings: keys are this enum, values are text. They live in the encrypted store,
 * are never sent, and default to whatever leaks least. A frontend lists every key
 * with whimsy_key_name and whimsy_get, and hands whimsy_set what the user typed. */
enum whimsy_key {
	WHIMSY_POLL_SECS,               /* seconds between fetch round trips, 1..3600 */
	WHIMSY_FILE_AUTOSAVE,           /* 1: a completed file also lands in <dir>/files */
	WHIMSY_TYPING_SEND,             /* 1: send them. 0: do not. empty: not asked yet */
	WHIMSY_NKEY
};
#define WHIMSY_MAX_VAL 64               /* one value, bytes, NUL not included */

const char *whimsy_key_name(int k);     /* NULL when k is out of range */
const char *whimsy_key_help(int k);
/* snprintf-style; the default until whimsy_set has stored something else */
size_t whimsy_get(const struct whimsy *w, int k, char *out, size_t cap);
int whimsy_set(struct whimsy *w, int k, const char *val);

/* What the last whimsy_poll delivered: one entry per group and channel that gained a
 * message from someone else, muted groups left out. The next poll clears them, so a
 * frontend reads this right after polling. snprintf-style. */
struct whimsy_event { size_t group; uint16_t channel; };
size_t whimsy_events(const struct whimsy *w, struct whimsy_event *out, size_t cap);
/* per group, local, never sent; whimsy_events honours it, unread counts do not */
int whimsy_mute(struct whimsy *w, size_t g, int on);
int whimsy_muted(const struct whimsy *w, size_t g);

/* one fetch round trip: stores and acks everything waiting. returns how many
 * blobs changed something, so > 0 means redraw. */
int whimsy_poll(struct whimsy *w);

/* Avatars, opt in, encoded by the frontend and never parsed here. A person's avatar is
 * sealed to one key and goes only to keys this user has verified out of band, at most
 * once per key: a fresh one, a new verification and a new membership record each send
 * what is owed on the next whimsy_poll. A group's avatar is the owner's, rides the group
 * chain, and is resent to every member when one is added. Empty bytes clear either.
 * The bytes handed back live until the next call that writes the store. */
int whimsy_avatar_set(struct whimsy *w, const void *bytes, size_t n);
const uint8_t *whimsy_avatar_get(const struct whimsy *w, const uint8_t pk[WHIMSY_PK], size_t *n);
int whimsy_group_avatar_set(struct whimsy *w, size_t g, const void *bytes, size_t n);
const uint8_t *whimsy_group_avatar_get(const struct whimsy *w, size_t g, size_t *n);

/* identity colour, one hue per key, constant lightness and chroma. needs -lm */
void whimsy_colour(const uint8_t pk[WHIMSY_PK], uint8_t rgb[3]);

#endif
