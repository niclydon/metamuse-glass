#include "ws.h"
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "crypto.h"

#define WS_MAX_HDR 8192
static char last_err[160];
const char *ws_last_error(void) { return last_err; }
static int fail(const char *what) {
    snprintf(last_err, sizeof last_err, "%s: %s", what, errno ? strerror(errno) : "protocol");
    return -1;
}

static int wait_fd(int fd, int events, int timeout_ms) {
    struct pollfd p = {fd, (short)events, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return -2;
    if (r < 0) return -1;
    return 0;
}

static int write_all(ws_t *ws, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = send(ws->fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { if (wait_fd(ws->fd, POLLOUT, 5000)) return fail("send wait"); continue; }
            return fail("send");
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Ensure at least need bytes are buffered; -1 error, -2 timeout, 0 closed by peer. */
static int fill(ws_t *ws, size_t need, int timeout_ms) {
    while (ws->rlen < need) {
        if (need > WS_RBUF) { errno = 0; return fail("frame too large"); }
        int r = wait_fd(ws->fd, POLLIN, timeout_ms);
        if (r) return r == -2 ? -2 : fail("poll");
        ssize_t n = recv(ws->fd, ws->rbuf + ws->rlen, WS_RBUF - ws->rlen, 0);
        if (n == 0) return 0;
        if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; return fail("recv"); }
        ws->rlen += (size_t)n;
    }
    return 1;
}

static void consume(ws_t *ws, size_t n) {
    memmove(ws->rbuf, ws->rbuf + n, ws->rlen - n);
    ws->rlen -= n;
}

int ws_connect(ws_t *ws, const char *host, const char *port, const char *path, int timeout_ms) {
    memset(ws, 0, sizeof *ws);
    ws->fd = -1;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) { snprintf(last_err, sizeof last_err, "resolve %s: %s", host, gai_strerror(rc)); return -1; }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return fail("connect");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    ws->fd = fd;

    uint8_t nonce[16];
    char key_b64[25], accept_b64[29];
    if (random_bytes(nonce, 16)) { errno = 0; return fail("random"); }
    base64_encode(nonce, 16, key_b64);
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "User-Agent: prismd/0.1\r\n"
                     "\r\n",
                     path, host, port, key_b64);
    if (n <= 0 || (size_t)n >= sizeof req) { errno = 0; return fail("request"); }
    if (write_all(ws, (const uint8_t *)req, (size_t)n)) return -1;

    /* expected accept: base64(sha1(key + GUID)) */
    char cat[64];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key_b64);
    uint8_t dig[20];
    sha1(cat, strlen(cat), dig);
    base64_encode(dig, 20, accept_b64);

    /* read headers: parsed in place in rbuf; the header block is capped at WS_MAX_HDR
     * bytes and the search never runs past it, so no copy can overflow */
    size_t hdr_end = 0;
    for (;;) {
        size_t limit = ws->rlen < WS_MAX_HDR ? ws->rlen : WS_MAX_HDR;
        for (size_t i = 3; i < limit; i++)
            if (!memcmp(ws->rbuf + i - 3, "\r\n\r\n", 4)) { hdr_end = i + 1; break; }
        if (hdr_end) break;
        if (ws->rlen >= WS_MAX_HDR) { errno = 0; return fail("response headers too long"); }
        int r = fill(ws, ws->rlen + 1, timeout_ms);
        if (r <= 0) { if (r == -2) errno = ETIMEDOUT; else if (r == 0) errno = ECONNRESET; return fail("upgrade"); }
    }
    if (hdr_end > WS_MAX_HDR) { errno = 0; return fail("response headers too long"); }
    char hdr[WS_MAX_HDR + 1];
    memcpy(hdr, ws->rbuf, hdr_end);
    hdr[hdr_end] = 0;
    consume(ws, hdr_end);
    if (strncmp(hdr, "HTTP/1.1 101", 12) != 0) {
        char line[80];
        size_t l = strcspn(hdr, "\r\n");
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, hdr, l); line[l] = 0;
        snprintf(last_err, sizeof last_err, "upgrade refused: %s", line);
        return -1;
    }
    const char *acc = NULL;
    for (char *p = hdr; (p = strchr(p, '\n')) != NULL; p++) {
        if (!strncasecmp(p + 1, "Sec-WebSocket-Accept:", 21)) { acc = p + 22; break; }
    }
    if (!acc) { errno = 0; return fail("no Sec-WebSocket-Accept"); }
    while (*acc == ' ') acc++;
    if (strncmp(acc, accept_b64, 28) != 0) { errno = 0; return fail("Sec-WebSocket-Accept mismatch"); }
    return 0;
}

static int send_frame(ws_t *ws, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) hdr[h++] = (uint8_t)(0x80 | len);
    else if (len < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)len; }
    else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint8_t mask[4];
    if (random_bytes(mask, 4)) { errno = 0; return fail("random"); }
    memcpy(hdr + h, mask, 4); h += 4;
    if (write_all(ws, hdr, h)) return -1;
    uint8_t chunk[1024];
    for (size_t off = 0; off < len; off += sizeof chunk) {
        size_t n = len - off < sizeof chunk ? len - off : sizeof chunk;
        for (size_t i = 0; i < n; i++) chunk[i] = payload[off + i] ^ mask[(off + i) & 3];
        if (write_all(ws, chunk, n)) return -1;
    }
    return 0;
}

int ws_send_text(ws_t *ws, const char *text, size_t len) {
    if (ws->closed) { errno = 0; return fail("closed"); }
    return send_frame(ws, 0x1, (const uint8_t *)text, len);
}

int ws_recv_text(ws_t *ws, char *buf, size_t cap, int timeout_ms) {
    size_t out = 0;
    int in_message = 0;
    for (;;) {
        int r = fill(ws, 2, timeout_ms);
        if (r <= 0) return r == 1 ? -1 : (r == 0 ? (ws->closed = 1, 0) : r);
        uint8_t b0 = ws->rbuf[0], b1 = ws->rbuf[1];
        int fin = b0 & 0x80, opcode = b0 & 0x0F, masked = b1 & 0x80;
        uint64_t len = b1 & 0x7F;
        size_t h = 2;
        if (len == 126) {
            if ((r = fill(ws, 4, timeout_ms)) <= 0) return r == 1 ? -1 : (r == 0 ? (ws->closed = 1, 0) : r);
            len = (uint64_t)ws->rbuf[2] << 8 | ws->rbuf[3]; h = 4;
        } else if (len == 127) {
            if ((r = fill(ws, 10, timeout_ms)) <= 0) return r == 1 ? -1 : (r == 0 ? (ws->closed = 1, 0) : r);
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | ws->rbuf[2 + i];
            h = 10;
        }
        if (masked) h += 4;
        if (len > WS_RBUF - h) { errno = 0; return fail("frame too large"); }
        size_t total = h + (size_t)len;
        if ((r = fill(ws, total, timeout_ms)) <= 0) return r == 1 ? -1 : (r == 0 ? (ws->closed = 1, 0) : r);
        uint8_t *p = ws->rbuf + h;
        if (masked) {
            const uint8_t *m = ws->rbuf + h - 4;
            for (size_t i = 0; i < len; i++) p[i] ^= m[i & 3];
        }
        switch (opcode) {
        case 0x1: case 0x0: case 0x2:
            if (opcode == 0x1 || (opcode == 0x0 && in_message)) {
                if (out + len + 1 > cap) { errno = 0; consume(ws, total); return fail("message too large"); }
                memcpy(buf + out, p, len); out += (size_t)len;
                in_message = 1;
                consume(ws, total);
                if (fin) { buf[out] = 0; return (int)out; }
            } else consume(ws, total);  /* binary (or stray continuation): ignored */
            break;
        case 0x8: {
            ws->close_code = len >= 2 ? (uint16_t)(p[0] << 8 | p[1]) : 1005;
            consume(ws, total);
            ws->closed = 1;
            uint8_t code[2] = {(uint8_t)(1000 >> 8), (uint8_t)1000};
            send_frame(ws, 0x8, code, 2);
            return 0;
        }
        case 0x9: {
            uint8_t tmp[125];
            size_t n = len < 125 ? (size_t)len : 125;
            memcpy(tmp, p, n);
            consume(ws, total);
            if (send_frame(ws, 0xA, tmp, n)) return -1;
            break;
        }
        default:
            consume(ws, total);  /* pong and reserved opcodes */
            break;
        }
    }
}

void ws_close(ws_t *ws, uint16_t code) {
    if (ws->fd < 0) return;
    if (!ws->closed) {
        uint8_t c[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        send_frame(ws, 0x8, c, 2);
    }
    close(ws->fd);
    ws->fd = -1;
    ws->closed = 1;
}
