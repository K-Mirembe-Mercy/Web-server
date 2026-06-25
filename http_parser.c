#define _GNU_SOURCE
/*
 * http_parser.c — HTTP/1.x request parser
 *
 * Parses a raw byte buffer into an http_request_t without external
 * dependencies.  The parser is intentionally strict: malformed requests
 * are rejected early rather than silently accepted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "http_parser.h"
#include "logger.h"
#include "utils.h"

/* ─── Internal helpers ───────────────────────────────────────────────────── */

static int parse_request_line(const char *line, size_t len,
                               http_request_t *req)
{
    /* METHOD SP URI SP HTTP/1.x */
    char method_str[MAX_METHOD_LEN];
    char uri[MAX_URI_LEN];
    char version_str[16];

    /* Use sscanf for initial split – safe because we NUL-terminate below */
    char tmp[READ_BUFFER_SIZE];
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, line, len);
    tmp[len] = '\0';

    int n = sscanf(tmp, "%15s %8191s %15s", method_str, uri, version_str);
    if (n != 3) {
        LOG_WARN("Bad request line: %.*s", (int)len, line);
        return -1;
    }

    req->method = http_method_from_str(method_str);
    if (req->method == HTTP_METHOD_UNKNOWN) {
        LOG_WARN("Unknown method: %s", method_str);
        return -1;
    }

    if (strncmp(version_str, "HTTP/1.1", 8) == 0) {
        req->version = HTTP_VERSION_1_1;
    } else if (strncmp(version_str, "HTTP/1.0", 8) == 0) {
        req->version = HTTP_VERSION_1_0;
    } else {
        LOG_WARN("Unsupported HTTP version: %s", version_str);
        return -1;
    }

    if (strlen(uri) >= MAX_URI_LEN) return -1;
    safe_copy(req->uri, uri, MAX_URI_LEN);

    /* Split URI into path + query */
    char *q = strchr(req->uri, '?');
    if (q) {
        size_t plen = (size_t)(q - req->uri);
        if (plen >= MAX_PATH_LEN) return -1;
        memcpy(req->path, req->uri, plen);
        req->path[plen] = '\0';
        safe_copy(req->query, q + 1, MAX_QUERY_LEN);
    } else {
        safe_copy(req->path, req->uri, MAX_PATH_LEN);
        req->query[0] = '\0';
    }

    /* URL-decode the path */
    char decoded[MAX_PATH_LEN];
    if (http_url_decode(req->path, strlen(req->path),
                        decoded, sizeof(decoded)) < 0) {
        return -1;
    }

    /* Sanitise (no directory traversal) */
    char safe[MAX_PATH_LEN];
    if (http_sanitise_path(decoded, safe, sizeof(safe)) < 0) {
        LOG_WARN("Path traversal attempt: %s", decoded);
        return -1;
    }
    safe_copy(req->path, safe, MAX_PATH_LEN);

    return 0;
}

static int parse_header_line(const char *line, size_t len,
                              http_request_t *req)
{
    if (req->header_count >= MAX_HEADERS) {
        LOG_WARN("Too many headers");
        return -1;
    }

    const char *colon = memchr(line, ':', len);
    if (!colon) return -1;

    size_t name_len  = (size_t)(colon - line);
    size_t value_len = len - name_len - 1;

    if (name_len == 0 || name_len >= MAX_HEADER_NAME_LEN) return -1;

    http_header_t *h = &req->headers[req->header_count];

    memcpy(h->name, line, name_len);
    h->name[name_len] = '\0';
    str_to_lower(h->name);

    /* Skip leading OWS in value */
    const char *val = colon + 1;
    while (value_len > 0 && (*val == ' ' || *val == '\t')) {
        val++; value_len--;
    }
    /* Strip trailing OWS */
    while (value_len > 0 &&
           (val[value_len-1] == ' ' || val[value_len-1] == '\t')) {
        value_len--;
    }

    if (value_len >= MAX_HEADER_VALUE_LEN) return -1;
    memcpy(h->value, val, value_len);
    h->value[value_len] = '\0';

    req->header_count++;
    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

int http_parse_request(const char *buf, size_t len,
                       http_request_t *req, size_t *consumed)
{
    memset(req, 0, sizeof(*req));

    /* Locate end of headers (\r\n\r\n) */
    const char *header_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            header_end = buf + i + 4;
            break;
        }
    }
    if (!header_end) return -EAGAIN;   /* incomplete, need more data */

    size_t headers_len = (size_t)(header_end - buf) - 4;

    /* Work on a mutable copy of the header section */
    char *hbuf = malloc(headers_len + 1);
    if (!hbuf) return -ENOMEM;
    memcpy(hbuf, buf, headers_len);
    hbuf[headers_len] = '\0';

    int rc = 0;
    char *saveptr = NULL;
    char *line    = strtok_r(hbuf, "\r\n", &saveptr);
    bool first    = true;

    while (line) {
        size_t llen = strlen(line);
        if (first) {
            if (parse_request_line(line, llen, req) != 0) {
                rc = -EINVAL; break;
            }
            first = false;
        } else {
            if (llen > 0 && parse_header_line(line, llen, req) != 0) {
                rc = -EINVAL; break;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    free(hbuf);
    if (rc != 0) return rc;

    /* Determine keep-alive */
    const char *conn = http_get_header(req, "connection");
    if (req->version == HTTP_VERSION_1_1) {
        req->keep_alive = !(conn && strcasecmp(conn, "close") == 0);
    } else {
        req->keep_alive =  (conn && strcasecmp(conn, "keep-alive") == 0);
    }

    /* Body */
    const char *cl_str = http_get_header(req, "content-length");
    if (cl_str) {
        char *endp;
        long cl = strtol(cl_str, &endp, 10);
        if (*endp != '\0' || cl < 0) { return -EINVAL; }
        if ((size_t)cl > MAX_BODY_SIZE)  { return -E2BIG;  }

        size_t body_offset = (size_t)(header_end - buf);
        if (len - body_offset < (size_t)cl) return -EAGAIN;

        req->body = malloc((size_t)cl + 1);
        if (!req->body) return -ENOMEM;
        memcpy(req->body, header_end, (size_t)cl);
        req->body[(size_t)cl] = '\0';
        req->body_len = (size_t)cl;
        *consumed = body_offset + (size_t)cl;
    } else {
        req->body     = NULL;
        req->body_len = 0;
        *consumed = (size_t)(header_end - buf);
    }

    return 0;
}

void http_request_free(http_request_t *req)
{
    if (req) {
        free(req->body);
        req->body     = NULL;
        req->body_len = 0;
    }
}

const char *http_get_header(const http_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

http_method_t http_method_from_str(const char *s)
{
    if (strcmp(s, "GET")     == 0) return HTTP_METHOD_GET;
    if (strcmp(s, "HEAD")    == 0) return HTTP_METHOD_HEAD;
    if (strcmp(s, "POST")    == 0) return HTTP_METHOD_POST;
    if (strcmp(s, "PUT")     == 0) return HTTP_METHOD_PUT;
    if (strcmp(s, "DELETE")  == 0) return HTTP_METHOD_DELETE;
    if (strcmp(s, "OPTIONS") == 0) return HTTP_METHOD_OPTIONS;
    if (strcmp(s, "PATCH")   == 0) return HTTP_METHOD_PATCH;
    return HTTP_METHOD_UNKNOWN;
}

const char *http_method_to_str(http_method_t m)
{
    switch (m) {
        case HTTP_METHOD_GET:     return "GET";
        case HTTP_METHOD_HEAD:    return "HEAD";
        case HTTP_METHOD_POST:    return "POST";
        case HTTP_METHOD_PUT:     return "PUT";
        case HTTP_METHOD_DELETE:  return "DELETE";
        case HTTP_METHOD_OPTIONS: return "OPTIONS";
        case HTTP_METHOD_PATCH:   return "PATCH";
        default:                  return "UNKNOWN";
    }
}

const char *http_status_text(http_status_t s)
{
    switch (s) {
        case HTTP_200_OK:                    return "OK";
        case HTTP_201_CREATED:               return "Created";
        case HTTP_204_NO_CONTENT:            return "No Content";
        case HTTP_206_PARTIAL_CONTENT:       return "Partial Content";
        case HTTP_301_MOVED_PERMANENTLY:     return "Moved Permanently";
        case HTTP_302_FOUND:                 return "Found";
        case HTTP_304_NOT_MODIFIED:          return "Not Modified";
        case HTTP_400_BAD_REQUEST:           return "Bad Request";
        case HTTP_403_FORBIDDEN:             return "Forbidden";
        case HTTP_404_NOT_FOUND:             return "Not Found";
        case HTTP_405_METHOD_NOT_ALLOWED:    return "Method Not Allowed";
        case HTTP_408_REQUEST_TIMEOUT:       return "Request Timeout";
        case HTTP_411_LENGTH_REQUIRED:       return "Length Required";
        case HTTP_413_PAYLOAD_TOO_LARGE:     return "Payload Too Large";
        case HTTP_414_URI_TOO_LONG:          return "URI Too Long";
        case HTTP_416_RANGE_NOT_SATISFIABLE: return "Range Not Satisfiable";
        case HTTP_500_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case HTTP_501_NOT_IMPLEMENTED:       return "Not Implemented";
        case HTTP_503_SERVICE_UNAVAILABLE:   return "Service Unavailable";
        case HTTP_505_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
        default:                             return "Unknown";
    }
}

int http_url_decode(const char *src, size_t src_len,
                    char *dst, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len; si++) {
        if (di + 1 >= dst_len) return -1;
        if (src[si] == '%' && si + 2 < src_len &&
            isxdigit((unsigned char)src[si+1]) &&
            isxdigit((unsigned char)src[si+2])) {
            char hex[3] = { src[si+1], src[si+2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return (int)di;
}

int http_sanitise_path(const char *uri_path, char *out, size_t out_len)
{
    /* Collapse consecutive slashes and resolve ./ and ../ */
    char tmp[MAX_PATH_LEN];
    size_t ti = 0;

    if (uri_path[0] != '/') {
        tmp[ti++] = '/';
    }

    for (size_t i = 0; uri_path[i] && ti + 1 < sizeof(tmp); ) {
        if (uri_path[i] == '/') {
            /* Collapse multiple slashes */
            while (uri_path[i] == '/') i++;
            tmp[ti++] = '/';
        } else if (uri_path[i] == '.' && uri_path[i+1] == '/' ) {
            /* ./ → skip */
            i += 2;
        } else if (uri_path[i] == '.' && uri_path[i+1] == '\0') {
            /* trailing . */
            i++;
        } else if (uri_path[i] == '.' && uri_path[i+1] == '.' &&
                   (uri_path[i+2] == '/' || uri_path[i+2] == '\0')) {
            /* ../ → go up one level */
            if (ti > 1) {
                ti--;
                while (ti > 1 && tmp[ti-1] != '/') ti--;
            }
            i += (uri_path[i+2] == '/') ? 3 : 2;
        } else {
            while (uri_path[i] && uri_path[i] != '/' && ti + 1 < sizeof(tmp)) {
                tmp[ti++] = uri_path[i++];
            }
        }
    }
    if (ti == 0) tmp[ti++] = '/';
    tmp[ti] = '\0';

    if (ti >= out_len) return -1;

    /* Security: ensure we did not escape root (shouldn't happen after above
     * but be explicit) */
    if (strncmp(tmp, "/../", 4) == 0 || strcmp(tmp, "/..") == 0) return -1;

    safe_copy(out, tmp, out_len);
    out[out_len - 1] = '\0';
    return 0;
}
