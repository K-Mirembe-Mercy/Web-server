#define _GNU_SOURCE
/*
 * main.c — CedarHTTP entry point
 *
 * Usage: cedar [options] [config_file]
 *
 * Options:
 *   -p <port>      Override port
 *   -r <dir>       Override document root
 *   -h <host>      Override bind address
 *   -t <threads>   Number of worker threads
 *   -d             Run as daemon
 *   -v             Verbose (debug) logging
 *   -D             Dump effective configuration and exit
 *   --version      Print version and exit
 *   --help         Print usage and exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/stat.h>

#include "server.h"
#include "config.h"
#include "json_config.h"
#include "logger.h"
#include "utils.h"

/* ─── Banner ─────────────────────────────────────────────────────────────── */

static void print_banner(void)
{
    printf(
        "\n"
        "  ██████╗███████╗██████╗  █████╗ ██████╗ \n"
        " ██╔════╝██╔════╝██╔══██╗██╔══██╗██╔══██╗\n"
        " ██║     █████╗  ██║  ██║███████║██████╔╝\n"
        " ██║     ██╔══╝  ██║  ██║██╔══██║██╔══██╗\n"
        " ╚██████╗███████╗██████╔╝██║  ██║██║  ██║\n"
        "  ╚═════╝╚══════╝╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝\n"
        "\n"
        "  " SERVER_NAME " v" SERVER_VERSION
        " — A high-performance HTTP/1.1 server written in C\n\n"
    );
}

/* ─── Usage ──────────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] [config_file]\n\n", prog);
    printf("Options:\n");
    printf("  -p <port>      Bind port (default: 8080)\n");
    printf("  -r <dir>       Document root directory (default: ./static)\n");
    printf("  -h <host>      Bind address (default: 0.0.0.0)\n");
    printf("  -t <threads>   Worker threads (default: 4)\n");
    printf("  -d             Daemonise (run in background)\n");
    printf("  -v             Verbose / debug logging\n");
    printf("  -D             Dump configuration and exit\n");
    printf("  --version      Print version and exit\n");
    printf("  --help         Print this help and exit\n\n");
    printf("Config file uses INI syntax:\n");
    printf("  port             = 8080\n");
    printf("  root_dir         = ./static\n");
    printf("  directory_listing= on\n");
    printf("  cors_enabled     = on\n\n");
}

/* ─── Ensure directories exist ───────────────────────────────────────────── */

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return;
    mkdir(path, 0755);
}

/* ─── Main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    server_config_t cfg;
    config_set_defaults(&cfg);

    bool dump_config  = false;
    bool verbose      = false;
    const char *prog  = argv[0];

    /* Long options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", SERVER_NAME, SERVER_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0) {
            print_banner();
            print_usage(prog);
            return 0;
        }
    }

    /* Short options */
    int opt;
    while ((opt = getopt(argc, argv, "p:r:h:t:dvD")) != -1) {
        switch (opt) {
            case 'p': {
                int p;
                if (str_to_int(optarg, &p) < 0 || p <= 0 || p > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", optarg);
                    return 1;
                }
                cfg.port = (uint16_t)p;
                break;
            }
            case 'r':
                strncpy(cfg.root_dir, optarg, sizeof(cfg.root_dir) - 1);
                break;
            case 'h':
                strncpy(cfg.host, optarg, sizeof(cfg.host) - 1);
                break;
            case 't': {
                int t;
                if (str_to_int(optarg, &t) < 0 || t <= 0) {
                    fprintf(stderr, "Invalid thread count: %s\n", optarg);
                    return 1;
                }
                cfg.worker_threads = t;
                break;
            }
            case 'd': cfg.daemon     = true;  break;
            case 'v': verbose        = true;  break;
            case 'D': dump_config    = true;  break;
            default:
                print_usage(prog);
                return 1;
        }
    }

    /* Optional positional config file — auto-detect JSON vs INI by extension */
    if (optind < argc) {
        const char *cfgpath = argv[optind];
        int rc;
        if (str_ends_with(cfgpath, ".json")) {
            rc = json_config_load(cfgpath, &cfg);
        } else {
            rc = config_load(cfgpath, &cfg);
        }
        if (rc < 0) return 1;
    }

    if (dump_config) {
        config_dump(&cfg);
        return 0;
    }

    /* Validate */
    if (config_validate(&cfg) < 0) return 1;

    print_banner();

    /* Ensure log directory exists */
    ensure_dir("./logs");

    /* Initialise logger */
    if (logger_init(cfg.log_file, cfg.error_log) < 0) return 1;
    if (verbose) logger_set_level(LOG_DEBUG);

    /* Daemonise if requested */
    if (cfg.daemon) {
        if (daemonise(cfg.pid_file) < 0) {
            LOG_ERROR("Failed to daemonise");
            return 1;
        }
        LOG_INFO("Daemonised (PID %d)", (int)getpid());
    }

    /* Initialise and run */
    server_ctx_t ctx;
    if (server_init(&ctx, &cfg) < 0) {
        LOG_ERROR("Failed to initialise server");
        return 1;
    }

    server_run(&ctx);
    server_stop(&ctx);
    server_destroy(&ctx);
    logger_close();

    return 0;
}
