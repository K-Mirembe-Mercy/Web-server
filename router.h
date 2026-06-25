#ifndef ROUTER_H
#define ROUTER_H

/*
 * router.h — URL router for built-in API endpoints
 *
 * Allows registering handler functions for specific path prefixes.
 * The file handler is the fallback for any unmatched route.
 *
 * Usage:
 *   router_init();
 *   router_register("/api/status", handle_status);
 *   router_register("/api/ping",   handle_ping);
 *   ...
 *   router_dispatch(ctx, client, req);   // in request loop
 */

#include "server.h"
#include "http_parser.h"
#include "http_response.h"

/* Handler function signature */
typedef http_status_t (*route_handler_t)(const server_ctx_t *ctx,
                                          const client_t     *client,
                                          const http_request_t *req);

#define MAX_ROUTES 64

/* Initialise the router (call once at startup) */
void router_init(void);

/* Register a handler for an exact path or prefix.
 * If prefix=true, matches any path that starts with `path`.
 * Returns 0 on success, -1 if the table is full. */
int router_register(const char *path, bool prefix, route_handler_t handler);

/* Dispatch a request: find a matching route and call its handler.
 * Falls back to the static file handler if no route matches.
 * Returns the HTTP status code sent. */
http_status_t router_dispatch(const server_ctx_t *ctx,
                               const client_t     *client,
                               const http_request_t *req);

/* ── Built-in handlers ───────────────────────────────────────────────────── */

/* GET /api/status  — JSON server stats */
http_status_t route_status(const server_ctx_t *ctx,
                            const client_t     *client,
                            const http_request_t *req);

/* GET /api/ping  — Health check (returns 200 "pong") */
http_status_t route_ping(const server_ctx_t *ctx,
                          const client_t     *client,
                          const http_request_t *req);

/* GET /api/config  — Dump running configuration as JSON */
http_status_t route_config(const server_ctx_t *ctx,
                            const client_t     *client,
                            const http_request_t *req);

#endif /* ROUTER_H */
