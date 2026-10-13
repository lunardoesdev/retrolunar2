#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#define SYSTEM_PREFIX "system@"

char *default_under_home(const char *leaf) {
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
        /* A list, because there can be several repositories. Each is made
         * absolute here, so a relative one keeps meaning what it meant on the
         * command line however the process later changes directory. */
        if (strchr(given, ':') == NULL) {
            char *abs = path_abs(given);
            if (abs == NULL || !is_dir(abs)) {
                fprintf(stderr, "retrolunar: packages tree '%s' is not a directory\n", given);
                free(abs);
                return NULL;
            }
            return abs;
        }
        strlist_t roots;
        strlist_init(&roots);
        const char *p = given;
        while (*p != '\0') {
            const char *colon = strchr(p, ':');
            size_t n = colon != NULL ? (size_t)(colon - p) : strlen(p);
            char *one = xmalloc(n + 1);
            memcpy(one, p, n);
            one[n] = '\0';
            char *abs = path_abs(one);
            free(one);
            if (abs == NULL || !is_dir(abs)) {
                fprintf(stderr, "retrolunar: packages tree '%s' is not a directory\n",
                        abs != NULL ? abs : "(unusable)");
                free(abs);
                strlist_free(&roots);
                return NULL;
            }
            strlist_add(&roots, abs);
            free(abs);
            if (colon == NULL)
                break;
            p = colon + 1;
        }
        char *joined = join_sp(&roots);
        strlist_free(&roots);
        char *out = xstrdup(joined);
        free(joined);
        /* join_sp separates with a space; the list is a path, so put the colons
         * back where they were. */
        for (char *q = out; *q != '\0'; q++)
            if (*q == ' ')
                *q = ':';
        return out;
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

void tree_search_one(const char *packages, const char *query, tree_list_t *out, strlist_t *seen);

void tree_search(const char *packages, const char *query, tree_list_t *out) {
    /* Repositories in order: a name that a project's repository has is not
     * listed again from the built-in one. */
    strlist_t seen;
    strlist_init(&seen);
    if (packages == NULL)
        return;
    const char *p = packages;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t n = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (n > 0) {
            char *root = xmalloc(n + 1);
            memcpy(root, p, n);
            root[n] = '\0';
            tree_search_one(root, query, out, &seen);
            free(root);
        }
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    strlist_free(&seen);
}

void tree_search_one(const char *packages, const char *query, tree_list_t *out, strlist_t *seen) {
    strlist_t names;
    strlist_init(&names);
    list_dir(packages, &names);
    char *lower_query = xstrdup(query ? query : "");
    for (char *p = lower_query; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');

    for (size_t i = 0; i < names.n; i++) {
        const char *n = names.v[i];
        if (strlist_has(seen, n))
            continue;
        strlist_add(seen, n);
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

/* A repository list is a colon-separated path, the way a program looks for an
 * executable in several directories. Order is the meaning: the first repository
 * that has the name wins, so a project's repository overrides the built-in one
 * without any of them knowing about the other. */
char *tree_root_with(const char *roots, const char *name) {
    if (roots == NULL)
        return NULL;
    const char *p = roots;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t n = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (n > 0) {
            char *root = xmalloc(n + 1);
            memcpy(root, p, n);
            root[n] = '\0';
            char *cand = path_join(root, name);
            free(root);
            if (is_dir(cand))
                return cand;
            free(cand);
        }
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    return NULL;
}

char *tree_root_with_entry(const char *roots, const char *name, const char *entry) {
    if (roots == NULL)
        return NULL;
    const char *p = roots;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t n = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (n > 0) {
            char *root = xmalloc(n + 1);
            memcpy(root, p, n);
            root[n] = '\0';
            char *cand = path_join(root, name);
            char *file = path_join(cand, entry);
            int ok = is_file(file);
            free(file);
            free(cand);
            free(root);
            if (ok) {
                char *again = xmalloc(n + 1);
                memcpy(again, p, n);
                again[n] = '\0';
                return again;
            }
        }
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    return NULL;
}
