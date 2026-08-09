#include "http_client_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

static int send_all(int fd, const uint8_t *data, int len) {
    int sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, (size_t)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (int)n;
    }
    return 0;
}

int http_post_native(const char *host, int port, const char *path_and_query,
                      const uint8_t *body, int body_len,
                      char *resp_body, int resp_body_cap, int *out_status) {
    *out_status = -1;
    if (resp_body_cap > 0) resp_body[0] = 0;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        printf("[http_client] DNS/resolve failed for %s\n", host);
        return 0;
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
        printf("[http_client] connect() failed for %s:%d\n", host, port);
        return 0;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path_and_query, host, port, body_len);
    if (header_len < 0 || header_len >= (int)sizeof(header)) {
        printf("[http_client] request header too long\n");
        close(fd);
        return 0;
    }

    if (send_all(fd, (const uint8_t *)header, header_len) != 0 ||
        (body_len > 0 && send_all(fd, body, body_len) != 0)) {
        printf("[http_client] failed to send request to %s:%d\n", host, port);
        close(fd);
        return 0;
    }

    /* Read the whole response until the server closes the connection
     * (we sent Connection: close, so it will -- same convention
     * server.py's own send_http already relies on for every response it
     * sends, including this endpoint's). Growable buffer since a GLB
     * upload's response is tiny but we don't want a silent truncation on
     * a future, larger reply. */
    int cap = 4096, len = 0;
    uint8_t *raw = (uint8_t *)malloc((size_t)cap);
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *grown = (uint8_t *)realloc(raw, (size_t)cap);
            if (!grown) { free(raw); close(fd); return 0; }
            raw = grown;
        }
        ssize_t n = recv(fd, raw + len, (size_t)(cap - len), 0);
        if (n < 0) { free(raw); close(fd); return 0; }
        if (n == 0) break;   /* server closed -- response complete */
        len += (int)n;
    }
    close(fd);

    /* Parse "HTTP/1.1 <code> ..." and find the header/body boundary. */
    int status = 0;
    const char *sp = memchr(raw, ' ', (size_t)len);
    if (sp) status = atoi(sp + 1);
    *out_status = status;

    const uint8_t *boundary = NULL;
    for (int i = 0; i + 3 < len; i++) {
        if (raw[i] == '\r' && raw[i+1] == '\n' && raw[i+2] == '\r' && raw[i+3] == '\n') {
            boundary = raw + i + 4;
            break;
        }
    }
    if (boundary && resp_body_cap > 0) {
        int body_avail = (int)(raw + len - boundary);
        int n = body_avail < resp_body_cap - 1 ? body_avail : resp_body_cap - 1;
        if (n > 0) memcpy(resp_body, boundary, (size_t)n);
        resp_body[n] = 0;
    }

    free(raw);
    return 1;
}

void http_url_encode(const char *in, char *out, int out_cap) {
    static const char *hex = "0123456789ABCDEF";
    int oi = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && oi < out_cap - 1; p++) {
        unsigned char c = *p;
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out[oi++] = (char)c;
        } else {
            if (oi + 3 > out_cap - 1) break;
            out[oi++] = '%';
            out[oi++] = hex[(c >> 4) & 0xF];
            out[oi++] = hex[c & 0xF];
        }
    }
    out[oi] = 0;
}

int http_parse_ws_host_port(const char *ws_url, char *host, int host_cap, int *port) {
    if (strncmp(ws_url, "ws://", 5) != 0) return -1;
    const char *p = ws_url + 5;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    int hn = (int)((colon ? colon : hostend) - p);
    if (hn <= 0 || hn >= host_cap) return -1;
    memcpy(host, p, (size_t)hn);
    host[hn] = 0;
    *port = 80;
    if (colon) *port = atoi(colon + 1);
    return 0;
}
