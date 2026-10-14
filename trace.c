#include "cachelab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>

/* Error-reporting helper: format into a stack buffer, then strdup into *errmsg.
 * Returns -1 so call sites can "goto fail" in one expression. */
static int set_errmsg(char **errmsg, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';
    *errmsg = strdup(buf);
    return -1;
}

/* ---------- Trace parser ---------- */

/* Parse a decimal size_t, return 0 on failure with *endpos set */
static int parse_size(const char *s, size_t *out, char **endpos) {
    unsigned long long val;
    char *ep = NULL;

    errno = 0;
    val = strtoull(s, &ep, 10);
    if (ep == s || !isdigit((unsigned char)*s) ||
        (*ep != '\0' && !isspace((unsigned char)*ep) && *ep != '\n' && *ep != '\r')) {
        *endpos = ep;
        return -1;
    }
    if (errno == ERANGE) {
        *endpos = ep;
        return -1;
    }
    /* Check overflow for size_t */
    if (val > (unsigned long long)(SIZE_MAX)) {
        *endpos = ep;
        return -1;
    }
    *out = (size_t)val;
    *endpos = ep;
    return 0;
}

/* Validate a key: [A-Za-z0-9._-]{1,64} */
static int valid_key(const char *s, size_t len) {
    if (len == 0 || len > KEY_MAX)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int parse_trace(const char *path, request_t **requests_out,
                size_t *count_out, char **errmsg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        set_errmsg(errmsg, "cannot open trace file");
        return -1;
    }

    request_t *requests = NULL;
    size_t cap = 0, count = 0;
    char *line = NULL;
    size_t line_cap = 0;
    int lineno = 0;

    while (1) {
        ssize_t r = getline(&line, &line_cap, f);
        if (r < 0)
            break;
        lineno++;

        /* Trim trailing newline/carriage return */
        size_t len = (size_t)r;
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        /* Skip blank and comment lines */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#')
            continue;

        /* Parse operation keyword */
        if (strncmp(p, "GET ", 4) == 0 || strncmp(p, "GET\t", 4) == 0) {
            p += 4;
            while (*p && isspace((unsigned char)*p)) p++;
            const char *key_start = p;
            size_t key_len = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '\n' && *p != '\r') {
                p++;
                key_len++;
            }
            /* Skip trailing whitespace to end of line */
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p != '\0' || key_len == 0) {
                set_errmsg(errmsg, "line %d: malformed GET", lineno);
                goto fail;
            }
            if (!valid_key(key_start, key_len)) {
                set_errmsg(errmsg, "line %d: invalid key", lineno);
                goto fail;
            }

            /* Grow array */
            if (count >= cap) {
                size_t ncap = cap ? cap * 2 : 64;
                request_t *tmp = realloc(requests, ncap * sizeof(request_t));
                if (!tmp) {
                    set_errmsg(errmsg, "allocation failure");
                    goto fail;
                }
                requests = tmp;
                cap = ncap;
            }
            request_t *rq = &requests[count];
            memset(rq, 0, sizeof(*rq));
            rq->sequence = count;
            rq->type = REQUEST_GET;
            strncpy(rq->key, key_start, key_len);
            rq->key[key_len] = '\0';
            count++;

        } else if (strncmp(p, "PUT ", 4) == 0 || strncmp(p, "PUT\t", 4) == 0) {
            p += 4;
            while (*p && isspace((unsigned char)*p)) p++;
            const char *key_start = p;
            size_t key_len = 0;
            while (*p && !isspace((unsigned char)*p) && *p != '\n' && *p != '\r') {
                p++;
                key_len++;
            }
            if (key_len == 0) {
                set_errmsg(errmsg, "line %d: malformed PUT (no key)", lineno);
                goto fail;
            }
            if (!valid_key(key_start, key_len)) {
                set_errmsg(errmsg, "line %d: invalid key", lineno);
                goto fail;
            }
            /* Skip whitespace before size */
            while (*p && isspace((unsigned char)*p)) p++;
            size_t size_val = 0;
            char *end = NULL;
            if (parse_size(p, &size_val, &end) != 0 || size_val == 0) {
                set_errmsg(errmsg, "line %d: invalid size", lineno);
                goto fail;
            }
            p = end;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p != '\0') {
                set_errmsg(errmsg, "line %d: extra fields after PUT", lineno);
                goto fail;
            }

            if (count >= cap) {
                size_t ncap = cap ? cap * 2 : 64;
                request_t *tmp = realloc(requests, ncap * sizeof(request_t));
                if (!tmp) {
                    set_errmsg(errmsg, "allocation failure");
                    goto fail;
                }
                requests = tmp;
                cap = ncap;
            }
            request_t *rq = &requests[count];
            memset(rq, 0, sizeof(*rq));
            rq->sequence = count;
            rq->type = REQUEST_PUT;
            strncpy(rq->key, key_start, key_len);
            rq->key[key_len] = '\0';
            rq->size = size_val;
            count++;
        } else {
            set_errmsg(errmsg, "line %d: unknown operation", lineno);
            goto fail;
        }
    }

    free(line);
    fclose(f);

    /* Shrink to exact size */
    request_t *tmp = realloc(requests, count * sizeof(request_t));
    if (tmp) requests = tmp;

    *requests_out = requests;
    *count_out = count;
    return 0;

fail:
    free(line);
    fclose(f);
    free(requests);
    return -1;
}
