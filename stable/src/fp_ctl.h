// fp_ctl.h
// -----------------------------------------------------------------------------
// Tiny raw-socket HTTP + JSON-scalar helpers shared by the orchestrators.
//
// Extracted from fingerprint_orchestrator.c so the Stage 3 (stress-ng) and Stage 4
// (website) orchestrators cannot drift apart, and so the buffer-sizing fixes below live
// in exactly one place.
//
// Everything here talks to loopback only: the Flask coordinator on FP_SERVER_PORT and,
// for the website orchestrator, Chrome's DevTools endpoint on FP_CDP_PORT. No libcurl.
//
// Why this is not just "the old code moved":
//   * PORT-PARAMETERIZED. The original hardcoded 8080; Stage 4 needs 9222 as well.
//   * NO FIXED REQUEST BUFFER. The original built the request in a char[512] and silently
//     truncated. A truncated JSON body reaches Flask as malformed, get_json(silent=True)
//     returns {}, the workload falls back to "manual", and EVERY class collapses into one
//     directory -- a whole run lost to a snprintf return value nobody checked. Requests are
//     now sized from the actual path/body lengths.
//   * JSON STRING ESCAPING. The original interpolated the workload straight into a JSON
//     body. Fine for "qsort"; not fine once arbitrary strings can reach it.
// -----------------------------------------------------------------------------

#ifndef FP_CTL_H
#define FP_CTL_H

#include <stddef.h>

#define FP_SERVER_PORT 8080   // Flask coordinator (JavaScript/server.py)
#define FP_CDP_PORT    9222   // Chrome DevTools HTTP endpoint (--remote-debugging-port)

// ---- HTTP ----
// All three return 0 on success, -1 on error. `resp` may be NULL to discard the response;
// when non-NULL it receives the full reply (headers + body), NUL-terminated and truncated
// to resp_len-1 if needed. Connection: close, so a short read loop is the whole protocol.

// Generic request. `method` is "GET"/"POST"/"PUT"; `body` may be NULL (no entity).
// `ctype` is the Content-Type for a body-bearing request; NULL selects application/json.
int fp_http_request(int port, const char *method, const char *path,
                    const char *ctype, const char *body, char *resp, int resp_len);

int fp_http_get(int port, const char *path, char *resp, int resp_len);
int fp_http_post(int port, const char *path, const char *body, char *resp, int resp_len);

// ---- Split round trip ----
// The two halves of fp_http_request, for a caller that wants to do work while the peer thinks.
// On loopback the connect+write is microseconds and cannot meaningfully block; all the waiting
// is in the read. website_prober.c uses this to start a victim page load and then sample the
// cache for the whole trace before collecting the reply.
//
// fp_http_send returns a connected fd with the request already written, or -1.
// fp_http_recv drains the reply and ALWAYS closes the fd, including on error.
int fp_http_send(int port, const char *method, const char *path,
                 const char *ctype, const char *body);
int fp_http_recv(int fd, char *resp, int resp_len);

// ---- Minimal JSON scalar extraction ----
// The responses are tiny, flat objects (Flask jsonify / CDP target descriptors), so a
// full parser would be dead weight. These find "key" and read the value that follows.

// Value of "key":<int>, or `dflt` if the key is absent.
long fp_json_int(const char *body, const char *key, long dflt);

// 1 iff "key" is followed by true. Tolerates whitespace after the colon -- Flask's jsonify
// emits `"key": true` (with a space), so a literal "key":true match would fail.
int fp_json_bool(const char *body, const char *key);

// Copy the string value of "key" into out[out_len]. Returns 0 on success, -1 if the key is
// absent, not a string, or does not fit. Understands \" and \\ escapes; other escapes are
// copied verbatim (enough for CDP target ids, which are plain hex).
int fp_json_str(const char *body, const char *key, char *out, size_t out_len);

// ---- Encoders ----
// Both return 0 on success, -1 if the result would not fit (callers must treat that as
// fatal rather than sending a half-encoded string).

// Escape `in` for use inside a JSON string literal (quotes, backslash, control chars).
int fp_json_escape(const char *in, char *out, size_t out_len);

// Percent-encode `in` for use in a URL query string (RFC 3986 unreserved set kept).
int fp_url_encode(const char *in, char *out, size_t out_len);

#endif // FP_CTL_H
