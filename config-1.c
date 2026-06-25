/*
 * config.c — INI-style configuration file parser
 *
 * Format:
 *   # comment
 *   key = value
 *
 * Keys are case-insensitive; values are trimmed.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#include "config.h"
#include "utils.h"

void config_set_defaults(server_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host,       "0.0.0.0",  sizeof(cfg->host) - 1);
    cfg->port             = 8080;
    strncpy(cfg->root_dir,   "./static",  sizeof(cfg->root_dir) - 1);
    strncpy(cfg->log_file,   "./logs/access.log", sizeof(cfg->log_file) - 1);
    strncpy(cfg->error_log,  "./logs/error.log",  sizeof(cfg->error_log) - 1);
    strncpy(cfg->index_file, "index.html", sizeof(cfg->index_file) - 1);
    cfg->directory_listing = false;
    cfg->keep_alive        = true;
    cfg->gzip              = false;
    cfg->worker_threads    = 4;
    cfg->backlog           = 128;
    cfg->max_body_size     = MAX_BODY_SIZE;
    cfg->daemon            = false;
    strncpy(cfg->pid_file, "./cedar.pid", sizeof(cfg->pid_file) - 1);
    cfg->cors_enabled = false;
    strncpy(cfg->cors_origin, "*", sizeof(cfg->cors_origin) - 1);
}

static void apply_key(const char *key, const char *value, server_config_t *cfg)
{
    if (strcasecmp(key, "host") == 0)
        strncpy(cfg->host, value, sizeof(cfg->host) - 1);
    else if (strcasecmp(key, "port") == 0)
        str_to_int(value, (int *)&cfg->port);
    else if (strcasecmp(key, "root_dir") == 0)
        strncpy(cfg->root_dir, value, sizeof(cfg->root_dir) - 1);
    else if (strcasecmp(key, "log_file") == 0)
        strncpy(cfg->log_file, value, sizeof(cfg->log_file) - 1);
    else if (strcasecmp(key, "error_log") == 0)
        strncpy(cfg->error_log, value, sizeof(cfg->error_log) - 1);
    else if (strcasecmp(key, "index_file") == 0)
        strncpy(cfg->index_file, value, sizeof(cfg->index_file) - 1);
    else if (strcasecmp(key, "directory_listing") == 0)
        cfg->directory_listing = (strcasecmp(value, "on") == 0 ||
                                   strcasecmp(value, "true") == 0 ||
                                   strcmp(value, "1") == 0);
    else if (strcasecmp(key, "keep_alive") == 0)
        cfg->keep_alive = (strcasecmp(value, "on") == 0 ||
                           strcasecmp(value, "true") == 0 ||
                           strcmp(value, "1") == 0);
    else if (strcasecmp(key, "worker_threads") == 0)
        str_to_int(value, &cfg->worker_threads);
    else if (strcasecmp(key, "backlog") == 0)
        str_to_int(value, &cfg->backlog);
    else if (strcasecmp(key, "daemon") == 0)
        cfg->daemon = (strcasecmp(value, "on") == 0 ||
                       strcasecmp(value, "true") == 0 ||
                       strcmp(value, "1") == 0);
    else if (strcasecmp(key, "pid_file") == 0)
        strncpy(cfg->pid_file, value, sizeof(cfg->pid_file) - 1);
    else if (strcasecmp(key, "cors_enabled") == 0)
        cfg->cors_enabled = (strcasecmp(value, "on") == 0 ||
                              strcasecmp(value, "true") == 0 ||
                              strcmp(value, "1") == 0);
    else if (strcasecmp(key, "cors_origin") == 0)
        strncpy(cfg->cors_origin, value, sizeof(cfg->cors_origin) - 1);
    /* unknown keys are silently ignored */
}

int config_load(const char *path, server_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char line[1024];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = str_trim(line);

        /* Skip blank lines and comments */
        if (!*p || *p == '#' || *p == ';') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: missing '='\n", path, lineno);
            fclose(f);
            return -1;
        }

        *eq = '\0';
        char *key = str_trim(p);
        char *val = str_trim(eq + 1);

        if (!*key) {
            fprintf(stderr, "%s:%d: empty key\n", path, lineno);
            fclose(f);
            return -1;
        }

        apply_key(key, val, cfg);
    }

    fclose(f);
    return 0;
}

void config_dump(const server_config_t *cfg)
{
    printf("# " SERVER_NAME " configuration\n");
    printf("host             = %s\n",  cfg->host);
    printf("port             = %u\n",  cfg->port);
    printf("root_dir         = %s\n",  cfg->root_dir);
    printf("log_file         = %s\n",  cfg->log_file);
    printf("error_log        = %s\n",  cfg->error_log);
    printf("index_file       = %s\n",  cfg->index_file);
    printf("directory_listing= %s\n",  cfg->directory_listing ? "on" : "off");
    printf("keep_alive       = %s\n",  cfg->keep_alive ? "on" : "off");
    printf("worker_threads   = %d\n",  cfg->worker_threads);
    printf("backlog          = %d\n",  cfg->backlog);
    printf("daemon           = %s\n",  cfg->daemon ? "on" : "off");
    printf("pid_file         = %s\n",  cfg->pid_file);
    printf("cors_enabled     = %s\n",  cfg->cors_enabled ? "on" : "off");
    printf("cors_origin      = %s\n",  cfg->cors_origin);
}

int config_validate(const server_config_t *cfg)
{
    if (cfg->port == 0) {
        fprintf(stderr, "config: invalid port %u\n", cfg->port);
        return -1;
    }
    if (cfg->worker_threads < 1 || cfg->worker_threads > 256) {
        fprintf(stderr, "config: worker_threads must be 1-256\n");
        return -1;
    }
    struct stat st;
    if (stat(cfg->root_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "config: root_dir '%s' is not a directory\n",
                cfg->root_dir);
        return -1;
    }
    return 0;
}
