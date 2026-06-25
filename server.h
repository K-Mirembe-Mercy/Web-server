#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ─── Version ────────────────────────────────────────────────────────────── */
#define SERVER_VERSION_MAJOR 1
#define SERVER_VERSION_MINOR 0
#define SERVER_VERSION_PATCH 0
#define SERVER_VERSION "1.0.0"
#define SERVER_NAME    "CedarHTTP"

/* ─── Limits ─────────────────────────────────────────────────────────────── */
#define MAX_CONNECTIONS       1024
#define MAX_HEADERS            64
#define MAX_HEADER_NAME_LEN   128
#define MAX_HEADER_VALUE_LEN  4096
#define MAX_URI_LEN           8192
#define MAX_METHOD_LEN          16
#define MAX_PATH_LEN          4096
#define MAX_QUERY_LEN         4096
#define READ_BUFFER_SIZE     65536   /* 64 KiB */
#define SEND_BUFFER_SIZE     65536
#define MAX_BODY_SIZE      (10 * 1024 * 1024)  /* 10 MiB */

/* ─── Timeouts (seconds) ─────────────────────────────────────────────────── */
#define CLIENT_TIMEOUT_SEC    30
#define KEEPALIVE_TIMEOUT_SEC 75
#define KEEPALIVE_MAX_REQ     100

/* ─── HTTP Methods ───────────────────────────────────────────────────────── */
typedef enum {
    HTTP_METHOD_GET     = 0,
    HTTP_METHOD_HEAD    = 1,
    HTTP_METHOD_POST    = 2,
    HTTP_METHOD_PUT     = 3,
    HTTP_METHOD_DELETE  = 4,
    HTTP_METHOD_OPTIONS = 5,
    HTTP_METHOD_PATCH   = 6,
    HTTP_METHOD_UNKNOWN = 7
} http_method_t;

/* ─── HTTP Versions ──────────────────────────────────────────────────────── */
typedef enum {
    HTTP_VERSION_1_0 = 10,
    HTTP_VERSION_1_1 = 11,
    HTTP_VERSION_UNKNOWN = 0
} http_version_t;

/* ─── HTTP Status Codes ──────────────────────────────────────────────────── */
typedef enum {
    HTTP_200_OK                    = 200,
    HTTP_201_CREATED               = 201,
    HTTP_204_NO_CONTENT            = 204,
    HTTP_206_PARTIAL_CONTENT       = 206,
    HTTP_301_MOVED_PERMANENTLY     = 301,
    HTTP_302_FOUND                 = 302,
    HTTP_304_NOT_MODIFIED          = 304,
    HTTP_400_BAD_REQUEST           = 400,
    HTTP_403_FORBIDDEN             = 403,
    HTTP_404_NOT_FOUND             = 404,
    HTTP_405_METHOD_NOT_ALLOWED    = 405,
    HTTP_408_REQUEST_TIMEOUT       = 408,
    HTTP_411_LENGTH_REQUIRED       = 411,
    HTTP_413_PAYLOAD_TOO_LARGE     = 413,
    HTTP_414_URI_TOO_LONG          = 414,
    HTTP_416_RANGE_NOT_SATISFIABLE = 416,
    HTTP_500_INTERNAL_SERVER_ERROR = 500,
    HTTP_501_NOT_IMPLEMENTED       = 501,
    HTTP_503_SERVICE_UNAVAILABLE   = 503,
    HTTP_505_VERSION_NOT_SUPPORTED = 505
} http_status_t;

/* ─── Header pair ────────────────────────────────────────────────────────── */
typedef struct {
    char name[MAX_HEADER_NAME_LEN];
    char value[MAX_HEADER_VALUE_LEN];
} http_header_t;

/* ─── HTTP Request ───────────────────────────────────────────────────────── */
typedef struct {
    http_method_t  method;
    http_version_t version;
    char           uri[MAX_URI_LEN];
    char           path[MAX_PATH_LEN];
    char           query[MAX_QUERY_LEN];
    http_header_t  headers[MAX_HEADERS];
    int            header_count;
    char          *body;
    size_t         body_len;
    bool           keep_alive;
} http_request_t;

/* ─── HTTP Response ──────────────────────────────────────────────────────── */
typedef struct {
    http_status_t  status;
    http_header_t  headers[MAX_HEADERS];
    int            header_count;
    char          *body;
    size_t         body_len;
    bool           body_is_file;   /* body points to a mmap'd region */
    bool           chunked;
} http_response_t;

/* ─── Client connection ──────────────────────────────────────────────────── */
typedef struct {
    int                 fd;
    struct sockaddr_in  addr;
    char                ip[INET_ADDRSTRLEN];
    uint16_t            port;
    time_t              connected_at;
    time_t              last_activity;
    int                 requests_served;
    bool                keep_alive;
    char                read_buf[READ_BUFFER_SIZE];
    size_t              read_len;
} client_t;

/* ─── Server config ──────────────────────────────────────────────────────── */
typedef struct {
    char     host[256];
    uint16_t port;
    char     root_dir[MAX_PATH_LEN];
    char     log_file[MAX_PATH_LEN];
    char     error_log[MAX_PATH_LEN];
    char     index_file[64];
    bool     directory_listing;
    bool     keep_alive;
    bool     gzip;                  /* future: compression */
    int      worker_threads;
    int      backlog;
    size_t   max_body_size;
    bool     daemon;
    char     pid_file[MAX_PATH_LEN];
    bool     cors_enabled;
    char     cors_origin[256];
} server_config_t;

/* ─── Server context ─────────────────────────────────────────────────────── */
typedef struct {
    int             listen_fd;
    server_config_t config;
    volatile bool   running;
    uint64_t        total_requests;
    uint64_t        total_bytes_sent;
} server_ctx_t;

/* ─── Forward declarations ───────────────────────────────────────────────── */
int  server_init(server_ctx_t *ctx, const server_config_t *cfg);
int  server_run(server_ctx_t *ctx);
void server_stop(server_ctx_t *ctx);
void server_destroy(server_ctx_t *ctx);

#endif /* SERVER_H */
