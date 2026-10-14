/* gc — remove store entries nothing points at.
 *
 * A build's directory is named by its fingerprint, so a changed recipe leaves
 * the previous entry behind and untouched: that is the point (two revisions of a
 * repository, or two projects, keep their own `libpng`), and it is also how the
 * store grows. An entry is in use when a name in the nest points at it — the
 * `libpng@clang-native` symlink a resolution leaves — so anything else is what a
 * previous state of the tree built and nothing needs any more.
 *
 * Nothing else is touched: state files, work directories and the local
 * directories `build --out` wrote to are not this command's business.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "tree.h"
#include "util.h"

/* The store entry this path resolves to, or NULL when the path does not exist.
 * A name points into the store; anything else (a work directory, a state file)
 * is ignored. */
static char *resolves_to(const char *path) {
    char buf[PATH_MAX];
    if (realpath(path, buf) == NULL)
        return NULL;
    return xstrdup(buf);
}

int cmd_gc(const options_t *o) {
    char *nest = tree_nest(o->nest);
    if (nest == NULL) {
        fputs("retrolunar: no nest directory (set HOME or pass --nest)\n", stderr);
        return 1;
    }
    /* The nest's lock, exclusively: a build holds it shared while it works, so this
     * waits for running builds instead of removing an entry one of them is about to
     * point a name at. */
    if (nest_lock_exclusive(nest) < 0) {
        free(nest);
        return 1;
    }
    char *store = path_join(nest, "store");
    if (!is_dir(store)) {
        printf("retrolunar: %s has no store; nothing to collect\n", nest);
        free(store);
        free(nest);
        return 0;
    }

    /* What the names in the nest point at. */
    strlist_t in_use;
    strlist_init(&in_use);
    strlist_t names;
    strlist_init(&names);
    size_t nnames = list_dir(nest, &names);
    for (size_t i = 0; i < nnames; i++) {
        char *p = path_join(nest, names.v[i]);
        char *target = resolves_to(p);
        if (target != NULL)
            strlist_add(&in_use, target);
        free(target);
        free(p);
    }
    strlist_free(&names);

    strlist_t entries;
    strlist_init(&entries);
    size_t n = list_dir(store, &entries);
    size_t removed = 0, kept = 0;
    for (size_t i = 0; i < n; i++) {
        char *p = path_join(store, entries.v[i]);
        char *target = resolves_to(p);
        if (target != NULL && strlist_has(&in_use, target)) {
            kept++;
            free(target);
            free(p);
            continue;
        }
        free(target);
        if (!is_dir(p)) {
            free(p);
            continue;
        }
        if (remove_tree(p) != 0) {
            free(p);
            continue;
        }
        removed++;
        free(p);
    }
    printf("retrolunar: %zu entr%s removed, %zu in use\n", removed, removed == 1 ? "y" : "ies",
           kept);
    strlist_free(&entries);
    strlist_free(&in_use);
    free(store);
    free(nest);
    return 0;
}
