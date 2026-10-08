#ifndef PDHOST_WS_H
#define PDHOST_WS_H

// A small WebSocket client (RFC 6455) for the lobby and online matches: ws:// and, with mbedTLS,
// wss:// (certificates aren't verified; consoles have no CA store). Connecting happens on a
// thread so the game keeps running; everything else is non-blocking and polled by the caller.

#include <stddef.h>
#include <stdint.h>

#define WS_CLOSED 0
#define WS_CONNECTING 1
#define WS_OPEN 2

struct wsconn;

struct wsconn *wsOpen(const char *url);
int wsState(struct wsconn *c);
const char *wsError(struct wsconn *c);
int wsSendText(struct wsconn *c, const char *text);
int wsSendBinary(struct wsconn *c, const void *data, size_t len);
// next complete message: 1 and *data/*len (valid until the next call), 0 if none yet
int wsRecv(struct wsconn *c, int *isBinary, const uint8_t **data, size_t *len);
void wsClose(struct wsconn *c);

#endif
