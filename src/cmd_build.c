/* build — the package this directory is.
 *
 * The manifest is a recipe, so this is an ordinary build of one package; the
 * difference is where it lands. Without `--out` it goes into the nest like any
 * other package and is cached there.
 *
 * With `--out DIR` the package is installed into DIR — its own configure is told
 * that prefix — while its dependencies stay in the nest. DIR therefore holds
 * this package and nothing else, which is what makes it the directory you point
 * a test, a package recipe or a debugger at.
 *
 * The tool refuses DIR unless it is empty: publishing replaces the directory,
 * and that must never remove something the project put there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "emit.h"
#include "manifest.h"
#include "resolve.h"
#include "tree.h"

int run_script(const char *path, const char *script, size_t len);

/* Anything in there that is not the directory itself? */
static int dir_has_files(const char *dir) {
    strlist_t entries;
    strlist_init(&entries);
    size_t n = list_dir(dir, &entries);
    strlist_free(&entries);
    return n > 0;
}

int cmd_build(const options_t *o) {
    if (o->args.n > 1) {
        fputs("retrolunar: build wants at most one <pack[@sys]>\n", stderr);
        return 1;
    }
    manifest_t m;
    int found = manifest_find(NULL, &m);
    if (found < 0)
        return 1;
    if (found == 1) {
        fputs("retrolunar: no retrolunar.toml here\n", stderr);
        return 1;
    }
    resolve_set_local(m.file, m.name);

    /* The target is the package this directory is; a name on the command line
     * only says which system to build it for. */
    char *target;
    if (o->args.n == 1) {
        if (strchr(o->args.v[0], '@') != NULL) {
            target = xstrdup(o->args.v[0]);
        } else {
            size_t n = strlen(o->args.v[0]) + 64;
            target = xmalloc(n);
            snprintf(target, n, "%s@%s", o->args.v[0], RETROLUNAR_DEFAULT_SYSTEM);
        }
    } else {
        size_t n = strlen(m.name) + 64;
        target = xmalloc(n);
        snprintf(target, n, "%s@%s", m.name, RETROLUNAR_DEFAULT_SYSTEM);
    }

    char key[512];
    if (target_key(target, RETROLUNAR_DEFAULT_SYSTEM, key, sizeof key) != 0) {
        free(target);
        manifest_free(&m);
        return 1;
    }

    char *out = NULL;
    if (o->to != NULL) {
        out = path_abs(o->to);
        if (out == NULL) {
            fprintf(stderr, "retrolunar: cannot use %s as a directory\n", o->to);
            free(target);
            manifest_free(&m);
            return 1;
        }
        if (is_dir(out) && dir_has_files(out)) {
            fprintf(stderr, "retrolunar: %s is not empty; publishing replaces it\n", out);
            free(out);
            free(target);
            manifest_free(&m);
            return 1;
        }
    }

    char *packages = tree_packages(o->packages);
    char *nest = tree_nest(o->nest);
    if (packages == NULL || nest == NULL) {
        free(packages);
        free(nest);
        free(out);
        free(target);
        manifest_free(&m);
        return 1;
    }

    graph_t g;
    if (resolve_targets(packages, (const char *const *)&target, 1, &g) != 0) {
        free(packages);
        free(nest);
        free(out);
        free(target);
        manifest_free(&m);
        return 1;
    }

    size_t len = 0;
    char *script = emit_script_string_out(&g, nest, o->cores, out != NULL ? key : NULL, out, &len);
    int rc = 1;
    if (script == NULL) {
        fputs("retrolunar: could not emit the build script\n", stderr);
    } else if (o->output != NULL) {
        /* `-o FILE` leaves the script behind instead of running it, which is
         * what makes a build auditable and reproducible by hand. */
        FILE *f = fopen(o->output, "wb");
        if (f == NULL) {
            fprintf(stderr, "retrolunar: cannot write %s\n", o->output);
        } else if (fwrite(script, 1, len, f) != len || fclose(f) != 0) {
            fprintf(stderr, "retrolunar: short write to %s\n", o->output);
        } else {
            chmod(o->output, 0755);
            printf("retrolunar: wrote %s\n", o->output);
            rc = 0;
        }
        free(script);
    } else {
        printf("retrolunar: %s: building %s\n", m.name, target);
        rc = run_script(NULL, script, len);
        if (rc == 0 && out != NULL)
            printf("retrolunar: %s is in %s\n", key, out);
        free(script);
    }

    graph_free(&g);
    free(packages);
    free(nest);
    free(out);
    free(target);
    manifest_free(&m);
    return rc;
}
