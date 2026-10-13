/* The dependency graph.
 *
 * Nodes are (package, system) pairs — `perl@native` and `perl@mingw64` are
 * different nodes. Edges come from `deps` in the recipe, and a bare entry
 * inherits the system of the package that asked, which is what v1's bare
 * `require("dep")` did. `@source` is the fetch pseudo-system, `@native` the
 * compile-time default system.
 *
 * Order is topological with dependencies first, and requires no ordering in
 * the recipe text: v1 got that for free from Lua's require order, here it is
 * computed. Cycles are refused with the chain that closes them.
 */
#ifndef RETROLUNAR_RESOLVE_H
#define RETROLUNAR_RESOLVE_H

#include "package.h"
#include "system.h"
#include "util.h"

typedef struct {
    char *key; /* pack@sys */
    pkg_t pkg;
} node_t;

typedef struct {
    node_t *nodes;
    size_t n, cap;
    system_t *systems;
    size_t nsystems, syscap;
    const char *packages;
} graph_t;

/* 0 on success. Every failure prints its reason; a partial graph is freed. */
/* Canonical graph key of a dependency: resolves `@native` and the inheritance of
 * a bare name. 0 on success. */
int target_key(const char *dep, const char *caller_sys, char *out, size_t outsz);

/* A project manifest to consult for its own name, before the tree: a local file
 * overrides a package of the same name, and supplies one the tree lacks. */
void resolve_set_local(const char *file, const char *name);

int resolve_targets(const char *packages, const char *const *targets, size_t ntargets,
                    graph_t *out);
void graph_free(graph_t *g);

/* The system of a node, or NULL for `@source`. */
const system_t *graph_system(const graph_t *g, const char *name);

#endif /* RETROLUNAR_RESOLVE_H */
