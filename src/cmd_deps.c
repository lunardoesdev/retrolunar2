/* deps [--packages DIR] <pack[@sys]>... — resolve the graph and print it,
 * dependencies first, then the targets that were asked for. Nothing is
 * downloaded, built or written; the tree is read only. */
#include <stdio.h>
#include <stdlib.h>

#include "cmds.h"
#include "resolve.h"
#include "tree.h"

int cmd_deps(const options_t *o) {
    if (o->args.n == 0) {
        fputs("retrolunar: deps wants at least one <pack[@sys]>\n", stderr);
        return 1;
    }
    char *packages = tree_packages(o->packages);
    if (packages == NULL)
        return 1;

    graph_t g;
    int rc = resolve_targets(packages, (const char *const *)o->args.v, o->args.n, &g);
    if (rc == 0) {
        for (size_t i = 0; i < g.n; i++) {
            pkg_t *p = &g.nodes[i].pkg;
            if (p->version != NULL)
                printf("%s@%s %s\n", p->name, p->sys, p->version);
            else
                printf("%s@%s\n", p->name, p->sys);
        }
        if (ferror(stdout))
            rc = 1;
    }
    graph_free(&g);
    free(packages);
    return rc == 0 ? 0 : 1;
}
