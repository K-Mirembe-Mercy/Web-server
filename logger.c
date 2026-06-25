#define _GNU_SOURCE
/*
 * logger.c — Access + error logging
 * Combined Log Format (Apache/Nginx compatible).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <strings.h>

#include "logger.h"
#include "http_parser.h"
#include "utils.h"

static FILE       *g_access_log = NULL;
static FILE       *g_error_log  = NULL;
static log_level_t g_level      = LOG_INFO;
static pthread_mutex_t g_mutex  = PTHREAD_MUTEX_INITIALIZER;

int logger_init(const char *access_log_path, const char *error_log_path)
{
    if (access_log_path && *access_log_path) {
        g_access_log = fopen(access_log_path, "a");
        if (!g_access_log) {
            perror(access_log_path);
            return -1;
        }
    } else {
        g_access_log = stdout;
    }

    if (error_log_path && *error_log_path) {
        g_error_log = fopen(error_log_path, "a");
        if (!g_error_log) {
            perror(error_log_path);
            return -1;
        }
    } else {
        g_error_log = stderr;
    }

    /* Line-buffer both so entries appear immediately */
    setlinebuf(g_access_log);
    setlinebuf(g_error_log);
    return 0;
}

void logger_close(void)
{
    if (g_access_log && g_access_log != stdout)  fclose(g_access_log);
    if (g_error_log  && g_error_log  != stderr)  fclose(g_error_log);
    g_access_log = NULL;
    g_error_log  = NULL;
}

void logger_set_level(log_level_t level)
{
    g_level = level;
}

/* Apache Combined Log Format:
 * 127.0.0.1 - - [01/Jan/2024:12:00:00 +0000] "GET / HTTP/1.1" 200 1234 */
void logger_access(const client_t *client,
                   const http_request_t *req,
                   http_status_t status,
                   size_t bytes_sent)
{
    if (!g_access_log) return;

    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S +0000", &tm_info);

    const char *referer  = http_get_header(req, "referer");
    const char *ua       = http_get_header(req, "user-agent");

    pthread_mutex_lock(&g_mutex);
    fprintf(g_access_log,
            "%s - - [%s] \"%s %s HTTP/1.%d\" %d %zu \"%s\" \"%s\"\n",
            client->ip,
            timebuf,
            http_method_to_str(req->method),
            req->uri,
            req->version == HTTP_VERSION_1_1 ? 1 : 0,
            (int)status,
            bytes_sent,
            referer  ? referer : "-",
            ua       ? ua      : "-");
    pthread_mutex_unlock(&g_mutex);
}

static const char *level_str(log_level_t l)
{
    switch (l) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

void logger_log(log_level_t level, const char *fmt, ...)
{
    if (level < g_level) return;
    if (!g_error_log)    return;

    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    va_list ap;
    va_start(ap, fmt);

    pthread_mutex_lock(&g_mutex);
    fprintf(g_error_log, "[%s] [%s] ", timebuf, level_str(level));
    vfprintf(g_error_log, fmt, ap);
    fputc('\n', g_error_log);
    pthread_mutex_unlock(&g_mutex);

    va_end(ap);
}
