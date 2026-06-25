#define _GNU_SOURCE
/*
 * mime.c — MIME type table
 */

#include <string.h>
#include <ctype.h>
#include "mime.h"

typedef struct { const char *ext; const char *type; } mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    /* Text */
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "shtml", "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8"  },
    { "txt",   "text/plain; charset=utf-8"},
    { "md",    "text/markdown; charset=utf-8" },
    { "xml",   "text/xml; charset=utf-8"  },
    { "csv",   "text/csv; charset=utf-8"  },
    { "ics",   "text/calendar"            },
    { "vcf",   "text/vcard"               },

    /* Scripts / data */
    { "js",    "application/javascript; charset=utf-8" },
    { "mjs",   "application/javascript; charset=utf-8" },
    { "json",  "application/json; charset=utf-8"       },
    { "jsonld","application/ld+json"                   },
    { "wasm",  "application/wasm"                      },

    /* Images */
    { "png",   "image/png"  },
    { "jpg",   "image/jpeg" },
    { "jpeg",  "image/jpeg" },
    { "gif",   "image/gif"  },
    { "webp",  "image/webp" },
    { "svg",   "image/svg+xml" },
    { "ico",   "image/x-icon"  },
    { "avif",  "image/avif"    },
    { "bmp",   "image/bmp"     },
    { "tif",   "image/tiff"    },
    { "tiff",  "image/tiff"    },

    /* Fonts */
    { "woff",  "font/woff"  },
    { "woff2", "font/woff2" },
    { "ttf",   "font/ttf"   },
    { "otf",   "font/otf"   },
    { "eot",   "application/vnd.ms-fontobject" },

    /* Audio */
    { "mp3",   "audio/mpeg" },
    { "ogg",   "audio/ogg"  },
    { "wav",   "audio/wav"  },
    { "flac",  "audio/flac" },
    { "aac",   "audio/aac"  },
    { "opus",  "audio/opus" },

    /* Video */
    { "mp4",   "video/mp4"  },
    { "webm",  "video/webm" },
    { "ogv",   "video/ogg"  },
    { "mov",   "video/quicktime" },
    { "avi",   "video/x-msvideo" },

    /* Documents / archives */
    { "pdf",   "application/pdf"   },
    { "zip",   "application/zip"   },
    { "gz",    "application/gzip"  },
    { "tar",   "application/x-tar" },
    { "bz2",   "application/x-bzip2" },
    { "xz",    "application/x-xz"  },
    { "7z",    "application/x-7z-compressed" },
    { "doc",   "application/msword" },
    { "docx",  "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { "xls",   "application/vnd.ms-excel" },
    { "xlsx",  "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
    { "ppt",   "application/vnd.ms-powerpoint" },
    { "pptx",  "application/vnd.openxmlformats-officedocument.presentationml.presentation" },

    /* Misc */
    { "bin",   "application/octet-stream" },
    { "exe",   "application/octet-stream" },
    { "dmg",   "application/octet-stream" },
    { "apk",   "application/vnd.android.package-archive" },
    { "swf",   "application/x-shockwave-flash" },
    { "atom",  "application/atom+xml" },
    { "rss",   "application/rss+xml" },
    { "xhtml", "application/xhtml+xml" },

    { NULL, NULL }
};

const char *mime_type_from_extension(const char *ext)
{
    if (!ext) return "application/octet-stream";

    /* Skip leading dot */
    if (*ext == '.') ext++;
    if (*ext == '\0') return "application/octet-stream";

    /* Case-insensitive linear search (small table, good cache behaviour) */
    char lower[32];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && ext[i]; i++) {
        lower[i] = (char)tolower((unsigned char)ext[i]);
    }
    lower[i] = '\0';

    for (const mime_entry_t *e = MIME_TABLE; e->ext; e++) {
        if (strcmp(e->ext, lower) == 0) return e->type;
    }
    return "application/octet-stream";
}

const char *mime_extension_from_path(const char *path)
{
    if (!path) return NULL;
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '.') dot = p;
        else if (*p == '/' || *p == '\\') dot = NULL;
    }
    return dot; /* May be NULL */
}
