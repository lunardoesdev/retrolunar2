#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define SYSTEM_PREFIX "system@"

static char *default_under_home(const char *leaf) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0')
        return NULL;
    char *root = path_join(home, ".cache/retrolunar");
    if (mkdirs(root) != 0) {
        free(root);
        return NULL;
    }
    char *dir = path_join(root, leaf);
    free(root);
    return dir;
}

char *tree_packages(const char *given) {
    if (given != NULL) {
        char *abs = path_abs(given);
        if (abs == NULL || !is_dir(abs)) {
            fprintf(stderr, "retrolunar: packages tree '%s' is not a directory\n", given);
            free(abs);
            return NULL;
        }
        return abs;
    }
    char *dir = default_under_home("packages");
    if (dir == NULL || !is_dir(dir)) {
        fprintf(stderr,
                "retrolunar: no packages tree at %s\n"
                "  pass --packages DIR, or clone %s there\n",
                dir != NULL ? dir : "$HOME/.cache/retrolunar/packages",
                RETROLUNAR_PACKAGES_REPO);
        free(dir);
        return NULL;
    }
    return dir;
}

char *tree_nest(const char *given) {
    if (given != NULL)
        return path_abs(given);
    return default_under_home("nestdir");
}

static void entry_free(tree_entry_t *e) {
    free(e->name);
    free(e->dir);
    strlist_free(&e->recipes);
}

void tree_list_free(tree_list_t *l) {
    for (size_t i = 0; i < l->n; i++)
        entry_free(&l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static void list_add(tree_list_t *l, tree_entry_t e) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = e;
}

int tree_is_entry(const char *dir) {
    static const char *const NEEDED[] = {"generic.toml", "source.toml",
                                         "system.toml"};
    for (size_t i = 0; i < sizeof NEEDED / sizeof NEEDED[0]; i++) {
        char *p = path_join(dir, NEEDED[i]);
        int found = is_file(p);
        free(p);
        if (found)
            return 1;
    }
    return 0;
}

/* Build files present in a package directory: every *.toml except source.toml,
 * which is the fetch recipe and reported separately. */
static void collect_recipes(const char *dir, strlist_t *out) {
    strlist_t names;
    strlist_init(&names);
    list_dir(dir, &names);
    for (size_t i = 0; i < names.n; i++) {
        const char *n = names.v[i];
        size_t len = strlen(n);
        if (len < 6 || strcmp(n + len - 5, ".toml") != 0)
            continue;
        if (strcmp(n, "source.toml") == 0)
            continue;
        strlist_add_own(out, xstrndup(n, len - 5));
    }
    strlist_free(&names);
}

void tree_search(const char *packages, const char *query, tree_list_t *out) {
    strlist_t names;
    strlist_init(&names);
    list_dir(packages, &names);
    char *lower_query = xstrdup(query ? query : "");
    for (char *p = lower_query; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');

    for (size_t i = 0; i < names.n; i++) {
        const char *n = names.v[i];
        char *dir = path_join(packages, n);
        if (!is_dir(dir) || !tree_is_entry(dir)) {
            free(dir);
            continue;
        }
        char *lower_name = xstrdup(n);
        for (char *p = lower_name; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p = (char)(*p - 'A' + 'a');
        int match = lower_query[0] == '\0' || strstr(lower_name, lower_query) != NULL;
        free(lower_name);
        if (!match) {
            free(dir);
            continue;
        }
        tree_entry_t e;
        memset(&e, 0, sizeof e);
        e.dir = dir;
        strlist_init(&e.recipes);
        size_t plen = strlen(SYSTEM_PREFIX);
        if (strncmp(n, SYSTEM_PREFIX, plen) == 0) {
            e.is_system = 1;
            e.name = xstrdup(n + plen);
        } else {
            e.name = xstrdup(n);
            collect_recipes(e.dir, &e.recipes);
        }
        list_add(out, e);
    }
    free(lower_query);
    strlist_free(&names);
}
