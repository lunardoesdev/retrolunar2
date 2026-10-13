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

void env_lines(const graph_t *g, size_t idx, int include_self, strlist_t *out) {
    const char *mysys = g->nodes[idx].pkg.sys;
    strlist_t deps;
    strlist_init(&deps);
    if (include_self)
        strlist_add_own(&deps, fmt_alloc("$NESTDIR/%s", g->nodes[idx].key));
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

    /* One variable per direct dependency, so a recipe can point a build system
     * at a specific dependency: `--with-zlib="$zlib_PREFIX"`. v1's single
     * $PREFIX is gone, and this is what replaces it in a ported recipe. Names
     * are shell identifiers, so punctuation becomes underscores. */
    strlist_t perdep, perdep_assign;
    strlist_init(&perdep);
    strlist_init(&perdep_assign);
    for (size_t d = 0; d < deps.n; d++) {
        const char *dir = deps.v[d]; /* $NESTDIR/name@sys */
        const char *at = strrchr(dir, '@');
        const char *slash = strrchr(dir, '/');
        if (at == NULL || slash == NULL)
            continue;
        const char *nm = slash + 1;
        char var[256];
        size_t k = 0;
        for (const char *q = nm; q < at && k < sizeof var - 8; q++)
            var[k++] = (*q == '-' || *q == '.' || *q == '+') ? '_' : *q;
        var[k] = '\0';
        strncat(var, "_PREFIX", sizeof var - strlen(var) - 1);
        strlist_add_own(&perdep_assign, fmt_alloc("%s=\"%s\"", var, dir));
        strlist_add_own(&perdep, xstrdup(var));
    }
    /* Ownership moves to `out`: freeing the list here would free the strings
     * that are now in it. */
    for (size_t i = 0; i < perdep_assign.n; i++)
        strlist_add_own(out, perdep_assign.v[i]);
    free(perdep_assign.v);
    perdep_assign.v = NULL;
    perdep_assign.n = perdep_assign.cap = 0;
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
    /* Every variable is defined even when this node has no dependencies: a system
     * fragment expands $CPPFLAGS and friends, and the script runs under `set -u`.
     * The `:-` form also keeps a caller's own value. */
    if (inc_v[0] != '\0')
        strlist_add_own(out, prepend("CPPFLAGS", inc_v, " "));
    else
        strlist_add(out, "CPPFLAGS=\"${CPPFLAGS:-}\"");
    if (lib_v[0] != '\0')
        strlist_add_own(out, prepend("LDFLAGS", lib_v, " "));
    else
        strlist_add(out, "LDFLAGS=\"${LDFLAGS:-}\"");
    if (pkg_v[0] != '\0')
        strlist_add_own(out, prepend("PKG_CONFIG_LIBDIR", pkg_v, ":"));
    else
        strlist_add(out, "PKG_CONFIG_LIBDIR=\"${PKG_CONFIG_LIBDIR:-}\"");
    /* Always set: our .pc files are the only ones that count. */
    strlist_add(out, "PKG_CONFIG_PATH=\"\"");
    if (cmake_v[0] != '\0')
        strlist_add_own(out, prepend("CMAKE_PREFIX_PATH", cmake_v, ";"));
    else
        strlist_add(out, "CMAKE_PREFIX_PATH=\"${CMAKE_PREFIX_PATH:-}\"");
    if (path_v[0] != '\0')
        strlist_add_own(out, prepend("PATH", path_v, ":"));
    else
        strlist_add(out, "PATH=\"${PATH:-}\"");
    if (ld_v[0] != '\0')
        strlist_add_own(out, prepend("LD_LIBRARY_PATH", ld_v, ":"));
    else
        strlist_add(out, "LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH:-}\"");
    if (perdep.n == 0)
        strlist_add(out, "PKG_CONFIG_PATH=\"${PKG_CONFIG_PATH:-}\"");
    if (perdep.n > 0) {
        char *line = xmalloc(64);
        size_t cap = 64, len = 0;
        len += (size_t)snprintf(line, cap, "export");
        for (size_t i = 0; i < perdep.n; i++) {
            while (len + strlen(perdep.v[i]) + 2 > cap) {
                cap *= 2;
                line = xrealloc(line, cap);
            }
            len += (size_t)snprintf(line + len, cap - len, " %s", perdep.v[i]);
        }
        strlist_add_own(out, line);
        strlist_free(&perdep);
    }
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
