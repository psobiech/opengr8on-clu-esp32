/**
 * @file http_proto.h
 * @brief Minimal HTTP/1.x request parsing and response framing for the GATE HttpListener. Pure C.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Parsed request head. Pointers point into the caller's buffer (NUL-terminated in place). */
typedef struct {
    const char *method;    /**< "GET", "POST", ... */
    const char *path;      /**< Request target without the query, e.g. "/api/x" (percent escapes kept). */
    const char *query;     /**< Text after '?', "" if none. */
    const char *headers;   /**< Header lines "Name: value\r\n..." (without the final empty line). */
    size_t content_length; /**< Content-Length, 0 if absent. */
    bool chunked;          /**< Transfer-Encoding: chunked (not supported by the listener). */
} http_req_head_t;

/**
 * @brief Parse a request head (request line + headers) at the start of @p buf.
 *
 * Modifies @p buf (terminators written in place).
 *
 * @param buf Received bytes; must have room for one more byte (a NUL at buf[len]).
 * @param len Bytes in @p buf.
 * @param out Parsed head.
 * @return Length of the head including the final CRLF CRLF (body starts there), 0 if the head is
 *         not complete yet, -1 if it is malformed.
 */
int http_parse_head(char *buf, size_t len, http_req_head_t *out);

/** @brief Reason phrase for a status code ("OK", "Not Found", ...; "Status" if unknown). */
const char *http_reason(int status);

/**
 * @brief Format a response head.
 *
 * Status line, @p headers (lines "Name: value\r\n" as given, may be NULL), Content-Length,
 * Connection: close and the empty line.
 *
 * @return Length written, or -1 if it does not fit @p cap.
 */
int http_format_head(char *out, size_t cap, int status, const char *headers, size_t body_len);

#ifdef __cplusplus
}
#endif
