/* install — the script on stdout, and nothing else.
 * This is the command to pipe somewhere; generate is this plus a file and the
 * option to run it. */
#include <stdio.h>
#include <stdlib.h>

#include "cmds.h"
#include "emit.h"
#include "resolve.h"
#include "tree.h"

/* Shared by install and generate: resolve the targets and write the script. */
/* The targets are a parameter because `shell` builds the dependencies its
 * manifest names, not the positionals the command line carried. */
int build_script_for_targets(const options_t *o, const char *const *targets, size_t ntargets,
                             char **packages_out, graph_t *g, char **script, size_t *len) {
    if (ntargets == 0) {
        fputs("retrolunar: no targets given\n", stderr);
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
    if (resolve_targets(packages, targets, ntargets, g) != 0) {
        free(packages);
        free(nest);
        return 1;
    }
    *script = emit_script_string(g, nest, o->cores, len);
    free(nest);
    if (*script == NULL) {
        fputs("retrolunar: could not emit the build script\n", stderr);
        free(packages);
        graph_free(g);
        return 1;
    }
    *packages_out = packages;
    return 0;
}

int build_script_for(const options_t *o, char **packages_out, graph_t *g, char **script,
                     size_t *len) {
    return build_script_for_targets(o, (const char *const *)o->args.v, o->args.n, packages_out,
                                    g, script, len);
}

int cmd_install(const options_t *o) {
    graph_t g;
    char *packages = NULL, *script = NULL;
    size_t len = 0;
    if (build_script_for(o, &packages, &g, &script, &len) != 0)
        return 1;
    int rc = 0;
    if (len > 0 && fwrite(script, 1, len, stdout) != len) {
        fputs("retrolunar: write failed\n", stderr);
        rc = 1;
    }
    if (ferror(stdout) || fflush(stdout) != 0)
        rc = 1;
    free(script);
    graph_free(&g);
    free(packages);
    return rc;
}
