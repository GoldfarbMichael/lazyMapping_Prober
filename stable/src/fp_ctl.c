// fp_ctl.c -- see fp_ctl.h for the rationale.

#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fp_ctl.h"

// Largest response we will hold. /json/list on a browser with many targets is the biggest
// realistic one and is still a few KB.
#define FP_MAX_RESPONSE (64 * 1024)
// Nothing here is remote: every peer is on loopback and answers in well under a second. The
// timeout exists purely so a misbehaving peer can never wedge a multi-hour run.
#define FP_IO_TIMEOUT_S 10

// Open a loopback TCP connection to `port`. -1 on failure.
static int ctl_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = FP_IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

// write() can return short on a socket; loop until the whole request is out.
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

// Case-insensitive scan of the header block for Content-Length. -1 if absent.
static long parse_content_length(const char *hdr, size_t hdr_len) {
    static const char needle[] = "content-length:";
    const size_t nlen = sizeof(needle) - 1;
    for (size_t i = 0; i + nlen < hdr_len; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hdr[i + j]) == needle[j]) j++;
        if (j == nlen) {
            const char *p = hdr + i + nlen;
            while (p < hdr + hdr_len && (*p == ' ' || *p == '\t')) p++;
            return strtol(p, NULL, 10);
        }
    }
    return -1;
}

int fp_http_request(int port, const char *method, const char *path,
                    const char *ctype, const char *body, char *resp, int resp_len) {
    int fd = ctl_connect(port);
    if (fd < 0) {
        fprintf(stderr, "[fp_ctl] connect :%d %s %s failed\n", port, method, path);
        return -1;
    }

    // Size the request from the actual path/body rather than a fixed char[512]: a truncated
    // request is worse than a failed one, because it arrives as a syntactically valid HTTP
    // message carrying a malformed body.
    size_t blen = body ? strlen(body) : 0;
    size_t cap  = strlen(method) + strlen(path) + blen + 256;
    char *req = malloc(cap);
    if (!req) { close(fd); return -1; }

    int rlen;
    if (body) {
        rlen = snprintf(req, cap,
            "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
            "Content-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n%s",
            method, path, port, ctype ? ctype : "application/json", blen, body);
    } else {
        // Chrome's /json/new requires PUT; it accepts a bodyless request but some HTTP
        // stacks want an explicit zero length, so always send Content-Length: 0.
        rlen = snprintf(req, cap,
            "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n",
            method, path, port);
    }
    if (rlen < 0 || (size_t)rlen >= cap) {   // cannot happen with the sizing above; assert it anyway
        fprintf(stderr, "[fp_ctl] request would truncate (%d >= %zu)\n", rlen, cap);
        free(req); close(fd); return -1;
    }
    if (write_all(fd, req, (size_t)rlen) != 0) {
        fprintf(stderr, "[fp_ctl] write :%d %s %s failed\n", port, method, path);
        free(req); close(fd); return -1;
    }
    free(req);

    // Read the reply, stopping at Content-Length rather than at EOF.
    //
    // Reading until EOF is WRONG against a peer that keeps the connection alive: Chrome's
    // DevTools endpoint ignores our `Connection: close` and holds the socket open, so an
    // EOF-terminated loop blocks forever and wedges the whole run. (Werkzeug does close, which
    // is why this only showed up once Stage 4 started talking to :9222.) curl gets this right
    // for the same reason -- it honours Content-Length.
    //
    // Falling back to read-until-EOF when no Content-Length is present keeps the old behaviour
    // for a closing peer; SO_RCVTIMEO bounds that case instead of hanging.
    //
    // The whole reply is drained even when the caller discards it: closing early would RST the
    // peer, and Flask's dev server logs that as a broken pipe on every call.
    char *acc = malloc(FP_MAX_RESPONSE + 1);
    if (!acc) { close(fd); return -1; }
    size_t total = 0, hdr_end = 0;
    long content_len = -1;
    while (total < FP_MAX_RESPONSE) {
        ssize_t r = read(fd, acc + total, FP_MAX_RESPONSE - total);
        if (r <= 0) break;              // EOF, timeout, or error
        total += (size_t)r;
        if (hdr_end == 0) {
            char *h = memmem(acc, total, "\r\n\r\n", 4);
            if (h) {
                hdr_end = (size_t)(h - acc) + 4;
                content_len = parse_content_length(acc, hdr_end);
            }
        }
        if (hdr_end != 0 && content_len >= 0 && total - hdr_end >= (size_t)content_len) break;
    }
    acc[total] = '\0';

    if (resp) {
        size_t n = (total < (size_t)resp_len - 1) ? total : (size_t)resp_len - 1;
        memcpy(resp, acc, n);
        resp[n] = '\0';
    }
    free(acc);
    close(fd);
    return 0;
}

int fp_http_get(int port, const char *path, char *resp, int resp_len) {
    return fp_http_request(port, "GET", path, NULL, NULL, resp, resp_len);
}

int fp_http_post(int port, const char *path, const char *body, char *resp, int resp_len) {
    return fp_http_request(port, "POST", path, "application/json", body, resp, resp_len);
}

// Locate the value position for "key": returns a pointer just past the colon+whitespace,
// or NULL if the key is absent.
static const char *find_value(const char *body, const char *key) {
    char pat[96];
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(body, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p && (*p == ':' || isspace((unsigned char)*p))) p++;
    return p;
}

long fp_json_int(const char *body, const char *key, long dflt) {
    const char *p = find_value(body, key);
    if (!p) return dflt;
    char *end;
    long v = strtol(p, &end, 10);
    return (end == p) ? dflt : v;   // key present but not a number -> default
}

int fp_json_bool(const char *body, const char *key) {
    const char *p = find_value(body, key);
    return p && strncmp(p, "true", 4) == 0;
}

int fp_json_str(const char *body, const char *key, char *out, size_t out_len) {
    const char *p = find_value(body, key);
    if (!p || *p != '"' || out_len == 0) return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;   // unescape the two that matter
        if (i + 1 >= out_len) return -1;                        // no silent truncation
        out[i++] = *p++;
    }
    if (*p != '"') return -1;   // unterminated
    out[i] = '\0';
    return 0;
}

int fp_json_escape(const char *in, char *out, size_t out_len) {
    size_t i = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s; s++) {
        const char *esc = NULL;
        char ubuf[7];
        switch (*s) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:
                if (*s < 0x20) { snprintf(ubuf, sizeof(ubuf), "\\u%04x", *s); esc = ubuf; }
                break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (i + n + 1 > out_len) return -1;
            memcpy(out + i, esc, n);
            i += n;
        } else {
            if (i + 2 > out_len) return -1;
            out[i++] = (char)*s;
        }
    }
    if (i + 1 > out_len) return -1;
    out[i] = '\0';
    return 0;
}

int fp_url_encode(const char *in, char *out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s; s++) {
        if (isalnum(*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            if (i + 2 > out_len) return -1;
            out[i++] = (char)*s;
        } else {
            if (i + 4 > out_len) return -1;
            out[i++] = '%';
            out[i++] = hex[*s >> 4];
            out[i++] = hex[*s & 0x0F];
        }
    }
    if (i + 1 > out_len) return -1;
    out[i] = '\0';
    return 0;
}
