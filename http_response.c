#define _GNU_SOURCE
/*
 * http_response.c — HTTP response builder & sender
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "http_response.h"
#include "http_parser.h"
#include "logger.h"
#include "utils.h"

/* ─── Init / teardown ────────────────────────────────────────────────────── */

void http_response_init(http_response_t *res)
{
    memset(res, 0, sizeof(*res));
    res->status = HTTP_200_OK;
}

void http_response_free(http_response_t *res)
{
    if (res && !res->body_is_file) {
        free(res->body);
        res->body = NULL;
    }
}

/* ─── Header management ──────────────────────────────────────────────────── */

int http_response_add_header(http_response_t *res,
                              const char *name, const char *value)
{
    if (res->header_count >= MAX_HEADERS) return -1;
    http_header_t *h = &res->headers[res->header_count++];
    strncpy(h->name,  name,  MAX_HEADER_NAME_LEN  - 1);
    strncpy(h->value, value, MAX_HEADER_VALUE_LEN - 1);
    return 0;
}

/* ─── Body ───────────────────────────────────────────────────────────────── */

int http_response_set_body(http_response_t *res,
                            const char *body, size_t len)
{
    free(res->body);
    res->body = malloc(len + 1);
    if (!res->body) return -1;
    memcpy(res->body, body, len);
    res->body[len] = '\0';
    res->body_len = len;
    res->body_is_file = false;
    return 0;
}

/* ─── Serialise headers to wire format ───────────────────────────────────── */

ssize_t http_response_serialise(const http_response_t *res,
                                 const http_request_t  *req,
                                 char **out)
{
    /* Rough upper bound: status line + headers + CRLF */
    size_t cap = 4096 + (size_t)res->header_count * (MAX_HEADER_NAME_LEN + MAX_HEADER_VALUE_LEN + 4);
    char *buf  = malloc(cap);
    if (!buf) return -1;

    /* Status line */
    int n = snprintf(buf, cap, "HTTP/1.1 %d %s\r\n",
                     res->status, http_status_text(res->status));
    if (n < 0 || (size_t)n >= cap) { free(buf); return -1; }

    size_t pos = (size_t)n;

    /* Standard headers */
    char date_buf[64];
    http_date_now(date_buf, sizeof(date_buf));

    char tmp[512];
    /* Date */
    snprintf(tmp, sizeof(tmp), "Date: %s\r\n", date_buf);
    if (pos + strlen(tmp) >= cap) { free(buf); return -1; }
    memcpy(buf + pos, tmp, strlen(tmp)); pos += strlen(tmp);

    /* Server */
    const char *srv = "Server: " SERVER_NAME "/" SERVER_VERSION "\r\n";
    memcpy(buf + pos, srv, strlen(srv)); pos += strlen(srv);

    /* Connection */
    bool ka = req ? req->keep_alive : false;
    const char *conn_hdr = ka
        ? "Connection: keep-alive\r\nKeep-Alive: timeout=75, max=100\r\n"
        : "Connection: close\r\n";
    memcpy(buf + pos, conn_hdr, strlen(conn_hdr)); pos += strlen(conn_hdr);

    /* Content-Length (unless body is a file – caller must add it) */
    if (!res->body_is_file && res->body_len > 0) {
        snprintf(tmp, sizeof(tmp), "Content-Length: %zu\r\n", res->body_len);
        memcpy(buf + pos, tmp, strlen(tmp)); pos += strlen(tmp);
    }

    /* Custom headers */
    for (int i = 0; i < res->header_count; i++) {
        snprintf(tmp, sizeof(tmp), "%s: %s\r\n",
                 res->headers[i].name, res->headers[i].value);
        if (pos + strlen(tmp) >= cap) { free(buf); return -1; }
        memcpy(buf + pos, tmp, strlen(tmp)); pos += strlen(tmp);
    }

    /* End of headers */
    memcpy(buf + pos, "\r\n", 2); pos += 2;

    /* Inline body (HEAD has no body) */
    if (!res->body_is_file && res->body_len > 0 &&
        req && req->method != HTTP_METHOD_HEAD) {
        buf = realloc(buf, pos + res->body_len + 1);
        if (!buf) return -1;
        memcpy(buf + pos, res->body, res->body_len);
        pos += res->body_len;
    }

    *out = buf;
    return (ssize_t)pos;
}

/* ─── Send response ──────────────────────────────────────────────────────── */

ssize_t http_response_send(http_response_t *res,
                            const http_request_t *req,
                            int fd)
{
    /* For file responses the caller sets body to a file path (not mmap'd).
     * We use a dedicated code path with sendfile(2). */
    if (res->body_is_file) {
        /* body field holds the file path */
        int file_fd = open(res->body, O_RDONLY);
        if (file_fd < 0) {
            LOG_ERROR("open(%s): %s", res->body, strerror(errno));
            http_send_error(fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
            return -1;
        }
        struct stat st;
        if (fstat(file_fd, &st) < 0) {
            close(file_fd);
            http_send_error(fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
            return -1;
        }

        /* Build header block with correct Content-Length */
        char cl_str[32];
        snprintf(cl_str, sizeof(cl_str), "%lld", (long long)st.st_size);
        http_response_add_header(res, "Content-Length", cl_str);

        char *hdr; ssize_t hdr_len;
        /* Temporarily treat as non-file for serialise */
        res->body_is_file = false;
        res->body         = NULL;
        res->body_len     = 0;
        hdr_len = http_response_serialise(res, req, &hdr);
        res->body_is_file = true; /* restore */

        if (hdr_len < 0) { close(file_fd); return -1; }

        ssize_t total = 0;
        if (write_all(fd, hdr, (size_t)hdr_len) != 0) {
            free(hdr); close(file_fd); return -1;
        }
        total += hdr_len;
        free(hdr);

        if (req && req->method != HTTP_METHOD_HEAD) {
            off_t offset = 0;
            ssize_t sent;
            off_t remaining = st.st_size;
            while (remaining > 0) {
                sent = sendfile(fd, file_fd, &offset, (size_t)remaining);
                if (sent <= 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                remaining -= sent;
                total     += sent;
            }
        }
        close(file_fd);
        return total;
    }

    /* Inline body */
    char *buf;
    ssize_t len = http_response_serialise(res, req, &buf);
    if (len < 0) return -1;
    int rc = write_all(fd, buf, (size_t)len);
    free(buf);
    return rc == 0 ? len : -1;
}

/* ─── Convenience helpers ────────────────────────────────────────────────── */

/* Error page built inline in http_send_error() */

void http_send_error(int fd, const http_request_t *req,
                     http_status_t status, const char *detail)
{
    if (!detail) detail = http_status_text(status);
    const char *text = http_status_text(status);

    /* Build error page explicitly to avoid -Wformat-nonliteral */
    char body[2048];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\"><head><meta charset=\"UTF-8\">"
        "<title>%d %s</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;max-width:600px;margin:80px auto;"
        "padding:0 20px;color:#333}"
        "h1{font-size:2rem;border-bottom:1px solid #ddd;padding-bottom:.5rem}"
        "p{color:#555}footer{margin-top:3rem;font-size:.8rem;color:#999}"
        "</style></head><body>"
        "<h1>%d %s</h1>"
        "<p>%s</p>"
        "<footer>" SERVER_NAME "/" SERVER_VERSION "</footer>"
        "</body></html>\n",
        (int)status, text, (int)status, text, detail);
    (void)blen;

    http_response_t res;
    http_response_init(&res);
    res.status = status;
    http_response_set_body(&res, body, strlen(body));
    http_response_add_header(&res, "Content-Type", "text/html; charset=utf-8");
    if (status == HTTP_405_METHOD_NOT_ALLOWED) {
        http_response_add_header(&res, "Allow", "GET, HEAD, OPTIONS");
    }

    http_response_send(&res, req, fd);
    http_response_free(&res);
}

void http_send_redirect(int fd, const http_request_t *req,
                        http_status_t status, const char *location)
{
    http_response_t res;
    http_response_init(&res);
    res.status = status;
    http_response_add_header(&res, "Location", location);

    char body[512];
    snprintf(body, sizeof(body),
             "<html><body>Redirecting to <a href=\"%s\">%s</a></body></html>",
             location, location);
    http_response_set_body(&res, body, strlen(body));
    http_response_add_header(&res, "Content-Type", "text/html; charset=utf-8");

    http_response_send(&res, req, fd);
    http_response_free(&res);
}
