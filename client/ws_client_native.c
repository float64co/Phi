#define _GNU_SOURCE  /* strcasestr */
#include "ws_client_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <openssl/evp.h>

static int  s_fd = -1;
static int  s_connected = 0;

/* Growable, not fixed: PKT_MAP_FULL sends an entire .cmap as one frame —
 * well over 100KB for a populated arena — so a small fixed buffer isn't
 * safe here (a fixed cap is fine for *sending*, see ws_client_send_binary,
 * since outgoing packets are all small fixed-format protocol messages;
 * incoming ones aren't uniformly small). */
static uint8_t *s_recvbuf = NULL;
static int      s_recvcap = 0;
static int      s_recvlen = 0;

static void ensure_recv_capacity(int min_cap) {
    if (s_recvcap >= min_cap) return;
    int newcap = s_recvcap > 0 ? s_recvcap : 4096;
    while (newcap < min_cap) newcap *= 2;
    uint8_t *grown = (uint8_t *)realloc(s_recvbuf, (size_t)newcap);
    if (!grown) { printf("[ws_client] out of memory growing recv buffer\n"); return; }
    s_recvbuf = grown;
    s_recvcap = newcap;
}

/* ---- base64 (RFC 4648, standard alphabet — used for the Sec-WebSocket-Key
 * we send and to verify the Accept header the server sends back) ---- */
static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *data, int len, char *out, int out_sz) {
    int oi = 0;
    for (int i = 0; i < len; i += 3) {
        if (oi + 5 > out_sz) return -1;
        uint32_t chunk = (uint32_t)data[i] << 16;
        int n = 1;
        if (i + 1 < len) { chunk |= (uint32_t)data[i + 1] << 8; n = 2; }
        if (i + 2 < len) { chunk |= (uint32_t)data[i + 2];      n = 3; }
        out[oi++] = B64_ALPHABET[(chunk >> 18) & 0x3F];
        out[oi++] = B64_ALPHABET[(chunk >> 12) & 0x3F];
        out[oi++] = (n >= 2) ? B64_ALPHABET[(chunk >> 6) & 0x3F] : '=';
        out[oi++] = (n >= 3) ? B64_ALPHABET[chunk & 0x3F]        : '=';
    }
    if (oi >= out_sz) return -1;
    out[oi] = 0;
    return oi;
}

/* ---- URL parsing: "ws://host[:port][/path]" ---- */
static int parse_ws_url(const char *url, char *host, int host_sz,
                         int *port, char *path, int path_sz) {
    if (strncmp(url, "ws://", 5) != 0) return -1;
    const char *p = url + 5;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));

    int hn = (int)((colon ? colon : hostend) - p);
    if (hn <= 0 || hn >= host_sz) return -1;
    memcpy(host, p, (size_t)hn);
    host[hn] = 0;

    *port = 80;
    if (colon) *port = atoi(colon + 1);

    if (slash) {
        int pn = (int)strlen(slash);
        if (pn >= path_sz) return -1;
        strcpy(path, slash);
    } else {
        strcpy(path, "/");
    }
    return 0;
}

/* ---- Sec-WebSocket-Accept verification ---- */
#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static int compute_expected_accept(const char *key, char *out_b64, int out_sz) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha1(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, combined, strlen(combined)) == 1 &&
             EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return -1;

    return base64_encode(digest, (int)digest_len, out_b64, out_sz) < 0 ? -1 : 0;
}

/* ---- Handshake ---- */
static int recv_line_buffered(int fd, char *out, int out_sz) {
    /* Reads the full HTTP response headers (until "\r\n\r\n") into `out`.
     * Blocking — the handshake itself is a one-time, small (<1KB), fast
     * exchange with a local dev server; not worth a non-blocking state
     * machine for this part. */
    int n = 0;
    while (n < out_sz - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        out[n++] = c;
        if (n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0) {
            out[n] = 0;
            return n;
        }
    }
    return -1;
}

int ws_client_connect(const char *url) {
    char host[128], path[256];
    int port;
    if (parse_ws_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        printf("[ws_client] bad URL: %s\n", url);
        return -1;
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        printf("[ws_client] DNS/resolve failed for %s\n", host);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        printf("[ws_client] connect() failed for %s:%d\n", host, port);
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* Sec-WebSocket-Key: 16 random bytes, base64-encoded */
    uint8_t keybytes[16];
    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) { size_t got = fread(keybytes, 1, sizeof(keybytes), ur); fclose(ur); (void)got; }
    else {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (size_t i = 0; i < sizeof(keybytes); i++) keybytes[i] = (uint8_t)rand();
    }
    char key_b64[32];
    base64_encode(keybytes, sizeof(keybytes), key_b64, sizeof(key_b64));

    char req[512];
    int reqlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, port, key_b64);
    if (send(fd, req, (size_t)reqlen, 0) != reqlen) {
        printf("[ws_client] failed to send handshake request\n");
        close(fd);
        return -1;
    }

    char resp[2048];
    if (recv_line_buffered(fd, resp, sizeof(resp)) < 0) {
        printf("[ws_client] no/short handshake response\n");
        close(fd);
        return -1;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        printf("[ws_client] handshake rejected: %.60s...\n", resp);
        close(fd);
        return -1;
    }

    /* Verify Sec-WebSocket-Accept — the local dev server doesn't require
     * this (it never checks what key it was given, see server.py's
     * ws_handshake_response), but a real client should always confirm the
     * server actually understood the handshake it was sent. */
    const char *hdr = strcasestr(resp, "Sec-WebSocket-Accept:");
    if (hdr) {
        hdr += strlen("Sec-WebSocket-Accept:");
        while (*hdr == ' ') hdr++;
        char actual[64]; int i = 0;
        while (*hdr && *hdr != '\r' && i < (int)sizeof(actual) - 1) actual[i++] = *hdr++;
        actual[i] = 0;
        char expected[64];
        if (compute_expected_accept(key_b64, expected, sizeof(expected)) == 0 &&
            strcmp(actual, expected) != 0) {
            printf("[ws_client] Sec-WebSocket-Accept mismatch (got %s, expected %s) — "
                   "continuing anyway, server accepted the upgrade\n", actual, expected);
        }
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);

    s_fd = fd;
    s_connected = 1;
    s_recvlen = 0;
    printf("[ws_client] connected to %s:%d%s\n", host, port, path);
    return 0;
}

void ws_client_send_binary(const uint8_t *data, int len) {
    if (!s_connected || s_fd < 0) return;

    uint8_t header[14];
    int hi = 0;
    header[hi++] = 0x80 | 0x02;  /* FIN + binary opcode */

    if (len < 126) {
        header[hi++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        header[hi++] = 0x80 | 126;
        header[hi++] = (uint8_t)((len >> 8) & 0xFF);
        header[hi++] = (uint8_t)(len & 0xFF);
    } else {
        header[hi++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) header[hi++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)rand();
    memcpy(header + hi, mask, 4); hi += 4;

    /* Small packets only (protocol table tops out well under 200 bytes) —
     * a fixed stack buffer is fine, no need for heap framing. */
    uint8_t framed[14 + 256];
    if (hi + len > (int)sizeof(framed)) {
        printf("[ws_client] payload too large (%d bytes), dropped\n", len);
        return;
    }
    memcpy(framed, header, (size_t)hi);
    for (int i = 0; i < len; i++) framed[hi + i] = data[i] ^ mask[i % 4];

    int total = hi + len, sent = 0;
    int attempts = 0;
    while (sent < total && attempts < 1000) {
        ssize_t n = send(s_fd, framed + sent, (size_t)(total - sent), MSG_NOSIGNAL);
        if (n > 0) { sent += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { attempts++; continue; }
        printf("[ws_client] send failed, closing\n");
        ws_client_close();
        return;
    }
}

static void dispatch_frames(WsClientOnMessage on_message, void *user) {
    int off = 0;
    for (;;) {
        int avail = s_recvlen - off;
        if (avail < 2) break;
        uint8_t b0 = s_recvbuf[off], b1 = s_recvbuf[off + 1];
        int opcode = b0 & 0x0F;
        int masked = (b1 & 0x80) != 0;
        int plen   = b1 & 0x7F;
        int idx = 2;
        if (plen == 126) {
            if (avail < 4) break;
            plen = (s_recvbuf[off + 2] << 8) | s_recvbuf[off + 3];
            idx = 4;
        } else if (plen == 127) {
            if (avail < 10) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | s_recvbuf[off + 2 + i];
            idx = 10;
        }
        int mask_end = idx + (masked ? 4 : 0);
        if (avail < mask_end + plen) break;  /* incomplete frame, wait for more bytes */

        uint8_t *maskkey = masked ? &s_recvbuf[off + idx] : NULL;
        uint8_t *payload = &s_recvbuf[off + mask_end];

        if (opcode == 0x2 /* binary */) {
            if (masked) {
                /* Server never masks (see server.py), but handle it anyway
                 * rather than assume. Unmask in place — payload points into
                 * our own buffer and is consumed immediately below, so
                 * there's no need for a second copy sized to match. */
                for (int i = 0; i < plen; i++) payload[i] ^= maskkey[i % 4];
            }
            on_message(user, payload, plen);
        } else if (opcode == 0x8 /* close */) {
            ws_client_close();
            return;
        }
        off += mask_end + plen;
    }
    if (off > 0) {
        s_recvlen -= off;
        memmove(s_recvbuf, s_recvbuf + off, (size_t)s_recvlen);
    }
}

void ws_client_poll(WsClientOnMessage on_message, void *user) {
    if (!s_connected || s_fd < 0) return;

    for (;;) {
        ensure_recv_capacity(s_recvlen + 4096);
        ssize_t n = recv(s_fd, s_recvbuf + s_recvlen, (size_t)(s_recvcap - s_recvlen), 0);
        if (n > 0) {
            s_recvlen += (int)n;
            continue;
        }
        if (n == 0) { ws_client_close(); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        ws_client_close();
        return;
    }
    dispatch_frames(on_message, user);
}

int ws_client_connected(void) { return s_connected; }

void ws_client_close(void) {
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    s_connected = 0;
    s_recvlen = 0;
}
