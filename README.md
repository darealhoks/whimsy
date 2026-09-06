# whimsy

Invite-only e2e messenger.

![](screenshot.png)

Only has groups with channels; a DM is a group of two. The server is a dumb relay, it holds
ciphertext for 30 days and knows nothing else. Nobody joins without an invite minted by
whoever runs that server.

C11, Monocypher vendored, no other dependency in the core. The GUI wants SDL3 and FreeType.

## Get started

    make                       dev build, -Werror + ASan/UBSan   -> build/dev/
    make MODE=release          -> build/release/

You get `libwhimsy.a`, `whimsyd` (the relay), and `whimsy` (the client, if pkg-config
finds sdl3 and freetype2).

Run a relay and mint yourself an invite:

    whimsyd serve  ~/.local/share/whimsyd 7717
    whimsyd invite ~/.local/share/whimsyd myhost:7717

That prints one single-use url. Start `whimsy`, paste it, pick a passphrase. Then `:help`.

## Who knows what

The server sees: ciphertext, mailbox ids, sizes, timing, which account sent, and that one
blob went to several mailboxes.

The server never sees: group ids, sender keys, group state, names

Your disk: always encrypted, argon2id passphrase or a 0600 keyfile (used if you don't put in a password, might add hardware key support later)

Not attempted: hiding your IP or your timing from the server, run it over tor yourself if you really need allat.
No device key sync either; a second device is a second identity, linked to the first.

## The parts

    vendor/monocypher/   crypto, verbatim, not edited
    core/                libwhimsy.a: the only code that touches keys
      wire.c             frame and blob encode/decode
      noise.c net.c      Noise_IK handshake, tcp, framing
      identity.c         seed, keys, fingerprint
      group.c            membership records, sender chains, message crypto
      store.c            encrypted append-only record file
      text.c             utf-8 validate, strip controls, cell width
      whimsy*.c          the public api behind core/whimsy.h
    server/whimsyd.c     epoll relay: register, put, fetch, ack, revoke, invites, ttl sweep
    gui/                 the client: SDL3 window, freetype atlas, panes, commands, markdown

Four places parse untrusted bytes: `wire_decode`, `text_sanitize`, `store_read`, and
Monocypher. Nothing else parses input.

`core/` includes nothing from `server/` or `gui/`. Key material never crosses `whimsy.h`.

## AI disclosure

This is a fair warning, AI was used to help write this app, if you're not comfortable with that just don't use it. The app is designed to be as secure as possible, privacy leaks are extremely unlikely - however they're still a possibility (as with any app, with or without AI).

## No warranty, and your law is your problem

This is a hobby project given away for free. It comes with **no warranty of any kind** —
see `LICENSE`. It has not had an independent security audit. Do not bet anything on it that
you cannot afford to lose.

It is also cryptography. Writing, publishing, importing, exporting or *using* strong
encryption is restricted or illegal in some countries, and some places can compel you to
hand over a passphrase. Whether you may run this is on you, not on me.

## Licence

MIT, see `LICENSE`. [Monocypher](https://monocypher.org) 4.0.2 under its CC0 option,
`vendor/monocypher/LICENCE.md`; stb headers in `vendor/LICENSES`.
