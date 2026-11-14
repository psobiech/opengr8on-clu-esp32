/**
 * @file http_proto.c
 * @brief Minimal HTTP/1.x request parsing and response framing for the GATE HttpListener.
 */
#include "http_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/** @brief Value of header @p name in the header block [h, end) (case-insensitive), NULL if absent. */
static const char *find_header(const char *header_block, const char *name, size_t *vlen)
{
    size_t name_length = strlen(name);
    while (*header_block) {
        const char *eol = strstr(header_block, "\r\n");
        if (!eol) {
            eol = header_block + strlen(header_block);
        }
        if ((size_t)(eol - header_block) > name_length && strncasecmp(header_block, name, name_length) == 0 && header_block[name_length] == ':') {
            const char *header_value = header_block + name_length + 1;
            while (header_value < eol && (*header_value == ' ' || *header_value == '\t')) {
                header_value++;
            }
            const char *trimmed_end = eol;
            while (trimmed_end > header_value && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) {
                trimmed_end--;
            }
            *vlen = (size_t)(trimmed_end - header_value);
            return header_value;
        }
        header_block = *eol ? eol + 2 : eol;
    }
    return NULL;
}

int http_parse_head(char *buf, size_t len, http_req_head_t *out)
{
    buf[len] = 0;
    char *end = strstr(buf, "\r\n\r\n");
    if (!end) {
        return memchr(buf, 0, len) ? -1 : 0; // a NUL in the head is malformed
    }
    int head_len = (int)(end - buf) + 4;
    end[2] = 0; // headers block keeps its last CRLF

    char *eol = strstr(buf, "\r\n");
    *eol = 0;
    // Request line: METHOD SP target SP HTTP/x.y
    char *sp1 = strchr(buf, ' ');
    if (!sp1 || sp1 == buf || sp1 - buf > 7) {
        return -1;
    }
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2 || sp2 == sp1 + 1 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        return -1;
    }
    *sp1 = 0;
    *sp2 = 0;
    for (const char *m = buf; *m; m++) {
        if (!isupper((unsigned char)*m)) {
            return -1;
        }
    }
    char *target = sp1 + 1;
    if (target[0] != '/') {
        // absolute-form (http://host/path): keep the path only
        char *absolute_path = strstr(target, "://");
        absolute_path = absolute_path ? strchr(absolute_path + 3, '/') : NULL;
        if (!absolute_path) {
            return -1;
        }
        target = absolute_path;
    }
    char *query_string = strchr(target, '?');
    if (query_string) {
        *query_string++ = 0;
    }
    out->method = buf;
    out->path = target;
    out->query = query_string ? query_string : "";
    out->headers = eol + 2;

    out->content_length = 0;
    size_t header_value_length;
    const char *header_value = find_header(out->headers, "Content-Length", &header_value_length);
    if (header_value) {
        char num[16];
        if (header_value_length == 0 || header_value_length >= sizeof num) {
            return -1;
        }
        memcpy(num, header_value, header_value_length);
        num[header_value_length] = 0;
        char *number_end;
        unsigned long parsed_content_length = strtoul(num, &number_end, 10);
        if (*number_end || !isdigit((unsigned char)num[0])) {
            return -1;
        }
        out->content_length = parsed_content_length;
    }
    header_value = find_header(out->headers, "Transfer-Encoding", &header_value_length);
    out->chunked = header_value && header_value_length >= 7 && strncasecmp(header_value + header_value_length - 7, "chunked", 7) == 0;
    return head_len;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
    }
}

int http_format_head(char *out, size_t cap, int status, const char *headers, size_t body_len)
{
    if (status < 100 || status > 999) {
        status = 500;
    }
    int formatted_length = snprintf(out, cap, "HTTP/1.1 %d %s\r\n%sContent-Length: %u\r\nConnection: close\r\n\r\n", status,
                     http_reason(status), headers ? headers : "", (unsigned)body_len);
    return (formatted_length < 0 || (size_t)formatted_length >= cap) ? -1 : formatted_length;
}
