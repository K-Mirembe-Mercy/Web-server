#define _GNU_SOURCE
/*
 * utils.c — Miscellaneous utility functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "utils.h"

/* ─── String utilities ───────────────────────────────────────────────────── */

char *str_trim(char *s)
{
    if (!s) return s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

bool str_starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return false;
    size_t sl = strlen(s), pl = strlen(suffix);
    return sl >= pl && strcmp(s + sl - pl, suffix) == 0;
}

int str_to_int(const char *s, int *out)
{
    if (!s || !out) return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || end == s) return -1;
    *out = (int)v;
    return 0;
}

char *str_to_lower(char *s)
{
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

/* ─── HTTP date formatting ───────────────────────────────────────────────── */

/* RFC 7231 §7.1.1.1 fixed-date format: "Mon, 01 Jan 2024 00:00:00 GMT" */
static const char *DAYS[]   = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char *MONTHS[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec" };

void http_date_from_time(char *buf, size_t len, time_t t)
{
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    snprintf(buf, len, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             DAYS[tm_info.tm_wday],
             tm_info.tm_mday,
             MONTHS[tm_info.tm_mon],
             tm_info.tm_year + 1900,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

void http_date_now(char *buf, size_t len)
{
    http_date_from_time(buf, len, time(NULL));
}

time_t http_date_parse(const char *date_str)
{
    if (!date_str) return (time_t)-1;
    struct tm tm_info;
    memset(&tm_info, 0, sizeof(tm_info));
    /* Try RFC 1123: "Mon, 01 Jan 2024 00:00:00 GMT" */
    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm_info)) {
        return timegm(&tm_info);
    }
    /* Try RFC 850: "Monday, 01-Jan-24 00:00:00 GMT" */
    if (strptime(date_str, "%A, %d-%b-%y %H:%M:%S GMT", &tm_info)) {
        return timegm(&tm_info);
    }
    /* Try ANSI C: "Mon Jan  1 00:00:00 2024" */
    if (strptime(date_str, "%a %b %e %H:%M:%S %Y", &tm_info)) {
        return timegm(&tm_info);
    }
    return (time_t)-1;
}

/* ─── File size formatting ───────────────────────────────────────────────── */

void format_size(char *buf, size_t len, off_t bytes)
{
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double d = (double)bytes;
    int ui = 0;
    while (d >= 1024.0 && ui < 4) { d /= 1024.0; ui++; }
    if (ui == 0)
        snprintf(buf, len, "%lld B", (long long)bytes);
    else
        snprintf(buf, len, "%.1f %s", d, units[ui]);
}

/* ─── Safe snprintf ──────────────────────────────────────────────────────── */

bool snprintf_safe(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return (n >= 0 && (size_t)n < size);
}

/* ─── Write helpers ──────────────────────────────────────────────────────── */

int write_all(int fd, const void *buf, size_t len)
{
    const char *p   = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p         += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* ─── Socket / fd options ────────────────────────────────────────────────── */

int fd_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int socket_set_keepalive(int fd)
{
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) < 0)
        return -1;
#ifdef TCP_KEEPIDLE
    int idle = 60;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = 10;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt, sizeof(cnt));
#endif
    return 0;
}

/* ─── Daemonise ──────────────────────────────────────────────────────────── */

int daemonise(const char *pid_file)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS);   /* parent exits */

    if (setsid() < 0) return -1;

    /* Second fork to detach from terminal */
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) exit(EXIT_SUCCESS);

    /* Redirect stdin/stdout/stderr */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }

    /* Write PID file */
    if (pid_file && *pid_file) {
        FILE *f = fopen(pid_file, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
    return 0;
}
