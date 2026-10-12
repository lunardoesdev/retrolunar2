#include "env.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* A growable string, for building PATH-like values without guessing sizes. */
typedef struct {
    char *s;
    size_t len, cap;
} buf_t;

static void buf_init(buf_t *b) {
    b->cap = 256;
    b->len = 0;
    b->s = xmalloc(b->cap);
    b->s[0] = '\0';
}

static void buf_addf(buf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (need < 0) {
        va_end(ap);
        return;
    }
    while (b->len + (size_t)need + 1 > b->cap) {
        b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
    vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
    b->len += (size_t)need;
    va_end(ap);
}

static char *fmt_alloc(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *s = xmalloc((size_t)need + 1);
    vsnprintf(s, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* Join with `sep`, dropping the separator's trailing instance. */
static char *join(const strlist_t *l, const char *sep) {
    buf_t b;
    buf_init(&b);
    for (size_t i = 0; i < l->n; i++)
        buf_addf(&b, "%s%s", i > 0 ? sep : "", l->v[i]);
    return b.s;
}

/* `NAME="<value><sep>${NAME:-}"`: the previous value is kept, and `${NAME:-}`
 * rather than `$NAME` because these variables are often unset and the script
 * runs under `set -u`. */
static char *prepend(const char *name, const char *value, const char *sep) {
    if (value[0] == '\0')
        return fmt_alloc("%s=\"${%s:-}\"", name, name);
    return fmt_alloc("%s=\"%s%s${%s:-}\"", name, value, sep, name);
}

static int node_index(const graph_t *g, const char *key) {
    for (size_t i = 0; i < g->n; i++)
        if (strcmp(g->nodes[i].key, key) == 0)
            return (int)i;
    return -1;
}

/* The graph index a dependency string refers to, or -1: bare names inherit the
 * system of the package that named them, exactly as resolution does. */
static int dep_index(const graph_t *g, size_t idx, const char *dep) {
    const char *at = strrchr(dep, '@');
    char key[512];
    if (at != NULL)
        snprintf(key, sizeof key, "%s", dep);
    else
        snprintf(key, sizeof key, "%s@%s", dep, g->nodes[idx].pkg.sys);
    return node_index(g, key);
}

void env_dep_dirs(const graph_t *g, size_t idx, strlist_t *out) {
    pkg_t *p = &g->nodes[idx].pkg;
    for (size_t d = 0; d < p->deps.n; d++) {
        int di = dep_index(g, idx, p->deps.v[d]);
        if (di < 0)
            continue;
        char *expr = fmt_alloc("$NESTDIR/%s", g->nodes[di].key);
        if (!strlist_has(out, expr))
            strlist_add_own(out, expr);
        else
            free(expr);
    }
}

void env_lines(const graph_t *g, size_t idx, strlist_t *out) {
    const char *mysys = g->nodes[idx].pkg.sys;
    strlist_t deps;
    strlist_init(&deps);
    env_dep_dirs(g, idx, &deps);

    strlist_t inc, lib, pkgcfg, cmake, hostpath, hostld;
    strlist_init(&inc);
    strlist_init(&lib);
    strlist_init(&pkgcfg);
    strlist_init(&cmake);
    strlist_init(&hostpath);
    strlist_init(&hostld);

    for (size_t d = 0; d < deps.n; d++) {
        const char *dir = deps.v[d];
        const char *at = strrchr(dir, '@');
        const char *dsys = at != NULL ? at + 1 : "";
        if (strcmp(dsys, mysys) == 0) {
            /* Same system: this is a link-time dependency. Order is the
             * closure's order, dependencies first.
             *
             * lib64 is not decoration: meson installs there on this host, and a
             * .pc file nobody can find is a dependency nobody can use. */
            strlist_add_own(&inc, fmt_alloc("-I%s/include", dir));
            strlist_add_own(&lib, fmt_alloc("-L%s/lib", dir));
            strlist_add_own(&lib, fmt_alloc("-L%s/lib64", dir));
            strlist_add_own(&pkgcfg, fmt_alloc("%s/lib/pkgconfig", dir));
            strlist_add_own(&pkgcfg, fmt_alloc("%s/lib64/pkgconfig", dir));
            strlist_add_own(&pkgcfg, fmt_alloc("%s/share/pkgconfig", dir));
            strlist_add_own(&cmake, fmt_alloc("%s", dir));
        } else {
            /* Another system: a host tool. It contributes runnable programs and
             * shared libraries, never link paths — a cross build must not link
             * against host libraries, and the v1 rule that a cross toolchain
             * never goes on PATH still holds. */
            strlist_add_own(&hostpath, fmt_alloc("%s/bin", dir));
            strlist_add_own(&hostld, fmt_alloc("%s/lib", dir));
            strlist_add_own(&hostld, fmt_alloc("%s/lib64", dir));
        }
    }

    char *deps_v = join(&deps, " ");
    char *inc_v = join(&inc, " ");
    char *lib_v = join(&lib, " ");
    char *pkg_v = join(&pkgcfg, ":");
    char *cmake_v = join(&cmake, ";");
    char *path_v = join(&hostpath, ":");
    char *ld_v = join(&hostld, ":");

    /* Existing values are kept: a system fragment appends to these, and a
     * caller's own flags must not disappear. An empty contribution leaves the
     * variable exactly as it was, without a stray separator. */
    /* An empty contribution is left out entirely: a line that only says
     * `CPPFLAGS="$CPPFLAGS"` is noise in a script the user is meant to read. */
    strlist_add_own(out, fmt_alloc("DEPS=\"%s\"", deps_v));
    if (inc_v[0] != '\0')
        strlist_add_own(out, prepend("CPPFLAGS", inc_v, " "));
    if (lib_v[0] != '\0')
        strlist_add_own(out, prepend("LDFLAGS", lib_v, " "));
    if (pkg_v[0] != '\0')
        strlist_add_own(out, prepend("PKG_CONFIG_LIBDIR", pkg_v, ":"));
    /* Always set: our .pc files are the only ones that count. */
    strlist_add(out, "PKG_CONFIG_PATH=\"\"");
    if (cmake_v[0] != '\0')
        strlist_add_own(out, prepend("CMAKE_PREFIX_PATH", cmake_v, ";"));
    if (path_v[0] != '\0')
        strlist_add_own(out, prepend("PATH", path_v, ":"));
    if (ld_v[0] != '\0')
        strlist_add_own(out, prepend("LD_LIBRARY_PATH", ld_v, ":"));
    strlist_add(out, "export DEPS CPPFLAGS LDFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_PATH "
                     "CMAKE_PREFIX_PATH PATH LD_LIBRARY_PATH");

    free(deps_v);
    free(inc_v);
    free(lib_v);
    free(pkg_v);
    free(cmake_v);
    free(path_v);
    free(ld_v);
    strlist_free(&deps);
    strlist_free(&inc);
    strlist_free(&lib);
    strlist_free(&pkgcfg);
    strlist_free(&cmake);
    strlist_free(&hostpath);
    strlist_free(&hostld);
}
