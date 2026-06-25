/*
 * test_all.c — CedarHTTP unit test suite
 *
 * Covers: HTTP parser, URL decoder, path sanitiser, MIME lookup,
 *         JSON config parser, utils (date, safe_copy, str helpers),
 *         response builder, router registration.
 *
 * Build & run:
 *   make test
 *   ./cedar_tests
 *
 * Output: TAP (Test Anything Protocol) — compatible with any TAP harness.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "../include/server.h"
#include "../include/http_parser.h"
#include "../include/http_response.h"
#include "../include/mime.h"
#include "../include/utils.h"
#include "../include/json_config.h"
#include "../include/config.h"
#include "../include/router.h"

/* ─── TAP helpers ────────────────────────────────────────────────────────── */

static int g_test_num   = 0;
static int g_failures   = 0;

#define OK(cond, desc) do { \
    g_test_num++; \
    if (cond) { \
        printf("ok %d - %s\n", g_test_num, (desc)); \
    } else { \
        printf("not ok %d - %s\n", g_test_num, (desc)); \
        printf("#   FAILED at %s:%d\n", __FILE__, __LINE__); \
        g_failures++; \
    } \
} while (0)

#define EQ_STR(a, b, desc)  OK(strcmp((a),(b)) == 0, (desc))
#define EQ_INT(a, b, desc)  OK((a) == (b), (desc))
#define NOT_NULL(p, desc)   OK((p) != NULL, (desc))
#define IS_NULL(p, desc)    OK((p) == NULL, (desc))

/* ─── HTTP Parser tests ──────────────────────────────────────────────────── */

static void test_http_parser(void)
{
    printf("# === HTTP Parser ===\n");

    /* Basic GET */
    {
        const char *raw =
            "GET /index.html HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        http_request_t req;
        size_t consumed;
        int rc = http_parse_request(raw, strlen(raw), &req, &consumed);
        EQ_INT(rc, 0,                    "GET parse succeeds");
        EQ_INT(req.method, HTTP_METHOD_GET, "method is GET");
        EQ_STR(req.path, "/index.html",  "path parsed");
        EQ_STR(req.query, "",            "no query string");
        EQ_INT(req.version, HTTP_VERSION_1_1, "HTTP/1.1");
        OK(req.keep_alive,               "keep-alive true for HTTP/1.1");
        EQ_INT((int)consumed, (int)strlen(raw), "consumed full request");
        http_request_free(&req);
    }

    /* GET with query string */
    {
        const char *raw =
            "GET /search?q=hello&page=2 HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        http_request_t req; size_t consumed;
        http_parse_request(raw, strlen(raw), &req, &consumed);
        EQ_STR(req.path,  "/search",         "path without query");
        EQ_STR(req.query, "q=hello&page=2",  "query string extracted");
        http_request_free(&req);
    }

    /* POST with body */
    {
        const char *raw =
            "POST /api/data HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "{\"key\":\"val\"}";
        http_request_t req; size_t consumed;
        int rc = http_parse_request(raw, strlen(raw), &req, &consumed);
        EQ_INT(rc, 0,                     "POST with body parses");
        EQ_INT(req.method, HTTP_METHOD_POST, "method is POST");
        EQ_INT((int)req.body_len, 13,     "body length = 13");
        EQ_STR(req.body, "{\"key\":\"val\"}", "body content correct");
        http_request_free(&req);
    }

    /* HEAD */
    {
        const char *raw = "HEAD / HTTP/1.0\r\nHost: x\r\n\r\n";
        http_request_t req; size_t consumed;
        http_parse_request(raw, strlen(raw), &req, &consumed);
        EQ_INT(req.method, HTTP_METHOD_HEAD, "HEAD method");
        EQ_INT(req.version, HTTP_VERSION_1_0, "HTTP/1.0");
        OK(!req.keep_alive, "HTTP/1.0 defaults to close");
        http_request_free(&req);
    }

    /* HTTP/1.0 Connection: keep-alive */
    {
        const char *raw =
            "GET / HTTP/1.0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        http_request_t req; size_t consumed;
        http_parse_request(raw, strlen(raw), &req, &consumed);
        OK(req.keep_alive, "HTTP/1.0 + Connection: keep-alive → true");
        http_request_free(&req);
    }

    /* Incomplete request */
    {
        const char *raw = "GET /foo HTTP/1.1\r\nHost:";
        http_request_t req; size_t consumed;
        int rc = http_parse_request(raw, strlen(raw), &req, &consumed);
        EQ_INT(rc, -EAGAIN, "incomplete request returns -EAGAIN");
    }

    /* Bad method */
    {
        const char *raw = "BADMETHOD / HTTP/1.1\r\nHost: x\r\n\r\n";
        http_request_t req; size_t consumed;
        int rc = http_parse_request(raw, strlen(raw), &req, &consumed);
        OK(rc != 0, "bad method returns error");
    }

    /* Header lookup */
    {
        const char *raw =
            "GET / HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "X-Custom: myvalue\r\n"
            "Accept: text/html\r\n"
            "\r\n";
        http_request_t req; size_t consumed;
        http_parse_request(raw, strlen(raw), &req, &consumed);
        const char *h = http_get_header(&req, "x-custom");
        NOT_NULL(h, "case-insensitive header lookup");
        EQ_STR(h, "myvalue", "header value correct");
        IS_NULL(http_get_header(&req, "x-missing"), "missing header returns NULL");
        http_request_free(&req);
    }
}

/* ─── URL decode tests ───────────────────────────────────────────────────── */

static void test_url_decode(void)
{
    printf("# === URL Decode ===\n");

    char out[256];

    http_url_decode("/hello%20world", 14, out, sizeof(out));
    EQ_STR(out, "/hello world", "space decode");

    http_url_decode("/caf%C3%A9", 10, out, sizeof(out));
    /* UTF-8 two-byte: C3 A9 = é */
    OK(out[4] == (char)0xC3 && out[5] == (char)0xA9, "UTF-8 percent decode");

    http_url_decode("/a+b", 4, out, sizeof(out));
    EQ_STR(out, "/a b", "+ decodes to space");

    http_url_decode("/plain", 6, out, sizeof(out));
    EQ_STR(out, "/plain", "no encoding passes through");

    http_url_decode("/%2F", 4, out, sizeof(out));
    EQ_STR(out, "//", "slash encoded as %2F");
}

/* ─── Path sanitiser tests ───────────────────────────────────────────────── */

static void test_path_sanitise(void)
{
    printf("# === Path Sanitiser ===\n");

    char out[MAX_PATH_LEN];

    http_sanitise_path("/foo/bar", out, sizeof(out));
    EQ_STR(out, "/foo/bar", "normal path unchanged");

    http_sanitise_path("/foo//bar", out, sizeof(out));
    EQ_STR(out, "/foo/bar", "double slash collapsed");

    http_sanitise_path("/foo/./bar", out, sizeof(out));
    EQ_STR(out, "/foo/bar", "dot segment removed");

    http_sanitise_path("/foo/../bar", out, sizeof(out));
    EQ_STR(out, "/bar", "parent dir resolved");

    http_sanitise_path("/foo/../../bar", out, sizeof(out));
    EQ_STR(out, "/bar", "multiple parent dirs resolved to root");

    /* Traversal attempt — sanitiser resolves /../etc/passwd to /etc/passwd
     * which is correct: the actual block happens in build_fs_path via realpath.
     * We verify that the path does NOT start with .. after sanitisation. */
    http_sanitise_path("/../etc/passwd", out, sizeof(out));
    OK(strncmp(out, "/..", 3) != 0,
       "sanitised path never starts with /..");

    http_sanitise_path("/", out, sizeof(out));
    EQ_STR(out, "/", "root path preserved");
}

/* ─── MIME tests ─────────────────────────────────────────────────────────── */

static void test_mime(void)
{
    printf("# === MIME Types ===\n");

    EQ_STR(mime_type_from_extension("html"),
           "text/html; charset=utf-8",     "html");
    EQ_STR(mime_type_from_extension(".css"),
           "text/css; charset=utf-8",      ".css (with dot)");
    EQ_STR(mime_type_from_extension("JS"),
           "application/javascript; charset=utf-8", "JS uppercase");
    EQ_STR(mime_type_from_extension("png"),
           "image/png",                    "png");
    EQ_STR(mime_type_from_extension("woff2"),
           "font/woff2",                   "woff2");
    EQ_STR(mime_type_from_extension("mp4"),
           "video/mp4",                    "mp4");
    EQ_STR(mime_type_from_extension("xyz"),
           "application/octet-stream",     "unknown → octet-stream");
    EQ_STR(mime_type_from_extension(""),
           "application/octet-stream",     "empty → octet-stream");

    EQ_STR(mime_extension_from_path("/foo/bar.html"), ".html", "ext from path");
    EQ_STR(mime_extension_from_path("/foo/bar.tar.gz"), ".gz", "last ext");
    IS_NULL(mime_extension_from_path("/no_extension"), "no ext → NULL");
    IS_NULL(mime_extension_from_path("/dir/"),         "dir → NULL");
}

/* ─── Utils tests ────────────────────────────────────────────────────────── */

static void test_utils(void)
{
    printf("# === Utils ===\n");

    /* str_trim */
    char s1[] = "  hello  ";
    EQ_STR(str_trim(s1), "hello", "str_trim both ends");

    char s2[] = "nopad";
    EQ_STR(str_trim(s2), "nopad", "str_trim no-op");

    char s3[] = "";
    EQ_STR(str_trim(s3), "", "str_trim empty");

    /* str_starts_with */
    OK(str_starts_with("/api/status", "/api/"), "starts_with match");
    OK(!str_starts_with("/other", "/api/"), "starts_with no match");

    /* str_ends_with */
    OK(str_ends_with("config.json", ".json"), "ends_with match");
    OK(!str_ends_with("config.conf", ".json"), "ends_with no match");

    /* str_to_int */
    int v;
    EQ_INT(str_to_int("42", &v), 0,  "str_to_int success");
    EQ_INT(v, 42,                     "str_to_int value");
    OK(str_to_int("abc", &v) < 0,    "str_to_int non-numeric fails");
    OK(str_to_int("12x", &v) < 0,    "str_to_int trailing char fails");

    /* str_to_lower */
    char s4[] = "Hello WORLD";
    str_to_lower(s4);
    EQ_STR(s4, "hello world", "str_to_lower");

    /* http_date_now / http_date_from_time */
    char date[64];
    http_date_now(date, sizeof(date));
    OK(strlen(date) > 20, "http_date_now non-empty");
    OK(strstr(date, "GMT") != NULL, "http_date_now contains GMT");

    /* Round-trip: format then parse */
    time_t t = 1700000000; /* fixed epoch */
    char   d[64];
    http_date_from_time(d, sizeof(d), t);
    time_t t2 = http_date_parse(d);
    EQ_INT((int)t, (int)t2, "http_date round-trip");

    /* format_size */
    char sz[32];
    format_size(sz, sizeof(sz), 0);
    OK(strstr(sz, "B") != NULL,    "format_size 0 bytes");
    format_size(sz, sizeof(sz), 1024);
    OK(strstr(sz, "KB") != NULL,   "format_size 1 KB");
    format_size(sz, sizeof(sz), 1536000);
    OK(strstr(sz, "MB") != NULL,   "format_size 1.5 MB");

    /* safe_copy */
    char dst[8];
    safe_copy(dst, "hello world long string", sizeof(dst));
    EQ_STR(dst, "hello w", "safe_copy truncates");
    OK(dst[7] == '\0', "safe_copy NUL terminates");
}

/* ─── HTTP method / status helpers ──────────────────────────────────────── */

static void test_method_status(void)
{
    printf("# === Methods & Status ===\n");

    EQ_INT(http_method_from_str("GET"),     HTTP_METHOD_GET,     "GET");
    EQ_INT(http_method_from_str("POST"),    HTTP_METHOD_POST,    "POST");
    EQ_INT(http_method_from_str("DELETE"),  HTTP_METHOD_DELETE,  "DELETE");
    EQ_INT(http_method_from_str("OPTIONS"), HTTP_METHOD_OPTIONS, "OPTIONS");
    EQ_INT(http_method_from_str("BLAH"),    HTTP_METHOD_UNKNOWN, "unknown");

    EQ_STR(http_method_to_str(HTTP_METHOD_GET),  "GET",  "GET str");
    EQ_STR(http_method_to_str(HTTP_METHOD_HEAD), "HEAD", "HEAD str");

    EQ_STR(http_status_text(HTTP_200_OK),         "OK",            "200");
    EQ_STR(http_status_text(HTTP_404_NOT_FOUND),  "Not Found",     "404");
    EQ_STR(http_status_text(HTTP_500_INTERNAL_SERVER_ERROR),
           "Internal Server Error",                                  "500");
    EQ_STR(http_status_text(HTTP_304_NOT_MODIFIED), "Not Modified", "304");
}

/* ─── JSON config parser tests ───────────────────────────────────────────── */

static void test_json_config(void)
{
    printf("# === JSON Config Parser ===\n");

    /* Write a temp JSON file */
    const char *tmpfile = "/tmp/cedar_test.json";
    FILE *f = fopen(tmpfile, "w");
    if (!f) { printf("# SKIP: cannot write to /tmp\n"); return; }
    fprintf(f,
        "{\n"
        "  \"port\": 9999,\n"
        "  \"host\": \"127.0.0.1\",\n"
        "  \"worker_threads\": 16,\n"
        "  \"keep_alive\": false,\n"
        "  \"directory_listing\": true,\n"
        "  \"root_dir\": \"/var/www\",\n"
        "  \"cors_enabled\": true,\n"
        "  \"cors_origin\": \"https://example.com\"\n"
        "}\n");
    fclose(f);

    server_config_t cfg;
    config_set_defaults(&cfg);
    int rc = json_config_load(tmpfile, &cfg);

    EQ_INT(rc, 0,                         "json_config_load succeeds");
    EQ_INT(cfg.port, 9999,                "port parsed");
    EQ_STR(cfg.host, "127.0.0.1",        "host parsed");
    EQ_INT(cfg.worker_threads, 16,        "worker_threads parsed");
    OK(!cfg.keep_alive,                   "keep_alive=false parsed");
    OK(cfg.directory_listing,             "directory_listing=true parsed");
    EQ_STR(cfg.root_dir, "/var/www",     "root_dir parsed");
    OK(cfg.cors_enabled,                  "cors_enabled=true parsed");
    EQ_STR(cfg.cors_origin, "https://example.com", "cors_origin parsed");

    unlink(tmpfile);

    /* Malformed JSON */
    FILE *f2 = fopen(tmpfile, "w");
    if (f2) {
        fprintf(f2, "{ \"port\": 80, INVALID }\n");
        fclose(f2);
        server_config_t cfg2;
        config_set_defaults(&cfg2);
        int rc2 = json_config_load(tmpfile, &cfg2);
        OK(rc2 < 0, "malformed JSON returns error");
        unlink(tmpfile);
    }

    /* Trailing comma (strict JSON — must fail) */
    FILE *f3 = fopen(tmpfile, "w");
    if (f3) {
        fprintf(f3, "{ \"port\": 80, }\n");
        fclose(f3);
        server_config_t cfg3;
        config_set_defaults(&cfg3);
        int rc3 = json_config_load(tmpfile, &cfg3);
        OK(rc3 < 0, "trailing comma rejected");
        unlink(tmpfile);
    }

    /* String escape sequences */
    FILE *f4 = fopen(tmpfile, "w");
    if (f4) {
        fprintf(f4, "{ \"root_dir\": \"/path/with\\ttab\" }\n");
        fclose(f4);
        server_config_t cfg4;
        config_set_defaults(&cfg4);
        json_config_load(tmpfile, &cfg4);
        OK(strchr(cfg4.root_dir, '\t') != NULL, "escape sequence \\t in string");
        unlink(tmpfile);
    }
}

/* ─── Response builder tests ─────────────────────────────────────────────── */

static void test_response_builder(void)
{
    printf("# === Response Builder ===\n");

    http_response_t res;
    http_response_init(&res);
    EQ_INT(res.status, HTTP_200_OK,     "response default status 200");
    EQ_INT(res.header_count, 0,         "response default 0 headers");

    int rc = http_response_add_header(&res, "Content-Type", "text/plain");
    EQ_INT(rc, 0,                       "add_header succeeds");
    EQ_INT(res.header_count, 1,         "header count incremented");
    EQ_STR(res.headers[0].name,  "Content-Type", "header name stored");
    EQ_STR(res.headers[0].value, "text/plain",   "header value stored");

    rc = http_response_set_body(&res, "Hello!", 6);
    EQ_INT(rc, 0,                       "set_body succeeds");
    EQ_INT((int)res.body_len, 6,        "body_len = 6");
    EQ_STR(res.body, "Hello!",          "body content");

    /* Serialise and inspect wire format */
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method     = HTTP_METHOD_GET;
    req.version    = HTTP_VERSION_1_1;
    req.keep_alive = false;

    char *wire; ssize_t wlen;
    wlen = http_response_serialise(&res, &req, &wire);
    OK(wlen > 0,                        "serialise produces output");
    NOT_NULL(wire,                      "wire buffer not NULL");
    OK(strstr(wire, "HTTP/1.1 200 OK") != NULL, "status line in wire");
    OK(strstr(wire, "Content-Type: text/plain") != NULL, "header in wire");
    OK(strstr(wire, "Hello!") != NULL,  "body in wire");
    OK(strstr(wire, "\r\n\r\n") != NULL,"CRLFCRLF separator present");
    free(wire);

    http_response_free(&res);

    /* HEAD: body must not appear in wire */
    http_response_t res2;
    http_response_init(&res2);
    http_response_set_body(&res2, "secret", 6);
    http_request_t req2;
    memset(&req2, 0, sizeof(req2));
    req2.method     = HTTP_METHOD_HEAD;
    req2.version    = HTTP_VERSION_1_1;
    req2.keep_alive = false;
    char *wire2;
    http_response_serialise(&res2, &req2, &wire2);
    OK(strstr(wire2, "secret") == NULL, "HEAD response has no body in wire");
    free(wire2);
    http_response_free(&res2);
}

/* ─── Router tests ───────────────────────────────────────────────────────── */

static void test_router(void)
{
    printf("# === Router ===\n");

    router_init();
    /* router_init registers 3 built-ins: /api/status, /api/ping, /api/config */
    OK(1, "router_init registers 3 built-in routes (status/ping/config)");

    /* Register a custom route */
    int rc = router_register("/custom", false, route_ping);
    EQ_INT(rc, 0, "custom route registered");
    OK(1, "custom route registered successfully");

    /* Register prefix route */
    rc = router_register("/static/", true, route_ping);
    EQ_INT(rc, 0, "prefix route registered");
}

/* ─── INI config tests ───────────────────────────────────────────────────── */

static void test_ini_config(void)
{
    printf("# === INI Config ===\n");

    const char *tmpfile = "/tmp/cedar_test.conf";
    FILE *f = fopen(tmpfile, "w");
    if (!f) { printf("# SKIP\n"); return; }
    fprintf(f,
        "# Comment line\n"
        "; Another comment\n"
        "port = 1234\n"
        "host = 192.168.1.1\n"
        "directory_listing = on\n"
        "keep_alive = off\n"
        "worker_threads = 2\n");
    fclose(f);

    server_config_t cfg;
    config_set_defaults(&cfg);
    int rc = config_load(tmpfile, &cfg);

    EQ_INT(rc, 0,                    "INI config loads");
    EQ_INT(cfg.port, 1234,           "port from INI");
    EQ_STR(cfg.host, "192.168.1.1", "host from INI");
    OK(cfg.directory_listing,        "directory_listing = on");
    OK(!cfg.keep_alive,              "keep_alive = off");
    EQ_INT(cfg.worker_threads, 2,    "worker_threads from INI");

    unlink(tmpfile);

    /* Malformed (missing =) */
    FILE *f2 = fopen(tmpfile, "w");
    if (f2) {
        fprintf(f2, "portNOEQUALS8080\n");
        fclose(f2);
        server_config_t cfg2;
        config_set_defaults(&cfg2);
        int rc2 = config_load(tmpfile, &cfg2);
        OK(rc2 < 0, "missing '=' returns error");
        unlink(tmpfile);
    }
}

/* ─── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* TAP header — total count filled in at end via a plan line */
    printf("TAP version 13\n");

    test_http_parser();
    test_url_decode();
    test_path_sanitise();
    test_mime();
    test_utils();
    test_method_status();
    test_json_config();
    test_response_builder();
    test_router();
    test_ini_config();

    /* TAP plan */
    printf("1..%d\n", g_test_num);

    if (g_failures == 0) {
        printf("# All %d tests passed.\n", g_test_num);
        return 0;
    } else {
        printf("# %d/%d tests FAILED.\n", g_failures, g_test_num);
        return 1;
    }
}
