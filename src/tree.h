/* The packages tree: where it is, and what is in it.
 *
 * Layout: one directory per package or system at the tree root. A system's
 * directory is named system@<name>; a package's directory is its own name.
 * Nothing here parses TOML yet — this is the directory-level view that
 * `search` and the resolver's file selection both start from. */
#ifndef RETROLUNAR_TREE_H
#define RETROLUNAR_TREE_H

#include "util.h"

/* Both return a malloc'd, absolute path. A NULL return means the location is
 * unusable and the reason has been written to stderr. */
char *tree_packages(const char *given);
char *tree_nest(const char *given);

/* One directory in the tree. */
typedef struct {
    char *name;    /* directory name, system@ prefix stripped for systems */
    char *dir;     /* absolute path */
    int is_system; /* directory name was system@<name> */
    strlist_t recipes; /* build files present: generic, <sys>, ... (no source) */
} tree_entry_t;

typedef struct {
    tree_entry_t *v;
    size_t n, cap;
} tree_list_t;

void tree_list_free(tree_list_t *l);

/* Directories whose entry name contains `query` (case-insensitive, empty
 * query matches everything), in name order. Reads the tree only. */
void tree_search(const char *packages, const char *query, tree_list_t *out);

/* True when the directory holds a package or a system we recognise: a
 * generic.toml, a source.toml or a system.toml. */
int tree_is_entry(const char *dir);

#endif /* RETROLUNAR_TREE_H */
