#ifndef WHIMSY_NET_H
#define WHIMSY_NET_H

#include <stddef.h>
#include <stdint.h>

#include "identity.h"
#include "noise.h"
#include "wire.h"

/* One tcp connection to one whimsyd, framed as u32 le length | noise message.
 * The length prefix is u32, not the u16 noise usually uses: a frame is larger
 * than 65535. Blocking i/o. */

#define NET_MAX (WIRE_FRAME_HDR + WIRE_MAX_PAYLOAD)     /* plaintext frame */
#define NET_TIMEOUT 5                                        /* seconds, connect and each read */
#define NET_BUF (NET_MAX + NOISE_TAG)                   /* what net_recv wants */

enum net_err {
	NET_OK    =  0,
	NET_EIO   = -1,
	NET_ECONN = -2,         /* resolve or connect failed */
	NET_EHS   = -3,         /* handshake refused: wrong server key, or not whimsyd */
	NET_EWIRE = -4,
	NET_EMEM  = -5
};

struct net;

int net_dial(struct net **out, const char *host, const char *port,
             const uint8_t server_pk[32], const struct identity *me);
void net_close(struct net *n);

int net_send(struct net *n, const struct wire_frame *f);
/* blocks for one whole frame. buf holds NET_BUF bytes and backs f's pointers */
int net_recv(struct net *n, struct wire_frame *f, uint8_t *buf, size_t cap);

#endif
