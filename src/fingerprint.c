#include "fingerprint.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "hash.h"

/* Recursive walk, names included in the digest so a rename is a change even
 * when the bytes are the same. Sorting is what makes the digest stable: a
 * directory listing has no order of its own. */
static void hash_dir(sha256_t *h, const char *dir, const char *prefix) {
    strlist_t names;
    strlist_init(&names);
    list_dir(dir, &names);
    for (size_t i = 0; i < names.n; i++) {
        const char *name = names.v[i];
        char *rel = prefix[0] == '\0' ? xstrdup(name) : path_join(prefix, name);
        char *full = path_join(dir, name);
        if (is_dir(full)) {
            sha256_update(h, "dir:", 4);
            sha256_update(h, rel, strlen(rel));
            sha256_update(h, "\n", 1);
            hash_dir(h, full, rel);
        } else if (is_file(full)) {
            char hex[65];
            if (sha256_file_hex(full, hex) == 0) {
                sha256_update(h, "file:", 5);
                sha256_update(h, rel, strlen(rel));
                sha256_update(h, " ", 1);
                sha256_update(h, hex, 64);
                sha256_update(h, "\n", 1);
            }
        } else if (is_symlink(full)) {
            /* A symlink's target is part of what the build sees. */
            char target[PATH_MAX];
            ssize_t n = readlink(full, target, sizeof target - 1);
            if (n > 0) {
                target[n] = '\0';
                sha256_update(h, "link:", 5);
                sha256_update(h, rel, strlen(rel));
                sha256_update(h, " -> ", 4);
                sha256_update(h, target, (size_t)n);
                sha256_update(h, "\n", 1);
            }
        }
        free(full);
        free(rel);
    }
    strlist_free(&names);
}

/* Only the directory's own name goes in, never its full path: a fingerprint
 * must not change because the tree was cloned somewhere else. */
static const char *base_name(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash != NULL ? slash + 1 : p;
}

char *fingerprint_dir(const char *dir) {
    sha256_t h;
    sha256_init(&h);
    sha256_update(&h, "dir:", 4);
    const char *name = base_name(dir);
    sha256_update(&h, name, strlen(name));
    sha256_update(&h, "\n", 1);
    if (is_dir(dir))
        hash_dir(&h, dir, "");
    char hex[65];
    sha256_final_hex(&h, hex);
    return xstrdup(hex);
}

static int node_index(const graph_t *g, const char *key) {
    for (size_t i = 0; i < g->n; i++)
        if (strcmp(g->nodes[i].key, key) == 0)
            return (int)i;
    return -1;
}

/* Everything a package's build reads, in one digest. */
static char *fingerprint_node(const graph_t *g, size_t i, char **done) {
    pkg_t *p = &g->nodes[i].pkg;
    sha256_t h;
    sha256_init(&h);
    sha256_update(&h, "tool:", 5);
    sha256_update(&h, RETROLUNAR_VERSION, strlen(RETROLUNAR_VERSION));
    sha256_update(&h, "\nnode:", 6);
    sha256_update(&h, g->nodes[i].key, strlen(g->nodes[i].key));
    sha256_update(&h, "\n", 1);

    /* The recipe files: the build file that won, plus source.toml when the
     * package has one, since a version move there must rebuild it. */
    char *files[3] = {p->file, NULL, NULL};
    char *src = path_join(p->dir, "source.toml");
    if (strcmp(p->sys, "source") != 0 && is_file(src))
        files[1] = src;
    for (int k = 0; k < 3 && files[k] != NULL; k++) {
        char hex[65];
        if (sha256_file_hex(files[k], hex) == 0) {
            sha256_update(&h, "recipe:", 7);
            sha256_update(&h, base_name(files[k]), strlen(base_name(files[k])));
            sha256_update(&h, " ", 1);
            sha256_update(&h, hex, 64);
            sha256_update(&h, "\n", 1);
        }
    }
    free(src);

    /* The system it builds under, and each dependency's fingerprint. */
    if (strcmp(p->sys, "source") != 0) {
        char *sysdir = str_concat("system@", p->sys);
        char *full = path_join(g->packages, sysdir);
        char *sfp = fingerprint_dir(full);
        sha256_update(&h, "system:", 7);
        sha256_update(&h, sfp, strlen(sfp));
        sha256_update(&h, "\n", 1);
        free(sfp);
        free(full);
        free(sysdir);
    }
    for (size_t d = 0; d < p->deps.n; d++) {
        /* A bare dependency inherits the system of the package that named it. */
        const char *at = strrchr(p->deps.v[d], '@');
        char key[512];
        if (at != NULL)
            snprintf(key, sizeof key, "%s", p->deps.v[d]);
        else
            snprintf(key, sizeof key, "%s@%s", p->deps.v[d], p->sys);
        int di = node_index(g, key);
        sha256_update(&h, "dep:", 4);
        sha256_update(&h, key, strlen(key));
        if (di >= 0 && done[di] != NULL) {
            sha256_update(&h, " ", 1);
            sha256_update(&h, done[di], strlen(done[di]));
        } else {
            /* Not in the graph: a name-only dependency such as the bare use of
             * @native. Its absence is part of the input, not a crash. */
            sha256_update(&h, " (absent)", 10);
        }
        sha256_update(&h, "\n", 1);
    }

    char hex[65];
    sha256_final_hex(&h, hex);
    return xstrdup(hex);
}

char **fingerprint_nodes(const graph_t *g) {
    char **done = xmalloc((g->n ? g->n : 1) * sizeof *done);
    for (size_t i = 0; i < g->n; i++)
        done[i] = NULL;
    /* Nodes come out in dependency order, so a node's deps are already
     * computed and the cascade falls out of the loop. */
    for (size_t i = 0; i < g->n; i++)
        done[i] = fingerprint_node(g, i, done);
    return done;
}
