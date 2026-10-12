#include "toml_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *where_name(toml_datum_t d) {
    return d.source != NULL ? d.source : "<toml>";
}

/* The file and line already name the place, so a context label is only added
 * when the caller has something more specific to say (which system, which
 * recipe). */
void toml_err(toml_datum_t where, const char *fmt, ...) {
    fprintf(stderr, "%s:%d: ", where_name(where), where.lineno);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void toml_err_ctx(toml_datum_t where, const char *ctx, const char *fmt, ...) {
    fprintf(stderr, "%s:%d: ", where_name(where), where.lineno);
    if (ctx != NULL && ctx[0] != '\0')
        fprintf(stderr, "%s: ", ctx);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int toml_doc_load(toml_doc_t *d, const char *path) {
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "retrolunar: cannot read %s\n", path);
        return -1;
    }
    d->res = toml_parse_file_named(f, path);
    fclose(f);
    if (!d->res.ok) {
        fprintf(stderr, "retrolunar: %s: %s\n", path, d->res.errmsg);
        return -1;
    }
    d->path = xstrdup(path);
    return 0;
}

void toml_doc_free(toml_doc_t *d) {
    if (d->path == NULL)
        return;
    toml_free(d->res);
    free(d->path);
    d->path = NULL;
}

toml_datum_t toml_doc_root(const toml_doc_t *d) { return d->res.toptab; }

const char *toml_type_name(toml_type_t t) {
    switch (t) {
    case TOML_STRING:
        return "string";
    case TOML_INT64:
        return "integer";
    case TOML_FP64:
        return "float";
    case TOML_BOOLEAN:
        return "boolean";
    case TOML_DATE:
        return "date";
    case TOML_TIME:
        return "time";
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        return "datetime";
    case TOML_ARRAY:
        return "array";
    case TOML_TABLE:
        return "table";
    default:
        return "nothing";
    }
}

toml_datum_t toml_req_table(toml_datum_t parent, const char *key, const char *ctx) {
    toml_datum_t d = toml_get(parent, key);
    if (d.type == TOML_UNKNOWN) {
        toml_err_ctx(parent, ctx, "no [%s] section", key);
        return d;
    }
    if (d.type != TOML_TABLE) {
        toml_err_ctx(d, ctx, "[%s] must be a table, found %s", key, toml_type_name(d.type));
        d.type = TOML_UNKNOWN;
    }
    return d;
}

toml_datum_t toml_opt_table(toml_datum_t parent, const char *key) { return toml_get(parent, key); }

char *toml_scalar_to_str(toml_datum_t d) {
    char buf[64];
    switch (d.type) {
    case TOML_STRING:
        return xstrdup(d.u.str.ptr);
    case TOML_INT64:
        snprintf(buf, sizeof buf, "%lld", (long long)d.u.int64);
        return xstrdup(buf);
    case TOML_FP64:
        snprintf(buf, sizeof buf, "%g", d.u.fp64);
        return xstrdup(buf);
    case TOML_BOOLEAN:
        return xstrdup(d.u.boolean ? "true" : "false");
    default:
        return NULL;
    }
}

static const char *req_str_type(toml_datum_t parent, const char *key, const char *ctx,
                                toml_type_t want) {
    toml_datum_t d = toml_get(parent, key);
    if (d.type == TOML_UNKNOWN) {
        toml_err_ctx(parent, ctx, "missing key '%s'", key);
        return NULL;
    }
    if (d.type != want) {
        toml_err_ctx(d, ctx, "key '%s' must be %s, found %s", key, toml_type_name(want),
                 toml_type_name(d.type));
        return NULL;
    }
    return d.u.str.ptr;
}

const char *toml_req_str(toml_datum_t parent, const char *key, const char *ctx) {
    return req_str_type(parent, key, ctx, TOML_STRING);
}

const char *toml_opt_str(toml_datum_t parent, const char *key) {
    toml_datum_t d = toml_get(parent, key);
    if (d.type != TOML_STRING)
        return NULL;
    return d.u.str.ptr;
}

static int strarray(toml_datum_t parent, const char *key, const char *ctx, strlist_t *out,
                    int required) {
    toml_datum_t d = toml_get(parent, key);
    if (d.type == TOML_UNKNOWN) {
        if (required) {
            toml_err_ctx(parent, ctx, "missing key '%s'", key);
            return -1;
        }
        return 0;
    }
    if (d.type != TOML_ARRAY) {
        toml_err_ctx(d, ctx, "key '%s' must be an array, found %s", key, toml_type_name(d.type));
        return -1;
    }
    for (int i = 0; i < d.u.arr.size; i++) {
        toml_datum_t e = d.u.arr.elem[i];
        if (e.type != TOML_STRING) {
            toml_err_ctx(e, ctx, "'%s[%d]' must be a string, found %s", key, i,
                     toml_type_name(e.type));
            return -1;
        }
        strlist_add(out, e.u.str.ptr);
    }
    return 0;
}

int toml_req_strarray(toml_datum_t parent, const char *key, const char *ctx, strlist_t *out) {
    return strarray(parent, key, ctx, out, 1);
}

int toml_opt_strarray(toml_datum_t parent, const char *key, strlist_t *out) {
    return strarray(parent, key, NULL, out, 0);
}

int toml_check_keys(toml_datum_t table, const char *const *known, const char *ctx) {
    if (table.type != TOML_TABLE)
        return 0;
    int bad = 0;
    for (int i = 0; i < table.u.tab.size; i++) {
        const char *k = table.u.tab.key[i];
        int ok = 0;
        for (size_t j = 0; known[j] != NULL; j++) {
            if (strcmp(k, known[j]) == 0) {
                ok = 1;
                break;
            }
        }
        if (!ok) {
            toml_err_ctx(table.u.tab.value[i], ctx, "unknown key '%s'", k);
            bad++;
        }
    }
    return bad;
}

void toml_iter_init(toml_iter_t *it, toml_datum_t table) {
    it->tab = table;
    it->i = 0;
}

int toml_iter_next(toml_iter_t *it, const char **key, toml_datum_t *value) {
    if (it->tab.type != TOML_TABLE || it->i >= it->tab.u.tab.size)
        return 0;
    *key = it->tab.u.tab.key[it->i];
    *value = it->tab.u.tab.value[it->i];
    it->i++;
    return 1;
}
