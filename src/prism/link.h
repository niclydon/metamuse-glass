/* prismd: Nexus Link v1 frames (portal/docs/NEXUS-LINK.md §1.2–§1.3), implemented
 * to nexusd/link/frames.py's letter: <compact JSON>\n<64 lowercase hex>, the hex
 * being HMAC-SHA256 over the exact JSON bytes with the 32 raw bytes of the link
 * key; decode order layout/size, signature, structure, op, version, sender, ts
 * window (hello exempt), replay. Verified against nexusd's shared vectors by
 * tests/test_vectors.c. */
#ifndef PRISM_LINK_H
#define PRISM_LINK_H
#include <stddef.h>
#include <stdint.h>
#include "json.h"

#define LINK_MAX_FRAME 8192
#define LINK_TS_WINDOW_MS 60000
#define LINK_RING_IDS 256
#define LINK_VERSION 1

typedef enum {
    LINK_OK = 0,
    LINK_E_FORMAT,     /* layout, size, JSON, structure, unknown op */
    LINK_E_SIG,        /* signature mismatch */
    LINK_E_TS,         /* outside the 60 s window */
    LINK_E_REPLAY,     /* id already seen */
    LINK_E_FORGED_OP,  /* approval.*, or an op that belongs to the other direction */
    LINK_E_VERSION     /* v != 1 */
} link_err;

const char *link_err_name(link_err e);

/* Which way the frame travels: from a node (accept NODE_OPS) or from the host. */
typedef enum { LINK_FROM_NODE, LINK_FROM_HOST } link_dir;

typedef struct {
    char ids[LINK_RING_IDS][65];
    int next, count;
} link_ring;

void link_ring_init(link_ring *r);
int link_ring_has(const link_ring *r, const char *id);
void link_ring_add(link_ring *r, const char *id);

/* link_key = HMAC-SHA256(key = device token, msg = "link-key-v1"); the 32 raw bytes
 * are the signing key (nexusd signs with bytes.fromhex(link_key)). */
void link_derive_key(const char *token, size_t tlen, uint8_t key[32]);
void link_sign(const uint8_t key[32], const char *head, size_t hlen, char sig[65]);
/* head + "\n" + sig into out; returns the text length, or -1 when it would exceed
 * LINK_MAX_FRAME or cap. */
int link_encode(const uint8_t key[32], const char *head, size_t hlen, char *out, size_t cap);
/* Verify one received frame. On LINK_OK *frame holds the parsed object (caller frees
 * with json_free) and its id is recorded in ring. peer may be NULL (any sender).
 * now_ms < 0 means "this node has no clock yet": the ts window is not applied. prismd
 * uses that for exactly one frame, the welcome, whose freshness is proven by the echo
 * of the node's own nonce instead; after it, time_ms from the welcome sets the clock. */
link_err link_decode(const char *text, size_t len, const uint8_t key[32], int64_t now_ms,
                     link_ring *ring, const char *peer, link_dir dir, jnode **frame);
#endif
