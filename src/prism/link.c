#include "link.h"
#include <string.h>
#include "crypto.h"

static const char *const NODE_OPS[] = {"hello", "ready", "presence", "input.intent", "input.pointer",
                                       "card.action", "event", "pong", NULL};
static const char *const HOST_OPS[] = {"welcome", "state.patch", "card.push", "card.clear", "mode.set",
                                       "ping", NULL};

const char *link_err_name(link_err e) {
    switch (e) {
    case LINK_OK: return "ok";
    case LINK_E_FORMAT: return "format";
    case LINK_E_SIG: return "sig";
    case LINK_E_TS: return "ts";
    case LINK_E_REPLAY: return "replay";
    case LINK_E_FORGED_OP: return "forged_op";
    case LINK_E_VERSION: return "version";
    }
    return "?";
}

void link_ring_init(link_ring *r) { memset(r, 0, sizeof *r); }

int link_ring_has(const link_ring *r, const char *id) {
    for (int i = 0; i < r->count; i++)
        if (!strcmp(r->ids[i], id)) return 1;
    return 0;
}

void link_ring_add(link_ring *r, const char *id) {
    strncpy(r->ids[r->next], id, 64);
    r->ids[r->next][64] = 0;
    r->next = (r->next + 1) % LINK_RING_IDS;
    if (r->count < LINK_RING_IDS) r->count++;
}

void link_derive_key(const char *token, size_t tlen, uint8_t key[32]) {
    hmac_sha256(token, tlen, "link-key-v1", 11, key);
}

void link_sign(const uint8_t key[32], const char *head, size_t hlen, char sig[65]) {
    uint8_t mac[32];
    hmac_sha256(key, 32, head, hlen, mac);
    hex_encode(mac, 32, sig);
}

int link_encode(const uint8_t key[32], const char *head, size_t hlen, char *out, size_t cap) {
    if (hlen + 1 + 64 > LINK_MAX_FRAME || hlen + 1 + 64 + 1 > cap) return -1;
    memcpy(out, head, hlen);
    out[hlen] = '\n';
    link_sign(key, head, hlen, out + hlen + 1);
    return (int)(hlen + 65);
}

static int in_list(const char *const *list, const char *op) {
    for (; *list; list++)
        if (!strcmp(*list, op)) return 1;
    return 0;
}

static int valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t k;
        unsigned cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; if (cp < 2) return 0; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; }
        else return 0;
        if (i + k >= n + 0 && i + k > n - 1 + 1) return 0;
        if (i + k > n - 1 && i + k >= n) return 0;
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        i += k + 1;
    }
    return 1;
}

static int valid_id(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 64) return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

link_err link_decode(const char *text, size_t len, const uint8_t key[32], int64_t now_ms,
                     link_ring *ring, const char *peer, link_dir dir, jnode **frame) {
    *frame = NULL;
    /* layout and size */
    if (len > LINK_MAX_FRAME) return LINK_E_FORMAT;
    if (!valid_utf8((const unsigned char *)text, len)) return LINK_E_FORMAT;
    const char *sep = NULL;
    for (size_t i = len; i > 0; i--)
        if (text[i - 1] == '\n') { sep = text + i - 1; break; }
    if (!sep || sep == text) return LINK_E_FORMAT;
    const char *sig = sep + 1;
    size_t siglen = (size_t)(text + len - sig);
    if (siglen != 64) return LINK_E_FORMAT;
    for (size_t i = 0; i < 64; i++) {
        char c = sig[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return LINK_E_FORMAT;
    }
    size_t hlen = (size_t)(sep - text);
    /* signature, before anything is parsed */
    char want[65];
    link_sign(key, text, hlen, want);
    if (!ct_equal(want, sig, 64)) return LINK_E_SIG;
    /* structure */
    jnode *f = json_parse(text, hlen);
    if (!f) return LINK_E_FORMAT;
    if (f->type != J_OBJ) { json_free(f); return LINK_E_FORMAT; }
    link_err err = LINK_E_FORMAT;
    const jnode *op = json_get(f, "op");
    if (!op || op->type != J_STR) goto out;
    const char *ops = op->v.s;
    if (!strcmp(ops, "approval") || !strncmp(ops, "approval.", 9)) { err = LINK_E_FORGED_OP; goto out; }
    const char *const *accepted = dir == LINK_FROM_NODE ? NODE_OPS : HOST_OPS;
    const char *const *other = dir == LINK_FROM_NODE ? HOST_OPS : NODE_OPS;
    if (!in_list(accepted, ops)) { err = in_list(other, ops) ? LINK_E_FORGED_OP : LINK_E_FORMAT; goto out; }
    const jnode *v = json_get(f, "v");
    if (!v || v->type != J_INT || v->v.i != LINK_VERSION) { err = LINK_E_VERSION; goto out; }
    const jnode *id = json_get(f, "id");
    if (!id || id->type != J_STR || !valid_id(id->v.s)) goto out;
    const jnode *ts = json_get(f, "ts");
    if (!ts || ts->type != J_INT || ts->v.i < 0) goto out;
    const jnode *from = json_get(f, "from");
    if (!from || from->type != J_STR || !from->v.s[0]) goto out;
    if (peer && strcmp(from->v.s, peer)) goto out;
    const jnode *body = json_get(f, "body");
    if (!body || body->type != J_OBJ) goto out;
    /* ts window (hello is exempt), then replay */
    if (strcmp(ops, "hello") != 0 && now_ms >= 0) {
        int64_t d = now_ms - ts->v.i;
        if (d > LINK_TS_WINDOW_MS || d < -LINK_TS_WINDOW_MS) { err = LINK_E_TS; goto out; }
    }
    /* Replay: the spec's 256-id ring (NEXUS-LINK §1.5, nexusd ReplayRing), after the 60 s
     * ts window above. The two together are the contract: a frame can only be replayed
     * within 60 s of its ts, and only once 256 newer ids have pushed it out of the ring.
     * prismd deliberately mirrors nexusd here rather than inventing a stronger scheme. */
    if (link_ring_has(ring, id->v.s)) { err = LINK_E_REPLAY; goto out; }
    link_ring_add(ring, id->v.s);
    *frame = f;
    return LINK_OK;
out:
    json_free(f);
    return err;
}
