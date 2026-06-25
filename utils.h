#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdbool.h>
#include <time.h>

/* String utilities */
char  *str_trim(char *s);
bool   str_starts_with(const char *s, const char *prefix);
bool   str_ends_with(const char *s, const char *suffix);
int    str_to_int(const char *s, int *out);
char  *str_to_lower(char *s);

/* HTTP date: RFC 7231 §7.1.1.1  "Mon, 01 Jan 2024 00:00:00 GMT" */
void   http_date_now(char *buf, size_t len);
void   http_date_from_time(char *buf, size_t len, time_t t);
time_t http_date_parse(const char *date_str);

/* Format a human-readable file size (e.g. "4.2 MB") */
void   format_size(char *buf, size_t len, off_t bytes);

/* Safe snprintf wrapper: returns true if output was fully written */
bool   snprintf_safe(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Write all bytes (handles EINTR + short writes). Returns 0 or -1. */
int    write_all(int fd, const void *buf, size_t len);

/* Set a file descriptor to non-blocking mode */
int    fd_set_nonblocking(int fd);

/* Set socket keep-alive options */
int    socket_set_keepalive(int fd);

/* Daemonise the process */
int    daemonise(const char *pid_file);

#endif /* UTILS_H */

/* ─── safe_copy: silences -Wstringop-truncation false positives ───────────
 * Copies at most (dst_size-1) bytes then always NUL-terminates.          */
#define safe_copy(dst, src, dst_size) \
    do { \
        size_t _n = strlen(src); \
        size_t _m = (dst_size) - 1; \
        if (_n > _m) _n = _m; \
        memcpy((dst), (src), _n); \
        (dst)[_n] = '\0'; \
    } while (0)
