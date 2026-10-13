/* shell, run — a project's environment, activated.
 *
 * `retrolunar.toml` says what the project is and what it depends on. shell
 * builds whatever is missing and then starts a shell with the closure's
 * environment: the same lines `retrolunar env` prints (NESTDIR, a `$<dep>_PREFIX`
 * per dependency, CPPFLAGS, LDFLAGS, PKG_CONFIG_LIBDIR, CMAKE_PREFIX_PATH, PATH,
 * LD_LIBRARY_PATH), so a build inside the shell finds everything without being
 * told where to look.
 *
 * `run <cmd> [args]` is the same without an interactive shell, which is what a
 * CI step wants.
 *
 * The lines are shell, not values — they mention $NESTDIR and append to
 * ${PATH:-} — so they are written into a wrapper script and a shell evaluates
 * them, exactly as `eval "$(retrolunar env ...)"` would. Nothing is re-parsed
 * here, and the wrapper is a file a person can read.
 *
 * The package in the manifest is not built here: `retrolunar build` does that. A
 * shell is for working on it, so what it needs is the dependencies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmds.h"
#include "lock.h"
#include "env.h"
#include "manifest.h"
#include "package.h"
#include "resolve.h"
#include "tree.h"

int build_script_for_targets(const options_t *o, const char *const *targets, size_t ntargets,
                             char **packages_out, graph_t *g, char **script, size_t *len);
int run_script(const char *path, const char *script, size_t len);

/* The environment of the targets. Each target's own closure is already in its
 * lines, so a dependency needs no second pass — and taking every node would let
 * a `@source` node overwrite the package it belongs to. */
static void closure_env(const graph_t *g, const char *nest, const strlist_t *targets,
                        strlist_t *out) {
    char line[4096];
    snprintf(line, sizeof line, "NESTDIR='%s'", nest);
    strlist_add(out, line);
    strlist_t wanted;
    strlist_init(&wanted);
    for (size_t i = 0; i < targets->n; i++) {
        char key[512];
        if (target_key(targets->v[i], RETROLUNAR_DEFAULT_SYSTEM, key, sizeof key) == 0)
            strlist_add(&wanted, key);
    }
    for (size_t i = 0; i < g->n; i++) {
        if (!strlist_has(&wanted, g->nodes[i].key))
            continue;
        strlist_t lines;
        strlist_init(&lines);
        env_lines(g, i, 1, &lines);
        for (size_t k = 0; k < lines.n; k++) {
            /* NESTDIR is stated once, above, for all of them. */
            if (strncmp(lines.v[k], "NESTDIR=", 8) == 0 ||
                strcmp(lines.v[k], "export NESTDIR") == 0)
                continue;
            strlist_add(out, lines.v[k]);
        }
        strlist_free(&lines);
    }
    strlist_free(&wanted);
}

/* A script that sets the environment and then becomes the command. The child
 * shell evaluates the lines, so what they mean is what a shell says they mean. */
static char *write_wrapper(const strlist_t *lines, const char *nest) {
    char *dir = path_join(nest, ".retrolunar");
    if (mkdirs(dir) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", dir);
        free(dir);
        return NULL;
    }
    char *path = path_join(dir, "env.sh");
    free(dir);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "retrolunar: cannot write %s\n", path);
        return NULL;
    }
    fputs("#!/bin/sh\n# The environment of this project's closure, then the command.\n", f);
    for (size_t i = 0; i < lines->n; i++)
        fprintf(f, "%s\n", lines->v[i]);
    fputs("exec \"$@\"\n", f);
    if (fclose(f) != 0) {
        fprintf(stderr, "retrolunar: cannot close %s\n", path);
        return NULL;
    }
    return xstrdup(path);
}

int cmd_shell(const options_t *o) {
    manifest_t m;
    int found = manifest_find(NULL, &m);
    if (found < 0)
        return 1;
    if (found == 1) {
        fputs("retrolunar: no retrolunar.toml here\n", stderr);
        return 1;
    }

    /* The manifest is the recipe for the package this directory is, so a
     * dependency of the same name resolves to it rather than to the tree. */
    resolve_set_local(m.file, m.name);

    pkg_t self;
    if (package_load_file(m.file, m.name, RETROLUNAR_DEFAULT_SYSTEM, &self) != 0) {
        manifest_free(&m);
        return 1;
    }

    strlist_t targets;
    strlist_init(&targets);
    for (size_t i = 0; i < self.deps.n; i++)
        strlist_add(&targets, self.deps.v[i]);
    for (size_t i = 0; i < m.shell_deps.n; i++)
        strlist_add(&targets, m.shell_deps.v[i]);
    package_free(&self);

    if (targets.n == 0) {
        fputs("retrolunar: the manifest names no dependencies\n", stderr);
        strlist_free(&targets);
        manifest_free(&m);
        return 1;
    }

    strlist_t shell_targets;
    strlist_init(&shell_targets);
    for (size_t i = 0; i < targets.n; i++)
        strlist_add(&shell_targets, targets.v[i]);

    graph_t g;
    char *packages = NULL, *script = NULL;
    size_t len = 0;
    if (build_script_for_targets(o, (const char *const *)targets.v, targets.n, &packages, &g,
                                 &script, &len) != 0) {
        strlist_free(&targets);
        manifest_free(&m);
        return 1;
    }
    printf("retrolunar: %s: %zu package(s) in the environment\n", m.name, g.n);
    int rc = run_script(NULL, script, len);
    if (rc == 0 && lock_write(&m, &g) != 0)
        rc = 1;
    free(script);
    if (rc != 0) {
        fputs("retrolunar: the dependencies did not build\n", stderr);
        graph_free(&g);
        free(packages);
        manifest_free(&m);
        return 1;
    }

    char *nest = tree_nest(o->nest);
    strlist_t lines;
    strlist_init(&lines);
    closure_env(&g, nest != NULL ? nest : "", &shell_targets, &lines);
    char *wrapper = write_wrapper(&lines, nest != NULL ? nest : "/tmp");
    printf("retrolunar: environment in %s\n", wrapper != NULL ? wrapper : "(not written)");
    strlist_free(&lines);
    graph_free(&g);
    free(packages);
    manifest_free(&m);
    free(nest);
    if (wrapper == NULL)
        return 1;

    if (o->cmd == CMD_RUN) {
        if (o->args.n == 0) {
            fputs("retrolunar: run wants a command\n", stderr);
            free(wrapper);
            return 1;
        }
        size_t n = o->args.n + 3;
        char **argv = xmalloc(n * sizeof *argv);
        argv[0] = (char *)"sh";
        argv[1] = wrapper;
        for (size_t i = 0; i < o->args.n; i++)
            argv[i + 2] = o->args.v[i];
        argv[n - 1] = NULL;
        execvp("sh", argv);
        fprintf(stderr, "retrolunar: cannot run %s\n", o->args.v[0]);
        free(argv);
        free(wrapper);
        return 1;
    }

    const char *shell = getenv("SHELL");
    if (shell == NULL || *shell == '\0')
        shell = "/bin/sh";
    execlp("sh", "sh", wrapper, shell, "-i", (char *)NULL);
    fprintf(stderr, "retrolunar: cannot start %s\n", shell);
    free(wrapper);
    return 1;
}
