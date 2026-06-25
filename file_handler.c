/*
 * file_handler.c — Static file serving + directory listing
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#include "file_handler.h"
#include "mime.h"
#include "logger.h"
#include "utils.h"

/* ─── Helpers ────────────────────────────────────────────────────────────── */

/* Build the filesystem path from root_dir + uri_path.
 * Returns 0 on success, -1 if the path is too long or escapes root. */
static int build_fs_path(const char *root_dir, const char *uri_path,
                          char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s%s", root_dir, uri_path);
    if (n < 0 || (size_t)n >= out_len) return -1;

    /* Resolve to absolute path and verify it is within root */
    char real_root[MAX_PATH_LEN], real_path[MAX_PATH_LEN];
    if (!realpath(root_dir, real_root)) return -1;

    /* realpath requires the file to exist; try with existing prefix */
    if (!realpath(out, real_path)) {
        /* File might not exist yet – that's OK, we'll 404 later.
         * We still need to confirm no traversal, so check the dirname. */
        char tmp[MAX_PATH_LEN];
        safe_copy(tmp, out, sizeof(tmp));
        char *slash = strrchr(tmp, '/');
        if (slash) {
            *slash = '\0';
            if (!realpath(tmp, real_path)) return 0; /* can't resolve – let stat fail */
            /* Append original filename */
            snprintf(out, out_len, "%s/%s", real_path, slash + 1);
        }
        return 0;
    }

    if (strncmp(real_path, real_root, strlen(real_root)) != 0) {
        LOG_WARN("Path traversal blocked: %s", out);
        return -1;
    }
    safe_copy(out, real_path, out_len);
    return 0;
}

/* ─── ETag & conditional requests ───────────────────────────────────────── */

static void make_etag(char *buf, size_t len, const struct stat *st)
{
    snprintf(buf, len, "\"%llx-%llx\"",
             (unsigned long long)st->st_mtime,
             (unsigned long long)st->st_size);
}

static bool is_not_modified(const http_request_t *req,
                              const struct stat *st,
                              const char *etag)
{
    const char *inm = http_get_header(req, "if-none-match");
    if (inm && strcmp(inm, etag) == 0) return true;

    const char *ims = http_get_header(req, "if-modified-since");
    if (ims) {
        time_t t = http_date_parse(ims);
        if (t != (time_t)-1 && st->st_mtime <= t) return true;
    }
    return false;
}

/* ─── Send a single file ─────────────────────────────────────────────────── */

static http_status_t serve_file(const server_ctx_t *ctx,
                                 const client_t *client,
                                 const http_request_t *req,
                                 const char *fs_path,
                                 const struct stat *st)
{
    (void)ctx;

    /* Only allow GET / HEAD */
    if (req->method != HTTP_METHOD_GET &&
        req->method != HTTP_METHOD_HEAD) {
        http_send_error(client->fd, req, HTTP_405_METHOD_NOT_ALLOWED, NULL);
        return HTTP_405_METHOD_NOT_ALLOWED;
    }

    char etag[64];
    make_etag(etag, sizeof(etag), st);

    if (is_not_modified(req, st, etag)) {
        http_response_t res;
        http_response_init(&res);
        res.status = HTTP_304_NOT_MODIFIED;
        http_response_add_header(&res, "ETag", etag);
        char lm[64];
        http_date_from_time(lm, sizeof(lm), st->st_mtime);
        http_response_add_header(&res, "Last-Modified", lm);
        ssize_t sent = http_response_send(&res, req, client->fd);
        http_response_free(&res);
        logger_access(client, req, HTTP_304_NOT_MODIFIED, (size_t)(sent > 0 ? sent : 0));
        return HTTP_304_NOT_MODIFIED;
    }

    /* Determine content type */
    const char *ext  = mime_extension_from_path(fs_path);
    const char *mime = mime_type_from_extension(ext ? ext : "");

    /* Build response */
    http_response_t res;
    http_response_init(&res);
    res.status = HTTP_200_OK;

    http_response_add_header(&res, "Content-Type", mime);
    http_response_add_header(&res, "ETag", etag);

    char lm[64];
    http_date_from_time(lm, sizeof(lm), st->st_mtime);
    http_response_add_header(&res, "Last-Modified", lm);

    /* Cache control: 1 hour for most assets, no-cache for HTML */
    if (mime && strncmp(mime, "text/html", 9) == 0) {
        http_response_add_header(&res, "Cache-Control",
                                  "no-cache, must-revalidate");
    } else {
        http_response_add_header(&res, "Cache-Control",
                                  "public, max-age=3600");
    }

    /* CORS headers if enabled */
    if (ctx->config.cors_enabled) {
        http_response_add_header(&res, "Access-Control-Allow-Origin",
                                  ctx->config.cors_origin);
    }

    /* Point the response at the file path (sendfile path) */
    res.body_is_file = true;
    res.body         = (char *)fs_path;   /* temporary – not freed */
    res.body_len     = (size_t)st->st_size;

    ssize_t sent = http_response_send(&res, req, client->fd);
    res.body = NULL;   /* prevent free */
    http_response_free(&res);

    http_status_t status = (sent >= 0) ? HTTP_200_OK
                                        : HTTP_500_INTERNAL_SERVER_ERROR;
    logger_access(client, req, status, (size_t)(sent > 0 ? sent : 0));
    return status;
}

/* ─── Directory listing ──────────────────────────────────────────────────── */

char *dir_listing_generate(const char *fs_path, const char *uri_path)
{
    DIR *dir = opendir(fs_path);
    if (!dir) return NULL;

    /* Collect entries */
    struct dirent **entries;
    int n = scandir(fs_path, &entries, NULL, alphasort);
    if (n < 0) { closedir(dir); return NULL; }
    closedir(dir);

    /* Build HTML */
    size_t cap = 8192;
    char *html = malloc(cap);
    if (!html) goto cleanup;

    size_t pos = 0;
    pos += (size_t)snprintf(html + pos, cap - pos,
        "<!DOCTYPE html><html lang=\"en\"><head>"
        "<meta charset=\"UTF-8\">"
        "<title>Index of %s</title>"
        "<style>"
        "body{font-family:system-ui,monospace;max-width:900px;margin:40px auto;padding:0 20px}"
        "h1{font-size:1.4rem;border-bottom:1px solid #ccc;padding-bottom:.5rem}"
        "table{width:100%%;border-collapse:collapse}"
        "th,td{text-align:left;padding:6px 12px;border-bottom:1px solid #eee}"
        "th{background:#f5f5f5;font-weight:600}"
        "a{text-decoration:none;color:#0060a0}"
        "a:hover{text-decoration:underline}"
        "td.size{text-align:right;color:#888}"
        "td.date{color:#888}"
        "footer{margin-top:2rem;font-size:.8rem;color:#aaa}"
        "</style></head><body>"
        "<h1>Index of %s</h1>"
        "<table><thead><tr>"
        "<th>Name</th><th class=\"date\">Last Modified</th><th class=\"size\">Size</th>"
        "</tr></thead><tbody>",
        uri_path, uri_path);

    /* Parent link */
    if (strcmp(uri_path, "/") != 0) {
        /* Compute parent */
        char parent[MAX_PATH_LEN];
        strncpy(parent, uri_path, sizeof(parent) - 1);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
        } else {
            strncpy(parent, "/", sizeof(parent) - 1);
        }
        pos += (size_t)snprintf(html + pos, cap - pos,
            "<tr><td><a href=\"%s\">../</a></td><td></td><td></td></tr>",
            parent);
    }

    for (int i = 0; i < n; i++) {
        const char *name = entries[i]->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            free(entries[i]); continue;
        }

        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", fs_path, name);

        struct stat st;
        if (stat(full, &st) < 0) { free(entries[i]); continue; }

        bool is_dir = S_ISDIR(st.st_mode);
        char sz[32], dt[64];
        format_size(sz, sizeof(sz), is_dir ? 0 : st.st_size);
        http_date_from_time(dt, sizeof(dt), st.st_mtime);

        /* Grow buffer if needed */
        size_t needed = strlen(name) + strlen(uri_path) + 512;
        if (cap - pos < needed) {
            cap *= 2;
            char *tmp = realloc(html, cap);
            if (!tmp) { free(entries[i]); break; }
            html = tmp;
        }

        /* Build href */
        char href[MAX_PATH_LEN];
        if (strcmp(uri_path, "/") == 0)
            snprintf(href, sizeof(href), "/%s%s", name, is_dir ? "/" : "");
        else
            snprintf(href, sizeof(href), "%s/%s%s", uri_path, name, is_dir ? "/" : "");

        pos += (size_t)snprintf(html + pos, cap - pos,
            "<tr>"
            "<td><a href=\"%s\">%s%s</a></td>"
            "<td class=\"date\">%s</td>"
            "<td class=\"size\">%s</td>"
            "</tr>",
            href, name, is_dir ? "/" : "",
            dt, is_dir ? "-" : sz);

        free(entries[i]);
    }

    pos += (size_t)snprintf(html + pos, cap - pos,
        "</tbody></table>"
        "<footer>" SERVER_NAME "/" SERVER_VERSION "</footer>"
        "</body></html>\n");

cleanup:
    free(entries);
    return html;
}

static http_status_t serve_directory(const server_ctx_t *ctx,
                                      const client_t *client,
                                      const http_request_t *req,
                                      const char *fs_path,
                                      const char *uri_path)
{
    /* Ensure trailing slash redirect */
    size_t ul = strlen(uri_path);
    if (ul > 0 && uri_path[ul-1] != '/') {
        char loc[MAX_URI_LEN];
        snprintf(loc, sizeof(loc), "%s/", uri_path);
        http_send_redirect(client->fd, req, HTTP_301_MOVED_PERMANENTLY, loc);
        logger_access(client, req, HTTP_301_MOVED_PERMANENTLY, 0);
        return HTTP_301_MOVED_PERMANENTLY;
    }

    /* Try index file */
    char index_path[MAX_PATH_LEN + 64];
    snprintf(index_path, sizeof(index_path), "%s%s",
             fs_path, ctx->config.index_file);

    struct stat ist;
    if (stat(index_path, &ist) == 0 && S_ISREG(ist.st_mode)) {
        return serve_file(ctx, client, req, index_path, &ist);
    }

    /* Directory listing */
    if (ctx->config.directory_listing) {
        char *html = dir_listing_generate(fs_path, uri_path);
        if (!html) {
            http_send_error(client->fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
            return HTTP_500_INTERNAL_SERVER_ERROR;
        }
        http_response_t res;
        http_response_init(&res);
        res.status = HTTP_200_OK;
        http_response_set_body(&res, html, strlen(html));
        free(html);
        http_response_add_header(&res, "Content-Type", "text/html; charset=utf-8");
        http_response_add_header(&res, "Cache-Control", "no-store");
        ssize_t sent = http_response_send(&res, req, client->fd);
        http_response_free(&res);
        logger_access(client, req, HTTP_200_OK, (size_t)(sent > 0 ? sent : 0));
        return HTTP_200_OK;
    }

    http_send_error(client->fd, req, HTTP_403_FORBIDDEN, "Directory listing disabled.");
    logger_access(client, req, HTTP_403_FORBIDDEN, 0);
    return HTTP_403_FORBIDDEN;
}

/* ─── Public entry point ─────────────────────────────────────────────────── */

http_status_t file_handler_serve(const server_ctx_t *ctx,
                                  const client_t *client,
                                  const http_request_t *req)
{
    /* OPTIONS pre-flight */
    if (req->method == HTTP_METHOD_OPTIONS) {
        http_response_t res;
        http_response_init(&res);
        res.status = HTTP_200_OK;
        http_response_add_header(&res, "Allow", "GET, HEAD, OPTIONS");
        if (ctx->config.cors_enabled) {
            http_response_add_header(&res, "Access-Control-Allow-Origin",
                                      ctx->config.cors_origin);
            http_response_add_header(&res, "Access-Control-Allow-Methods",
                                      "GET, HEAD, OPTIONS");
        }
        http_response_send(&res, req, client->fd);
        http_response_free(&res);
        logger_access(client, req, HTTP_200_OK, 0);
        return HTTP_200_OK;
    }

    /* Build filesystem path */
    char fs_path[MAX_PATH_LEN];
    if (build_fs_path(ctx->config.root_dir, req->path,
                       fs_path, sizeof(fs_path)) < 0) {
        http_send_error(client->fd, req, HTTP_400_BAD_REQUEST, NULL);
        logger_access(client, req, HTTP_400_BAD_REQUEST, 0);
        return HTTP_400_BAD_REQUEST;
    }

    struct stat st;
    if (stat(fs_path, &st) < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            http_send_error(client->fd, req, HTTP_404_NOT_FOUND,
                            "The requested resource was not found.");
            logger_access(client, req, HTTP_404_NOT_FOUND, 0);
            return HTTP_404_NOT_FOUND;
        }
        if (errno == EACCES) {
            http_send_error(client->fd, req, HTTP_403_FORBIDDEN, NULL);
            logger_access(client, req, HTTP_403_FORBIDDEN, 0);
            return HTTP_403_FORBIDDEN;
        }
        http_send_error(client->fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
        logger_access(client, req, HTTP_500_INTERNAL_SERVER_ERROR, 0);
        return HTTP_500_INTERNAL_SERVER_ERROR;
    }

    if (S_ISDIR(st.st_mode)) {
        return serve_directory(ctx, client, req, fs_path, req->path);
    }

    if (!S_ISREG(st.st_mode)) {
        http_send_error(client->fd, req, HTTP_403_FORBIDDEN,
                        "Not a regular file.");
        logger_access(client, req, HTTP_403_FORBIDDEN, 0);
        return HTTP_403_FORBIDDEN;
    }

    return serve_file(ctx, client, req, fs_path, &st);
}
