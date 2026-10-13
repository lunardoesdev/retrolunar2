#include "resolve.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"

/* --- target parsing ------------------------------------------------------ */

typedef struct {
    char *pack;
    char *sys; /* resolved: never "native" */
} target_t;

static void target_free(target_t *t) {
    free(t->pack);
    free(t->sys);
}

/* `pack`, `pack@sys`. `@native` resolves to the default system here, so every
 * later stage sees one identity for it. `@sys` defaulting is the caller's
 * business: a target on the command line uses the default system, a bare
 * dependency inherits the requiring package's system. */
static int parse_target(const char *s, const char *default_sys, target_t *out) {
    out->pack = NULL;
    out->sys = NULL;
    const char *at = strrchr(s, '@');
    if (at == NULL) {
        out->pack = xstrdup(s);
        out->sys = xstrdup(default_sys);
    } else {
        if (at == s) {
            fprintf(stderr, "retrolunar: '%s' has no package name\n", s);
            return -1;
        }
        out->pack = xstrndup(s, (size_t)(at - s));
        out->sys = xstrdup(at + 1);
        if (out->sys[0] == '\0') {
            fprintf(stderr, "retrolunar: '%s' has no system after '@'\n", s);
            target_free(out);
            return -1;
        }
        if (strcmp(out->sys, "native") == 0) {
            free(out->sys);
            out->sys = xstrdup(default_sys);
        }
    }
    return 0;
}

/* --- graph --------------------------------------------------------------- */

static void graph_add_node(graph_t *g, const char *key, pkg_t pkg) {
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 32;
        g->nodes = xrealloc(g->nodes, g->cap * sizeof *g->nodes);
    }
    g->nodes[g->n].key = xstrdup(key);
    g->nodes[g->n].pkg = pkg;
    g->n++;
}

static int graph_has(const graph_t *g, const char *key) {
    for (size_t i = 0; i < g->n; i++)
        if (strcmp(g->nodes[i].key, key) == 0)
            return 1;
    return 0;
}

const system_t *graph_system(const graph_t *g, const char *name) {
    for (size_t i = 0; i < g->nsystems; i++)
        if (strcmp(g->systems[i].name, name) == 0)
            return &g->systems[i];
    return NULL;
}

/* Systems load once per name: the symbol table and the peer-file merge are
 * the same for every package that uses it. */
static const system_t *graph_load_system(graph_t *g, const char *name) {
    const system_t *found = graph_system(g, name);
    if (found != NULL)
        return found;
    if (g->nsystems == g->syscap) {
        g->syscap = g->syscap ? g->syscap * 2 : 8;
        g->systems = xrealloc(g->systems, g->syscap * sizeof *g->systems);
    }
    system_t *s = &g->systems[g->nsystems];
    if (system_load(g->packages, name, s) != 0)
        return NULL;
    g->nsystems++;
    return s;
}

void graph_free(graph_t *g) {
    for (size_t i = 0; i < g->n; i++) {
        free(g->nodes[i].key);
        package_free(&g->nodes[i].pkg);
    }
    free(g->nodes);
    for (size_t i = 0; i < g->nsystems; i++)
        system_free(&g->systems[i]);
    free(g->systems);
    memset(g, 0, sizeof *g);
}

/* --- traversal ----------------------------------------------------------- */

typedef struct {
    graph_t *g;
    strlist_t stack; /* keys currently being visited, for cycle messages */
} visit_ctx_t;

static int visit(visit_ctx_t *ctx, const char *pack, const char *sys, const char *wanted_by);

static int visit_dep(visit_ctx_t *ctx, const char *dep, const char *sys, const char *who,
                     const char *pack) {
    target_t t;
    if (parse_target(dep, sys, &t) != 0) {
        fprintf(stderr, "retrolunar: %s: cannot read dependency '%s'\n", who, dep);
        return -1;
    }
    /* A recipe that needs its own tool for the host says `require("ragel@native")`.
     * On the native system that resolves to the node being visited, which v1
     * treated as already satisfied: a no-op, not a cycle. */
    if (strcmp(t.pack, pack) == 0 && strcmp(t.sys, sys) == 0) {
        target_free(&t);
        return 0;
    }
    int rc = visit(ctx, t.pack, t.sys, who);
    target_free(&t);
    return rc;
}

static int visit(visit_ctx_t *ctx, const char *pack, const char *sys, const char *wanted_by) {
    char key[512];
    snprintf(key, sizeof key, "%s@%s", pack, sys);
    if (graph_has(ctx->g, key))
        return 0;
    if (strlist_has(&ctx->stack, key)) {
        /* Print from the first appearance of this key: the cycle, not the
         * path that led into it. */
        int from = strlist_index(&ctx->stack, key);
        fprintf(stderr, "retrolunar: dependency cycle: %s", ctx->stack.v[from]);
        for (size_t i = (size_t)(from + 1); i < ctx->stack.n; i++)
            fprintf(stderr, " -> %s", ctx->stack.v[i]);
        fprintf(stderr, " -> %s\n", key);
        return -1;
    }

    const system_t *system = NULL;
    if (strcmp(sys, "source") != 0) {
        system = graph_load_system(ctx->g, sys);
        if (system == NULL) {
            fprintf(stderr, "retrolunar: system '%s' not found (needed by '%s')\n", sys,
                    wanted_by != NULL ? wanted_by : pack);
            return -1;
        }
    }

    pkg_t pkg;
    if (package_load(ctx->g->packages, pack, sys, system, &pkg) != 0)
        return -1;

    strlist_add(&ctx->stack, key);
    for (size_t i = 0; i < pkg.deps.n; i++) {
        if (visit_dep(ctx, pkg.deps.v[i], sys, key, pack) != 0) {
            package_free(&pkg);
            return -1;
        }
    }
    free(ctx->stack.v[--ctx->stack.n]);

    graph_add_node(ctx->g, key, pkg);
    return 0;
}

int resolve_targets(const char *packages, const char *const *targets, size_t ntargets,
                    graph_t *out) {
    memset(out, 0, sizeof *out);
    out->packages = packages;
    visit_ctx_t ctx;
    ctx.g = out;
    strlist_init(&ctx.stack);
    for (size_t i = 0; i < ntargets; i++) {
        target_t t;
        if (parse_target(targets[i], RETROLUNAR_DEFAULT_SYSTEM, &t) != 0) {
            strlist_free(&ctx.stack);
            graph_free(out);
            return -1;
        }
        int rc = visit(&ctx, t.pack, t.sys, NULL);
        target_free(&t);
        if (rc != 0) {
            strlist_free(&ctx.stack);
            graph_free(out);
            return -1;
        }
    }
    strlist_free(&ctx.stack);
    return 0;
}
