/* prismd: a minimal RFC 6455 client over plain TCP. Text frames in and out, client
 * masking, ping answered with pong, fragments reassembled, close handshake. No TLS:
 * Nexus Link runs on the Hearth LAN only (NEXUS-LINK §1.1). The upgrade request
 * carries no Origin header: nexusd refuses upgrades that do. */
#ifndef PRISM_WS_H
#define PRISM_WS_H
#include <stddef.h>
#include <stdint.h>

#define WS_RBUF 65536

typedef struct {
    int fd;
    uint8_t rbuf[WS_RBUF];
    size_t rlen;
    int closed;
    uint16_t close_code;   /* peer's close code when closed, else 0 */
} ws_t;

/* 0 on success. host may be an IP or a name; path like "/link". */
int ws_connect(ws_t *ws, const char *host, const char *port, const char *path, int timeout_ms);
int ws_send_text(ws_t *ws, const char *text, size_t len);
/* Next complete text message, NUL-terminated into buf. Returns its length, 0 when the
 * peer closed (ws->close_code set), -1 on error, -2 on timeout. */
int ws_recv_text(ws_t *ws, char *buf, size_t cap, int timeout_ms);
void ws_close(ws_t *ws, uint16_t code);
const char *ws_last_error(void);
#endif
