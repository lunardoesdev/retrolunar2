/* Small shared helpers: allocation, string lists, paths, file reads.
 * No shell anywhere — v1 shelled out to `pwd` and `ls` because Lua has no
 * readdir; here the same work is done directly. */
#ifndef RETROLUNAR_UTIL_H
#define RETROLUNAR_UTIL_H

#include <stddef.h>

/* Allocation that gives up loudly instead of returning NULL: every caller
 * would have to abort anyway. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Growable list of strings, always NUL-terminated in v[n]. */
typedef struct {
    char **v;
    size_t n, cap;
} strlist_t;

void strlist_init(strlist_t *l);
void strlist_add(strlist_t *l, const char *s); /* copies s */
void strlist_add_own(strlist_t *l, char *s);
void strlist_free(strlist_t *l);
int strlist_has(const strlist_t *l, const char *s);
void strlist_sort(strlist_t *l);
int strlist_index(const strlist_t *l, const char *s);

/* Paths and the questions the resolver asks about them. */
char *path_join(const char *a, const char *b);
char *path_abs(const char *p); /* NULL when p is unusable */
int is_dir(const char *p);
int is_file(const char *p);
int is_symlink(const char *p);
int mkdirs(const char *path); /* mkdir -p */

/* Whole-file read. Returns malloc'd bytes of *len bytes plus a NUL, or NULL. */
char *read_file(const char *path, size_t *len);

/* Directory listing, entries sorted, "." and ".." skipped into out.
 * Returns the number of entries added (0 for a missing directory too). */
size_t list_dir(const char *path, strlist_t *out);

#endif /* RETROLUNAR_UTIL_H */
