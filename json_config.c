/*
 * json_config.c — Hand-rolled JSON configuration parser
 *
 * Parses a single flat JSON object.  No malloc beyond the string buffer.
 * Strict: rejects trailing commas, unquoted keys, comments, etc.
 *
 *  {
 *    "port": 8080,
 *    "host": "0.0.0.0",
 *    "root_dir": "./static",
 *    "worker_threads": 8,
 *    "keep_alive": true,
 *    "directory_listing": false,
 *    "cors_enabled": true,
 *    "cors_origin": "*"
 *  }
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

#include "json_config.h"
#include "config.h"
#include "utils.h"

/* ─── Tokeniser ──────────────────────────────────────────────────────────── */

typedef enum {
    TOK_LBRACE, TOK_RBRACE,
    TOK_COLON,  TOK_COMMA,
    TOK_STRING, TOK_NUMBER, TOK_TRUE, TOK_FALSE, TOK_NULL,
    TOK_EOF,    TOK_ERROR
} tok_type_t;

typedef struct {
    tok_type_t  type;
    char        str[4096];   /* for TOK_STRING / TOK_NUMBER */
    double      num;
} token_t;

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    int         line;
    int         col;
    char        err[512];
} lexer_t;

static void lex_init(lexer_t *l, const char *src, size_t len)
{
    l->src  = src;
    l->pos  = 0;
    l->len  = len;
    l->line = 1;
    l->col  = 1;
    l->err[0] = '\0';
}

static char lex_peek(const lexer_t *l)
{
    return l->pos < l->len ? l->src[l->pos] : '\0';
}

static char lex_advance(lexer_t *l)
{
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; }
    else            { l->col++; }
    return c;
}

static void lex_skip_ws(lexer_t *l)
{
    while (l->pos < l->len && isspace((unsigned char)lex_peek(l)))
        lex_advance(l);
}

/* Parse a JSON string (after opening quote) into out[out_max]. */
static int lex_string(lexer_t *l, char *out, size_t out_max)
{
    size_t wi = 0;
    while (l->pos < l->len) {
        char c = lex_advance(l);
        if (c == '"') { out[wi] = '\0'; return 0; }
        if (c == '\\') {
            if (l->pos >= l->len) break;
            char esc = lex_advance(l);
            char decoded;
            switch (esc) {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case 'n':  decoded = '\n'; break;
                case 'r':  decoded = '\r'; break;
                case 't':  decoded = '\t'; break;
                case 'b':  decoded = '\b'; break;
                case 'f':  decoded = '\f'; break;
                case 'u':
                    /* \uXXXX — just skip 4 hex digits, replace with '?' */
                    for (int i = 0; i < 4 && l->pos < l->len; i++) lex_advance(l);
                    decoded = '?';
                    break;
                default:
                    snprintf(l->err, sizeof(l->err),
                             "Unknown escape \\%c at line %d", esc, l->line);
                    return -1;
            }
            if (wi + 1 < out_max) out[wi++] = decoded;
        } else {
            if (wi + 1 < out_max) out[wi++] = c;
        }
    }
    snprintf(l->err, sizeof(l->err), "Unterminated string at line %d", l->line);
    return -1;
}

static token_t lex_next(lexer_t *l)
{
    token_t tok = { .type = TOK_ERROR };
    lex_skip_ws(l);
    if (l->pos >= l->len) { tok.type = TOK_EOF; return tok; }

    char c = lex_advance(l);
    switch (c) {
        case '{': tok.type = TOK_LBRACE; return tok;
        case '}': tok.type = TOK_RBRACE; return tok;
        case ':': tok.type = TOK_COLON;  return tok;
        case ',': tok.type = TOK_COMMA;  return tok;
        case '"':
            tok.type = lex_string(l, tok.str, sizeof(tok.str)) == 0
                       ? TOK_STRING : TOK_ERROR;
            return tok;
        case 't':
            if (strncmp(l->src + l->pos, "rue", 3) == 0) {
                l->pos += 3; l->col += 3;
                tok.type = TOK_TRUE; return tok;
            }
            break;
        case 'f':
            if (strncmp(l->src + l->pos, "alse", 4) == 0) {
                l->pos += 4; l->col += 4;
                tok.type = TOK_FALSE; return tok;
            }
            break;
        case 'n':
            if (strncmp(l->src + l->pos, "ull", 3) == 0) {
                l->pos += 3; l->col += 3;
                tok.type = TOK_NULL; return tok;
            }
            break;
        default:
            if (c == '-' || isdigit((unsigned char)c)) {
                size_t start = l->pos - 1;
                while (l->pos < l->len &&
                       (isdigit((unsigned char)lex_peek(l)) ||
                        lex_peek(l) == '.' || lex_peek(l) == 'e' ||
                        lex_peek(l) == 'E' || lex_peek(l) == '+' ||
                        lex_peek(l) == '-')) {
                    lex_advance(l);
                }
                size_t nlen = l->pos - start;
                if (nlen >= sizeof(tok.str)) nlen = sizeof(tok.str) - 1;
                memcpy(tok.str, l->src + start, nlen);
                tok.str[nlen] = '\0';
                tok.num  = atof(tok.str);
                tok.type = TOK_NUMBER;
                return tok;
            }
            break;
    }
    snprintf(l->err, sizeof(l->err),
             "Unexpected character '%c' at line %d col %d", c, l->line, l->col);
    return tok;
}

/* ─── Apply key/value to config ──────────────────────────────────────────── */

static void apply_kv(const char *key, const token_t *val,
                     server_config_t *cfg, lexer_t *l)
{
    bool bool_val = (val->type == TOK_TRUE);

    if (strcasecmp(key, "host")              == 0 && val->type == TOK_STRING)  { safe_copy(cfg->host,       val->str, sizeof(cfg->host)); return; }
    if (strcasecmp(key, "root_dir")          == 0 && val->type == TOK_STRING)  { safe_copy(cfg->root_dir,   val->str, sizeof(cfg->root_dir)); return; }
    if (strcasecmp(key, "log_file")          == 0 && val->type == TOK_STRING)  { safe_copy(cfg->log_file,   val->str, sizeof(cfg->log_file)); return; }
    if (strcasecmp(key, "error_log")         == 0 && val->type == TOK_STRING)  { safe_copy(cfg->error_log,  val->str, sizeof(cfg->error_log)); return; }
    if (strcasecmp(key, "index_file")        == 0 && val->type == TOK_STRING)  { safe_copy(cfg->index_file, val->str, sizeof(cfg->index_file)); return; }
    if (strcasecmp(key, "pid_file")          == 0 && val->type == TOK_STRING)  { safe_copy(cfg->pid_file,   val->str, sizeof(cfg->pid_file)); return; }
    if (strcasecmp(key, "cors_origin")       == 0 && val->type == TOK_STRING)  { safe_copy(cfg->cors_origin,val->str, sizeof(cfg->cors_origin)); return; }
    if (strcasecmp(key, "port")              == 0 && val->type == TOK_NUMBER)  { cfg->port            = (uint16_t)val->num; return; }
    if (strcasecmp(key, "worker_threads")    == 0 && val->type == TOK_NUMBER)  { cfg->worker_threads  = (int)val->num;      return; }
    if (strcasecmp(key, "backlog")           == 0 && val->type == TOK_NUMBER)  { cfg->backlog         = (int)val->num;      return; }
    if (strcasecmp(key, "keep_alive")        == 0 && (val->type == TOK_TRUE || val->type == TOK_FALSE)) { cfg->keep_alive         = bool_val; return; }
    if (strcasecmp(key, "directory_listing") == 0 && (val->type == TOK_TRUE || val->type == TOK_FALSE)) { cfg->directory_listing  = bool_val; return; }
    if (strcasecmp(key, "daemon")            == 0 && (val->type == TOK_TRUE || val->type == TOK_FALSE)) { cfg->daemon             = bool_val; return; }
    if (strcasecmp(key, "cors_enabled")      == 0 && (val->type == TOK_TRUE || val->type == TOK_FALSE)) { cfg->cors_enabled       = bool_val; return; }
}

/* ─── Parser ─────────────────────────────────────────────────────────────── */

static int parse_object(lexer_t *l, server_config_t *cfg)
{
    /* Expect opening { */
    token_t tok = lex_next(l);
    if (tok.type != TOK_LBRACE) {
        snprintf(l->err, sizeof(l->err),
                 "Expected '{' at line %d, got type %d", l->line, tok.type);
        return -1;
    }

    tok = lex_next(l);

    /* Empty object */
    if (tok.type == TOK_RBRACE) return 0;

    while (1) {
        /* Key must be a string */
        if (tok.type != TOK_STRING) {
            snprintf(l->err, sizeof(l->err),
                     "Expected string key at line %d", l->line);
            return -1;
        }
        char key[256];
        strncpy(key, tok.str, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        /* Colon */
        tok = lex_next(l);
        if (tok.type != TOK_COLON) {
            snprintf(l->err, sizeof(l->err),
                     "Expected ':' after key \"%s\" at line %d", key, l->line);
            return -1;
        }

        /* Value */
        token_t val = lex_next(l);
        if (val.type == TOK_ERROR || val.type == TOK_EOF) {
            snprintf(l->err, sizeof(l->err),
                     "Expected value for key \"%s\" at line %d", key, l->line);
            return -1;
        }

        apply_kv(key, &val, cfg, l);

        /* Comma or closing brace */
        tok = lex_next(l);
        if (tok.type == TOK_RBRACE) break;
        if (tok.type != TOK_COMMA) {
            snprintf(l->err, sizeof(l->err),
                     "Expected ',' or '}' at line %d", l->line);
            return -1;
        }

        tok = lex_next(l);

        /* Strict: no trailing comma */
        if (tok.type == TOK_RBRACE) {
            snprintf(l->err, sizeof(l->err),
                     "Trailing comma at line %d (not valid JSON)", l->line);
            return -1;
        }
    }
    return 0;
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

int json_config_load(const char *path, server_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz <= 0) { fclose(f); return 0; }

    char *buf = malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)fsz, f);
    buf[got] = '\0';
    fclose(f);

    lexer_t lex;
    lex_init(&lex, buf, got);
    int rc = parse_object(&lex, cfg);
    free(buf);

    if (rc != 0) {
        fprintf(stderr, "json_config: parse error in '%s': %s\n",
                path, lex.err);
        return -2;
    }
    return 0;
}

int json_config_dump(const char *path, const server_config_t *cfg)
{
    FILE *f = path ? fopen(path, "w") : stdout;
    if (!f) { perror(path); return -1; }

    fprintf(f,
        "{\n"
        "  \"host\":              \"%s\",\n"
        "  \"port\":              %u,\n"
        "  \"root_dir\":          \"%s\",\n"
        "  \"log_file\":          \"%s\",\n"
        "  \"error_log\":         \"%s\",\n"
        "  \"index_file\":        \"%s\",\n"
        "  \"directory_listing\": %s,\n"
        "  \"keep_alive\":        %s,\n"
        "  \"worker_threads\":    %d,\n"
        "  \"backlog\":           %d,\n"
        "  \"daemon\":            %s,\n"
        "  \"pid_file\":          \"%s\",\n"
        "  \"cors_enabled\":      %s,\n"
        "  \"cors_origin\":       \"%s\"\n"
        "}\n",
        cfg->host,
        cfg->port,
        cfg->root_dir,
        cfg->log_file,
        cfg->error_log,
        cfg->index_file,
        cfg->directory_listing ? "true" : "false",
        cfg->keep_alive        ? "true" : "false",
        cfg->worker_threads,
        cfg->backlog,
        cfg->daemon            ? "true" : "false",
        cfg->pid_file,
        cfg->cors_enabled      ? "true" : "false",
        cfg->cors_origin);

    if (path) fclose(f);
    return 0;
}

void json_config_print(const server_config_t *cfg)
{
    json_config_dump(NULL, cfg);
}
