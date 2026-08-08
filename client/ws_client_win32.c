/* winsock2.h must come before windows.h (or WIN32_LEAN_AND_MEAN must be
 * defined first) or windows.h pulls in the old Winsock 1.1 header and
 * every Winsock2 symbol collides with it. */
#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include "ws_client_win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Winsock port of ws_client_native.c — same RFC 6455 handshake/framing
 * logic already proven against the real server on Linux, Winsock2
 * instead of BSD sockets. See that file's comments for the parts that
 * carry over unchanged (base64, URL parsing, frame format). Differences
 * worth knowing:
 *   - SOCKET/INVALID_SOCKET instead of int/-1, closesocket not close,
 *     ioctlsocket(FIONBIO) not fcntl(O_NONBLOCK), WSAGetLastError/
 *     WSAEWOULDBLOCK not errno/EWOULDBLOCK.
 *   - No MSG_NOSIGNAL — Windows sockets don't raise SIGPIPE on writing to
 *     a closed connection in the first place, different error model.
 *   - SHA1 (for verifying Sec-WebSocket-Accept) uses Windows' own CNG/
 *     BCrypt API instead of OpenSSL, which isn't part of this MinGW
 *     install — also more idiomatic here, matching the project's "native
 *     platform APIs directly" stance (see phi.md's "No SDL/GLFW" row). */

static SOCKET s_sock = INVALID_SOCKET;
static int    s_connected = 0;
static int    s_wsa_started = 0;

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

/* ---- base64 (identical to ws_client_native.c's) ---- */
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

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* SHA1 via Windows CNG — the verbose open/create/hash/finish/destroy
 * sequence, supported since Vista (vs. the newer one-shot BCryptHash,
 * Windows 10 1607+); no reason to assume a newer minimum here. */
static int sha1_bcrypt(const uint8_t *data, int len, unsigned char *digest20) {
    BCRYPT_ALG_HANDLE  hAlg  = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    int ok = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0)
        return 0;
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0) == 0 &&
            BCryptFinishHash(hHash, digest20, 20, 0) == 0) {
            ok = 1;
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

static int compute_expected_accept(const char *key, char *out_b64, int out_sz) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);
    unsigned char digest[20];
    if (!sha1_bcrypt((const uint8_t *)combined, (int)strlen(combined), digest)) return -1;
    return base64_encode(digest, 20, out_b64, out_sz) < 0 ? -1 : 0;
}

static int recv_line_buffered(SOCKET sock, char *out, int out_sz) {
    int n = 0;
    while (n < out_sz - 1) {
        char c;
        int r = recv(sock, &c, 1, 0);
        if (r <= 0) return -1;
        out[n++] = c;
        if (n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0) {
            out[n] = 0;
            return n;
        }
    }
    return -1;
}

/* Windows has no strcasestr — hand-rolled, case-insensitive substring
 * search, same interface. */
static const char *win_strcasestr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (_strnicmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

int ws_client_connect(const char *url) {
    if (!s_wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[ws_client] WSAStartup failed\n");
            return -1;
        }
        s_wsa_started = 1;
    }

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

    SOCKET sock = INVALID_SOCKET;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
        if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(sock); sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (sock == INVALID_SOCKET) {
        printf("[ws_client] connect() failed for %s:%d\n", host, port);
        return -1;
    }

    BOOL one = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    uint8_t keybytes[16];
    srand((unsigned)time(NULL) ^ (unsigned)GetCurrentProcessId());
    for (size_t i = 0; i < sizeof(keybytes); i++) keybytes[i] = (uint8_t)rand();
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
    if (send(sock, req, reqlen, 0) != reqlen) {
        printf("[ws_client] failed to send handshake request\n");
        closesocket(sock);
        return -1;
    }

    char resp[2048];
    if (recv_line_buffered(sock, resp, sizeof(resp)) < 0) {
        printf("[ws_client] no/short handshake response\n");
        closesocket(sock);
        return -1;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        printf("[ws_client] handshake rejected: %.60s...\n", resp);
        closesocket(sock);
        return -1;
    }

    const char *hdr = win_strcasestr(resp, "Sec-WebSocket-Accept:");
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

    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);

    s_sock = sock;
    s_connected = 1;
    s_recvlen = 0;
    printf("[ws_client] connected to %s:%d%s\n", host, port, path);
    return 0;
}

void ws_client_send_binary(const uint8_t *data, int len) {
    if (!s_connected || s_sock == INVALID_SOCKET) return;

    uint8_t header[14];
    int hi = 0;
    header[hi++] = 0x80 | 0x02;

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
        int n = send(s_sock, (const char *)(framed + sent), total - sent, 0);
        if (n > 0) { sent += n; continue; }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) { attempts++; continue; }
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
        if (avail < mask_end + plen) break;

        uint8_t *maskkey = masked ? &s_recvbuf[off + idx] : NULL;
        uint8_t *payload = &s_recvbuf[off + mask_end];

        if (opcode == 0x2) {
            if (masked) {
                for (int i = 0; i < plen; i++) payload[i] ^= maskkey[i % 4];
            }
            on_message(user, payload, plen);
        } else if (opcode == 0x8) {
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
    if (!s_connected || s_sock == INVALID_SOCKET) return;

    for (;;) {
        ensure_recv_capacity(s_recvlen + 4096);
        int n = recv(s_sock, (char *)(s_recvbuf + s_recvlen), s_recvcap - s_recvlen, 0);
        if (n > 0) {
            s_recvlen += n;
            continue;
        }
        if (n == 0) { ws_client_close(); return; }
        if (WSAGetLastError() == WSAEWOULDBLOCK) break;
        ws_client_close();
        return;
    }
    dispatch_frames(on_message, user);
}

int ws_client_connected(void) { return s_connected; }

void ws_client_close(void) {
    if (s_sock != INVALID_SOCKET) closesocket(s_sock);
    s_sock = INVALID_SOCKET;
    s_connected = 0;
    s_recvlen = 0;
}
