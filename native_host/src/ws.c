#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "ws.h"

#ifdef PDHOST_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <psa/crypto.h>
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#define MBEDTLS_ERR_NET_CONN_RESET -0x0050
#endif
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct wsconn {
	char host[256];
	char port[8];
	char path[256];
	int tls;
	int fd;
	volatile int state;
	volatile int stop;
	char error[128];
	pthread_t thread;
	int threadStarted;

	uint8_t *rx;          // bytes received, not yet parsed
	size_t rxLen, rxCap;
	uint8_t *msg;         // the message being assembled from frames
	size_t msgLen, msgCap;
	int msgBinary;
	uint8_t *out;         // the last complete message handed to the caller
	size_t outCap;

#ifdef PDHOST_TLS
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
#endif
};

static void setError(struct wsconn *c, const char *fmt, const char *detail)
{
	snprintf(c->error, sizeof(c->error), fmt, detail ? detail : "");
}

/* ------------------------------------------------------------------------
 * transport: plain socket or TLS
 * ------------------------------------------------------------------------ */

#ifdef PDHOST_TLS
static int bioSend(void *ctx, const unsigned char *buf, size_t len)
{
	struct wsconn *c = ctx;
	const ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
	if (n < 0) {
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return (int)n;
}

static int bioRecv(void *ctx, unsigned char *buf, size_t len)
{
	struct wsconn *c = ctx;
	const ssize_t n = recv(c->fd, buf, len, 0);
	if (n < 0) {
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (n == 0) {
		return MBEDTLS_ERR_NET_CONN_RESET;
	}
	return (int)n;
}
#endif

// 0 = would block, -1 = closed/error, >0 = bytes
static ssize_t rawRead(struct wsconn *c, void *buf, size_t len)
{
#ifdef PDHOST_TLS
	if (c->tls) {
		const int n = mbedtls_ssl_read(&c->ssl, buf, len);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
			return 0;
		}
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
			return 0;
		}
#endif
		return n > 0 ? n : -1;
	}
#endif
	const ssize_t n = recv(c->fd, buf, len, 0);
	if (n < 0) {
		return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
	}
	return n == 0 ? -1 : n;
}

static int rawWriteAll(struct wsconn *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len) {
		ssize_t n;
#ifdef PDHOST_TLS
		if (c->tls) {
			n = mbedtls_ssl_write(&c->ssl, p, len);
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
				usleep(1000);
				continue;
			}
		} else
#endif
		{
			n = send(c->fd, p, len, MSG_NOSIGNAL);
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
				usleep(1000);
				continue;
			}
		}
		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * connecting (on a thread)
 * ------------------------------------------------------------------------ */

static void base64(const uint8_t *in, size_t len, char *out)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
		out[o++] = tbl[(v >> 18) & 63];
		out[o++] = tbl[(v >> 12) & 63];
		out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < len ? tbl[v & 63] : '=';
	}
	out[o] = '\0';
}

static uint32_t rnd32(void)
{
	static uint32_t s;
	if (!s) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		s = (uint32_t)(tv.tv_sec ^ (tv.tv_usec << 12) ^ (uintptr_t)&tv) | 1;
	}
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

static int tcpConnect(struct wsconn *c)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(c->host, c->port, &hints, &res) != 0 || !res) {
		setError(c, "can't find %s", c->host);
		return -1;
	}
	for (struct addrinfo *a = res; a && !c->stop; a = a->ai_next) {
		const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (fd < 0) {
			continue;
		}
		struct timeval tv = { 5, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
			const int one = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
			c->fd = fd;
			freeaddrinfo(res);
			return 0;
		}
		close(fd);
	}
	freeaddrinfo(res);
	setError(c, "can't connect to %s", c->host);
	return -1;
}

#ifdef PDHOST_TLS
#ifdef __PROSPERO__
// the console sandbox may not allow mbedTLS's own entropy sources; the system's arc4random works
#include <stdlib.h>
static int consoleEntropy(void *ctx, unsigned char *out, size_t len)
{
	arc4random_buf(out, len);
	return 0;
}
#endif

static int tlsStart(struct wsconn *c)
{
	static int psaReady;
	if (!psaReady) {
		psa_crypto_init();
		psaReady = 1;
	}
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_entropy_init(&c->entropy);
	mbedtls_ctr_drbg_init(&c->drbg);
#ifdef __PROSPERO__
	int (*entropy)(void *, unsigned char *, size_t) = consoleEntropy;
#else
	int (*entropy)(void *, unsigned char *, size_t) = mbedtls_entropy_func;
#endif
	if (mbedtls_ctr_drbg_seed(&c->drbg, entropy, &c->entropy, (const unsigned char *)"pdhost", 6) != 0
			|| mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		setError(c, "TLS setup failed%s", NULL);
		return -1;
	}
	// no CA store on consoles; the game's data isn't secret
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
	if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) {
		setError(c, "TLS setup failed%s", NULL);
		return -1;
	}
	mbedtls_ssl_set_hostname(&c->ssl, c->host);
	mbedtls_ssl_set_bio(&c->ssl, c, bioSend, bioRecv, NULL);
	int r;
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
			setError(c, "TLS handshake with %s failed", c->host);
			return -1;
		}
	}
	return 0;
}
#endif

static int httpUpgrade(struct wsconn *c)
{
	uint8_t keyBytes[16];
	char key[32], req[1024];
	for (int i = 0; i < 16; i++) {
		keyBytes[i] = (uint8_t)rnd32();
	}
	base64(keyBytes, 16, key);
	const int defaultPort = !strcmp(c->port, c->tls ? "443" : "80");
	snprintf(req, sizeof(req),
		"GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: pdhost\r\n\r\n",
		c->path, c->host, defaultPort ? "" : ":", defaultPort ? "" : c->port, key);
	if (rawWriteAll(c, req, strlen(req)) != 0) {
		setError(c, "%s closed the connection", c->host);
		return -1;
	}

	// read the response headers (blocking, with the socket's timeout)
	char resp[2048];
	size_t len = 0;
	while (len < sizeof(resp) - 1) {
		const ssize_t n = rawRead(c, resp + len, 1);
		if (n < 0) {
			setError(c, "%s closed the connection", c->host);
			return -1;
		}
		if (n == 0) {
			usleep(1000);
			continue;
		}
		len += (size_t)n;
		resp[len] = '\0';
		if (len >= 4 && !strcmp(resp + len - 4, "\r\n\r\n")) {
			break;
		}
	}
	if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
		setError(c, "%s isn't a game lobby", c->host);
		return -1;
	}
	return 0;
}

static void *connectThread(void *arg)
{
	struct wsconn *c = arg;
	if (tcpConnect(c) == 0
#ifdef PDHOST_TLS
			&& (!c->tls || tlsStart(c) == 0)
#endif
			&& httpUpgrade(c) == 0) {
		fcntl(c->fd, F_SETFL, fcntl(c->fd, F_GETFL) | O_NONBLOCK);
		c->state = WS_OPEN;
	} else {
		c->state = WS_CLOSED;
	}
	return NULL;
}

struct wsconn *wsOpen(const char *url)
{
	struct wsconn *c = calloc(1, sizeof(*c));
	const char *p = url;
	c->fd = -1;

	if (!strncmp(p, "wss://", 6)) {
		c->tls = 1;
		p += 6;
	} else if (!strncmp(p, "ws://", 5)) {
		p += 5;
	}

	const char *slash = strchr(p, '/');
	const size_t hostLen = slash ? (size_t)(slash - p) : strlen(p);
	char hostport[256];
	snprintf(hostport, sizeof(hostport), "%.*s", (int)hostLen, p);
	snprintf(c->path, sizeof(c->path), "%s", slash ? slash : "/");

	char *colon = strrchr(hostport, ':');
	if (colon && !strchr(colon, ']')) {
		*colon = '\0';
		snprintf(c->port, sizeof(c->port), "%s", colon + 1);
	} else {
		snprintf(c->port, sizeof(c->port), "%s", c->tls ? "443" : "80");
	}
	snprintf(c->host, sizeof(c->host), "%s", hostport);

#ifndef PDHOST_TLS
	if (c->tls) {
		setError(c, "secure connections (wss) aren't available in this build%s", NULL);
		c->state = WS_CLOSED;
		return c;
	}
#endif

	c->state = WS_CONNECTING;
	if (pthread_create(&c->thread, NULL, connectThread, c) == 0) {
		c->threadStarted = 1;
	} else {
		c->state = WS_CLOSED;
	}
	return c;
}

int wsState(struct wsconn *c)
{
	return c ? c->state : WS_CLOSED;
}

const char *wsError(struct wsconn *c)
{
	return c ? c->error : "";
}

/* ------------------------------------------------------------------------
 * frames
 * ------------------------------------------------------------------------ */

static int sendFrame(struct wsconn *c, int opcode, const void *data, size_t len)
{
	if (!c || c->state != WS_OPEN) {
		return -1;
	}
	uint8_t hdr[14];
	size_t h = 0;
	uint8_t mask[4];
	const uint32_t m = rnd32();
	memcpy(mask, &m, 4);

	hdr[h++] = (uint8_t)(0x80 | opcode);
	if (len < 126) {
		hdr[h++] = (uint8_t)(0x80 | len);
	} else if (len < 65536) {
		hdr[h++] = 0x80 | 126;
		hdr[h++] = (uint8_t)(len >> 8);
		hdr[h++] = (uint8_t)len;
	} else {
		hdr[h++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--) {
			hdr[h++] = (uint8_t)((uint64_t)len >> (i * 8));
		}
	}
	memcpy(hdr + h, mask, 4);
	h += 4;

	uint8_t *buf = malloc(h + len);
	memcpy(buf, hdr, h);
	for (size_t i = 0; i < len; i++) {
		buf[h + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
	}
	const int r = rawWriteAll(c, buf, h + len);
	free(buf);
	if (r != 0) {
		c->state = WS_CLOSED;
	}
	return r;
}

int wsSendText(struct wsconn *c, const char *text)
{
	return sendFrame(c, 0x1, text, strlen(text));
}

int wsSendBinary(struct wsconn *c, const void *data, size_t len)
{
	return sendFrame(c, 0x2, data, len);
}

static void grow(uint8_t **buf, size_t *cap, size_t need)
{
	if (need > *cap) {
		size_t n = *cap ? *cap : 4096;
		while (n < need) {
			n *= 2;
		}
		*buf = realloc(*buf, n);
		*cap = n;
	}
}

int wsRecv(struct wsconn *c, int *isBinary, const uint8_t **data, size_t *len)
{
	if (!c || c->state != WS_OPEN) {
		return 0;
	}

	// take in what's arrived
	for (;;) {
		grow(&c->rx, &c->rxCap, c->rxLen + 65536);
		const ssize_t n = rawRead(c, c->rx + c->rxLen, c->rxCap - c->rxLen);
		if (n < 0) {
			c->state = WS_CLOSED;
			setError(c, "%s closed the connection", c->host);
			break;
		}
		if (n == 0) {
			break;
		}
		c->rxLen += (size_t)n;
	}

	// parse whole frames
	for (;;) {
		if (c->rxLen < 2) {
			return 0;
		}
		const uint8_t b0 = c->rx[0], b1 = c->rx[1];
		size_t h = 2;
		uint64_t plen = b1 & 0x7f;
		if (plen == 126) {
			if (c->rxLen < 4) return 0;
			plen = (uint64_t)c->rx[2] << 8 | c->rx[3];
			h = 4;
		} else if (plen == 127) {
			if (c->rxLen < 10) return 0;
			plen = 0;
			for (int i = 0; i < 8; i++) {
				plen = plen << 8 | c->rx[2 + i];
			}
			h = 10;
		}
		const int masked = b1 & 0x80;
		if (masked) {
			h += 4;
		}
		if (c->rxLen < h + plen) {
			return 0;
		}

		uint8_t *payload = c->rx + h;
		if (masked) {
			for (uint64_t i = 0; i < plen; i++) {
				payload[i] ^= c->rx[h - 4 + (i & 3)];
			}
		}

		const int fin = b0 & 0x80;
		const int opcode = b0 & 0x0f;
		int complete = 0;

		if (opcode == 0x8) {
			c->state = WS_CLOSED;
			setError(c, "%s closed the connection", c->host);
			return 0;
		} else if (opcode == 0x9) {
			sendFrame(c, 0xA, payload, (size_t)plen); // pong
		} else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
			if (opcode != 0x0) {
				c->msgLen = 0;
				c->msgBinary = opcode == 0x2;
			}
			grow(&c->msg, &c->msgCap, c->msgLen + (size_t)plen + 1);
			memcpy(c->msg + c->msgLen, payload, (size_t)plen);
			c->msgLen += (size_t)plen;
			complete = fin;
		}

		// drop the frame from the receive buffer
		memmove(c->rx, c->rx + h + plen, c->rxLen - (size_t)(h + plen));
		c->rxLen -= (size_t)(h + plen);

		if (complete) {
			grow(&c->out, &c->outCap, c->msgLen + 1);
			memcpy(c->out, c->msg, c->msgLen);
			c->out[c->msgLen] = '\0'; // text messages can be used as C strings
			*isBinary = c->msgBinary;
			*data = c->out;
			*len = c->msgLen;
			c->msgLen = 0;
			return 1;
		}
	}
}

void wsClose(struct wsconn *c)
{
	if (!c) {
		return;
	}
	c->stop = 1;
	if (c->threadStarted) {
		pthread_join(c->thread, NULL);
	}
	if (c->state == WS_OPEN) {
		sendFrame(c, 0x8, "\x03\xe8", 2);
	}
#ifdef PDHOST_TLS
	if (c->tls) {
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_ctr_drbg_free(&c->drbg);
		mbedtls_entropy_free(&c->entropy);
	}
#endif
	if (c->fd >= 0) {
		close(c->fd);
	}
	free(c->rx);
	free(c->msg);
	free(c->out);
	free(c);
}
