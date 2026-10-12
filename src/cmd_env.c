/* env <target> — the closure's environment as export lines.
 *
 * With every package in its own directory there is no prefix to point at, so
 * this is how a person builds something against the nest:
 *
 *   eval "$(retrolunar env libpng@native)"
 *
 * The lines come from the same closure→environment code the generated script
 * uses, so what works here works there. */
#include <stdio.h>
#include <stdlib.h>

#include "cmds.h"
#include "env.h"
#include "resolve.h"
#include "tree.h"

int cmd_env(const options_t *o) {
    if (o->args.n != 1) {
        fputs("retrolunar: env wants exactly one <pack[@sys]>\n", stderr);
        return 1;
    }
    char *packages = tree_packages(o->packages);
    if (packages == NULL)
        return 1;
    char *nest = tree_nest(o->nest);
    if (nest == NULL) {
        fputs("retrolunar: no nest directory (set HOME or pass --nest)\n", stderr);
        free(packages);
        return 1;
    }

    graph_t g;
    int rc = 1;
    if (resolve_targets(packages, (const char *const *)o->args.v, o->args.n, &g) == 0) {
        size_t idx = g.n > 0 ? g.n - 1 : 0;
        printf("NESTDIR='%s'\nexport NESTDIR\n", nest);
        strlist_t lines;
        strlist_init(&lines);
        env_lines(&g, idx, &lines);
        for (size_t i = 0; i < lines.n; i++)
            printf("%s\n", lines.v[i]);
        strlist_free(&lines);
        rc = ferror(stdout) ? 1 : 0;
        graph_free(&g);
    }
    free(nest);
    free(packages);
    return rc;
}
