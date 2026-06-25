/*
 * router.c — URL router + built-in API endpoints
 *
 * Provides:
 *   GET /api/status  → JSON server statistics
 *   GET /api/ping    → {"pong": true}
 *   GET /api/config  → JSON dump of running config
 *
 * All other paths fall through to the static file handler.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "router.h"
#include "file_handler.h"
#include "logger.h"
#include "json_config.h"
#include "utils.h"

/* ─── Route table ────────────────────────────────────────────────────────── */

typedef struct {
    char            path[MAX_PATH_LEN];
    bool            prefix;
    route_handler_t handler;
} route_t;

static route_t  g_routes[MAX_ROUTES];
static int      g_route_count = 0;
static time_t   g_start_time  = 0;

/* ─── Init ───────────────────────────────────────────────────────────────── */

void router_init(void)
{
    memset(g_routes, 0, sizeof(g_routes));
    g_route_count = 0;
    g_start_time  = time(NULL);

    /* Register built-in endpoints */
    router_register("/api/status", false, route_status);
    router_register("/api/ping",   false, route_ping);
    router_register("/api/config", false, route_config);
}

int router_register(const char *path, bool prefix, route_handler_t handler)
{
    if (g_route_count >= MAX_ROUTES) {
        LOG_ERROR("router: route table full (max %d)", MAX_ROUTES);
        return -1;
    }
    route_t *r = &g_routes[g_route_count++];
    safe_copy(r->path, path, MAX_PATH_LEN);
    r->prefix  = prefix;
    r->handler = handler;
    LOG_DEBUG("router: registered %s%s", path, prefix ? "*" : "");
    return 0;
}

/* ─── Dispatch ───────────────────────────────────────────────────────────── */

http_status_t router_dispatch(const server_ctx_t *ctx,
                               const client_t     *client,
                               const http_request_t *req)
{
    for (int i = 0; i < g_route_count; i++) {
        const route_t *r = &g_routes[i];
        bool match = r->prefix
            ? str_starts_with(req->path, r->path)
            : (strcmp(req->path, r->path) == 0);
        if (match) {
            LOG_DEBUG("router: %s → route[%d] %s", req->path, i, r->path);
            return r->handler(ctx, client, req);
        }
    }
    /* No route matched — serve static file */
    return file_handler_serve(ctx, client, req);
}

/* ─── Helper: send JSON response ─────────────────────────────────────────── */

static http_status_t send_json(const client_t *client,
                                const http_request_t *req,
                                http_status_t status,
                                const char *json,
                                size_t json_len)
{
    http_response_t res;
    http_response_init(&res);
    res.status = status;
    http_response_set_body(&res, json, json_len);
    http_response_add_header(&res, "Content-Type",
                              "application/json; charset=utf-8");
    http_response_add_header(&res, "Cache-Control", "no-store");
    http_response_add_header(&res, "X-Content-Type-Options", "nosniff");
    ssize_t sent = http_response_send(&res, req, client->fd);
    http_response_free(&res);
    logger_access(client, req, status, (size_t)(sent > 0 ? sent : 0));
    return status;
}

/* ─── /api/ping ──────────────────────────────────────────────────────────── */

http_status_t route_ping(const server_ctx_t *ctx,
                          const client_t     *client,
                          const http_request_t *req)
{
    (void)ctx;
    const char *body = "{\"pong\":true,\"server\":\"" SERVER_NAME "\"}\n";
    return send_json(client, req, HTTP_200_OK, body, strlen(body));
}

/* ─── /api/status ────────────────────────────────────────────────────────── */

http_status_t route_status(const server_ctx_t *ctx,
                            const client_t     *client,
                            const http_request_t *req)
{
    time_t  now     = time(NULL);
    long    uptime  = (long)(now - g_start_time);
    long    days    = uptime / 86400;
    long    hours   = (uptime % 86400) / 3600;
    long    mins    = (uptime % 3600)  / 60;
    long    secs    = uptime % 60;

    char date_buf[64];
    http_date_now(date_buf, sizeof(date_buf));

    char body[1024];
    int  n = snprintf(body, sizeof(body),
        "{\n"
        "  \"server\":           \"" SERVER_NAME "\",\n"
        "  \"version\":          \"" SERVER_VERSION "\",\n"
        "  \"status\":           \"running\",\n"
        "  \"timestamp\":        \"%s\",\n"
        "  \"uptime_seconds\":   %ld,\n"
        "  \"uptime_human\":     \"%ldd %ldh %ldm %lds\",\n"
        "  \"total_requests\":   %llu,\n"
        "  \"total_bytes_sent\": %llu,\n"
        "  \"worker_threads\":   %d,\n"
        "  \"port\":             %u,\n"
        "  \"root_dir\":         \"%s\",\n"
        "  \"keep_alive\":       %s,\n"
        "  \"directory_listing\":%s\n"
        "}\n",
        date_buf,
        uptime,
        days, hours, mins, secs,
        (unsigned long long)ctx->total_requests,
        (unsigned long long)ctx->total_bytes_sent,
        ctx->config.worker_threads,
        ctx->config.port,
        ctx->config.root_dir,
        ctx->config.keep_alive        ? "true" : "false",
        ctx->config.directory_listing ? "true" : "false");

    if (n < 0 || (size_t)n >= sizeof(body)) {
        http_send_error(client->fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
        return HTTP_500_INTERNAL_SERVER_ERROR;
    }

    return send_json(client, req, HTTP_200_OK, body, (size_t)n);
}

/* ─── /api/config ────────────────────────────────────────────────────────── */

http_status_t route_config(const server_ctx_t *ctx,
                            const client_t     *client,
                            const http_request_t *req)
{
    /* Reuse json_config_dump but capture to buffer */
    char body[2048];
    int  n = snprintf(body, sizeof(body),
        "{\n"
        "  \"host\":              \"%s\",\n"
        "  \"port\":              %u,\n"
        "  \"root_dir\":          \"%s\",\n"
        "  \"index_file\":        \"%s\",\n"
        "  \"directory_listing\": %s,\n"
        "  \"keep_alive\":        %s,\n"
        "  \"worker_threads\":    %d,\n"
        "  \"backlog\":           %d,\n"
        "  \"cors_enabled\":      %s,\n"
        "  \"cors_origin\":       \"%s\"\n"
        "}\n",
        ctx->config.host,
        ctx->config.port,
        ctx->config.root_dir,
        ctx->config.index_file,
        ctx->config.directory_listing ? "true" : "false",
        ctx->config.keep_alive        ? "true" : "false",
        ctx->config.worker_threads,
        ctx->config.backlog,
        ctx->config.cors_enabled      ? "true" : "false",
        ctx->config.cors_origin);

    if (n < 0 || (size_t)n >= sizeof(body)) {
        http_send_error(client->fd, req, HTTP_500_INTERNAL_SERVER_ERROR, NULL);
        return HTTP_500_INTERNAL_SERVER_ERROR;
    }

    return send_json(client, req, HTTP_200_OK, body, (size_t)n);
}
