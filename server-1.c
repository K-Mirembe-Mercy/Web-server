#define _GNU_SOURCE
/*
 * server.c — Core TCP server: bind, accept loop, thread pool, keep-alive
 *
 * Architecture:
 *   • One accept loop on the main thread.
 *   • A fixed-size thread pool processes client connections.
 *   • Each worker handles one client at a time (blocking I/O).
 *   • Keep-alive is supported: a worker loops until the client closes
 *     the connection or times out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "server.h"
#include "http_parser.h"
#include "http_response.h"
#include "file_handler.h"
#include "router.h"
#include "logger.h"
#include "utils.h"

/* ─── Thread-pool work queue ─────────────────────────────────────────────── */

typedef struct work_item_s {
    client_t            *client;
    struct work_item_s  *next;
} work_item_t;

typedef struct {
    work_item_t     *head;
    work_item_t     *tail;
    int              count;
    int              capacity;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond_work;
    pthread_cond_t   cond_space;
    bool             shutdown;
} work_queue_t;

static work_queue_t  g_queue;
static pthread_t    *g_workers      = NULL;
static int           g_worker_count = 0;
static server_ctx_t *g_server_ctx   = NULL;

/* ─── Queue helpers ──────────────────────────────────────────────────────── */

static int queue_init(work_queue_t *q, int capacity)
{
    memset(q, 0, sizeof(*q));
    q->capacity = capacity;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond_work, NULL);
    pthread_cond_init(&q->cond_space, NULL);
    return 0;
}

static void queue_destroy(work_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond_work);
    pthread_cond_destroy(&q->cond_space);
}

static int queue_push(work_queue_t *q, client_t *client)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count >= q->capacity && !q->shutdown) {
        pthread_cond_wait(&q->cond_space, &q->mutex);
    }
    if (q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    work_item_t *item = malloc(sizeof(work_item_t));
    if (!item) { pthread_mutex_unlock(&q->mutex); return -1; }
    item->client = client;
    item->next   = NULL;
    if (q->tail) q->tail->next = item;
    else         q->head       = item;
    q->tail = item;
    q->count++;
    pthread_cond_signal(&q->cond_work);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static client_t *queue_pop(work_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->shutdown) {
        pthread_cond_wait(&q->cond_work, &q->mutex);
    }
    if (q->shutdown && q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }
    work_item_t *item = q->head;
    q->head = item->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    pthread_cond_signal(&q->cond_space);
    pthread_mutex_unlock(&q->mutex);

    client_t *c = item->client;
    free(item);
    return c;
}

/* ─── Client lifecycle ───────────────────────────────────────────────────── */

static client_t *client_new(int fd, const struct sockaddr_in *addr)
{
    client_t *c = calloc(1, sizeof(client_t));
    if (!c) return NULL;
    c->fd           = fd;
    c->addr         = *addr;
    c->connected_at = time(NULL);
    c->last_activity= time(NULL);
    c->keep_alive   = true;
    inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof(c->ip));
    c->port = ntohs(addr->sin_port);
    return c;
}

static void client_free(client_t *c)
{
    if (c) {
        close(c->fd);
        free(c);
    }
}

/* ─── Wait for data with timeout ─────────────────────────────────────────── */

static int wait_for_data(int fd, int timeout_sec)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r == 0) return -ETIMEDOUT;
    if (r < 0)  return -errno;
    return 0;
}

/* ─── Handle one HTTP request ────────────────────────────────────────────── */

static bool handle_request(server_ctx_t *ctx, client_t *client)
{
    /* Read until we have a complete request or an error */
    while (1) {
        int wait = wait_for_data(client->fd,
                                  client->requests_served == 0
                                      ? CLIENT_TIMEOUT_SEC
                                      : KEEPALIVE_TIMEOUT_SEC);
        if (wait == -ETIMEDOUT) {
            /* Silent timeout on keep-alive idle */
            return false;
        }
        if (wait < 0) return false;

        ssize_t n = read(client->fd,
                         client->read_buf + client->read_len,
                         sizeof(client->read_buf) - client->read_len - 1);
        if (n <= 0) return false;

        client->read_len           += (size_t)n;
        client->read_buf[client->read_len] = '\0';
        client->last_activity = time(NULL);

        http_request_t req;
        size_t consumed;
        int rc = http_parse_request(client->read_buf, client->read_len,
                                    &req, &consumed);
        if (rc == -EAGAIN) {
            /* Need more data */
            if (client->read_len >= sizeof(client->read_buf) - 1) {
                http_send_error(client->fd, NULL,
                                HTTP_413_PAYLOAD_TOO_LARGE, NULL);
                return false;
            }
            continue;
        }
        if (rc != 0) {
            http_send_error(client->fd, NULL, HTTP_400_BAD_REQUEST, NULL);
            http_request_free(&req);
            return false;
        }

        /* Dispatch */
        /* Dispatch through router (API routes + static file fallback) */
        router_dispatch(ctx, client, &req);

        client->requests_served++;

        /* Slide the buffer: pipelined requests */
        if (consumed < client->read_len) {
            memmove(client->read_buf,
                    client->read_buf + consumed,
                    client->read_len - consumed);
            client->read_len -= consumed;
        } else {
            client->read_len = 0;
        }

        bool ka = req.keep_alive
               && g_server_ctx->config.keep_alive
               && client->requests_served < KEEPALIVE_MAX_REQ;

        http_request_free(&req);

        if (!ka) return false;
        return true;   /* keep-alive: come back for next request */
    }
}

/* ─── Worker thread ──────────────────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    while (1) {
        client_t *client = queue_pop(&g_queue);
        if (!client) break;   /* shutdown */

        LOG_DEBUG("Worker handling %s:%u (fd=%d)",
                  client->ip, client->port, client->fd);

        while (handle_request(ctx, client)) {
            /* keep-alive loop */
        }

        LOG_DEBUG("Closing %s:%u after %d request(s)",
                  client->ip, client->port, client->requests_served);
        client_free(client);
    }
    return NULL;
}

/* ─── Signal handling ────────────────────────────────────────────────────── */

static void signal_handler(int sig)
{
    if (g_server_ctx) {
        g_server_ctx->running = false;
        LOG_INFO("Received signal %d — shutting down", sig);
    }
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

int server_init(server_ctx_t *ctx, const server_config_t *cfg)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->config  = *cfg;
    ctx->running = true;
    g_server_ctx = ctx;

    /* Create socket */
    ctx->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /* SO_REUSEADDR / SO_REUSEPORT */
    int opt = 1;
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(ctx->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    /* TCP_NODELAY: disable Nagle for lower latency */
    setsockopt(ctx->listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(cfg->port),
    };
    inet_pton(AF_INET, cfg->host, &addr.sin_addr);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(ctx->listen_fd);
        return -1;
    }

    if (listen(ctx->listen_fd, cfg->backlog) < 0) {
        perror("listen");
        close(ctx->listen_fd);
        return -1;
    }

    /* Thread pool */
    queue_init(&g_queue, cfg->worker_threads * 4);
    g_worker_count = cfg->worker_threads;
    g_workers      = calloc((size_t)g_worker_count, sizeof(pthread_t));
    if (!g_workers) return -1;

    for (int i = 0; i < g_worker_count; i++) {
        if (pthread_create(&g_workers[i], NULL, worker_thread, ctx) != 0) {
            perror("pthread_create");
            return -1;
        }
    }

    /* Signal handlers */
    struct sigaction sa = { .sa_handler = signal_handler };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe */

    /* Initialise URL router + built-in API endpoints */
    router_init();

    LOG_INFO(SERVER_NAME " " SERVER_VERSION
             " listening on %s:%u  (%d workers)",
             cfg->host, cfg->port, g_worker_count);

    return 0;
}

int server_run(server_ctx_t *ctx)
{
    while (ctx->running) {
        /* Use select with a short timeout so we can check ctx->running */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(ctx->listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select: %s", strerror(errno));
            break;
        }
        if (r == 0) continue;   /* timeout – recheck ctx->running */

        struct sockaddr_in caddr;
        socklen_t caddrlen = sizeof(caddr);
        int cfd = accept(ctx->listen_fd, (struct sockaddr *)&caddr, &caddrlen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOG_ERROR("accept: %s", strerror(errno));
            continue;
        }

        /* Set receive timeout on client socket */
        struct timeval ctv = { .tv_sec = CLIENT_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
        socket_set_keepalive(cfd);

        client_t *client = client_new(cfd, &caddr);
        if (!client) {
            close(cfd);
            continue;
        }

        ctx->total_requests++;

        if (queue_push(&g_queue, client) < 0) {
            LOG_WARN("Queue full, dropping connection from %s", client->ip);
            http_send_error(cfd, NULL, HTTP_503_SERVICE_UNAVAILABLE,
                            "Server is busy. Please try again later.");
            client_free(client);
        }
    }

    return 0;
}

void server_stop(server_ctx_t *ctx)
{
    ctx->running = false;

    /* Signal all workers to wake and exit */
    pthread_mutex_lock(&g_queue.mutex);
    g_queue.shutdown = true;
    pthread_cond_broadcast(&g_queue.cond_work);
    pthread_cond_broadcast(&g_queue.cond_space);
    pthread_mutex_unlock(&g_queue.mutex);

    for (int i = 0; i < g_worker_count; i++) {
        pthread_join(g_workers[i], NULL);
    }
    free(g_workers);
    g_workers      = NULL;
    g_worker_count = 0;

    queue_destroy(&g_queue);
}

void server_destroy(server_ctx_t *ctx)
{
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }

    LOG_INFO("Server stopped. Total requests served: %llu",
             (unsigned long long)ctx->total_requests);
}
