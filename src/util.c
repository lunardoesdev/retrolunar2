#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fputs("retrolunar: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (q == NULL) {
        fputs("retrolunar: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void strlist_init(strlist_t *l) {
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

void strlist_add_own(strlist_t *l, char *s) {
    if (l->n + 2 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
    l->v[l->n] = NULL;
}

void strlist_add(strlist_t *l, const char *s) { strlist_add_own(l, xstrdup(s)); }

void strlist_free(strlist_t *l) {
    for (size_t i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    strlist_init(l);
}

int strlist_index(const strlist_t *l, const char *s) {
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0)
            return (int)i;
    return -1;
}

int strlist_has(const strlist_t *l, const char *s) {
    return strlist_index(l, s) >= 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void strlist_sort(strlist_t *l) {
    if (l->n > 1)
        qsort(l->v, l->n, sizeof *l->v, cmp_str);
}

void pairlist_init(pairlist_t *p) {
    p->v = NULL;
    p->n = 0;
    p->cap = 0;
}

void pairlist_add(pairlist_t *p, const char *name, const char *value) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->v = xrealloc(p->v, p->cap * sizeof *p->v);
    }
    p->v[p->n].name = xstrdup(name);
    p->v[p->n].value = xstrdup(value);
    p->n++;
}

const char *pairlist_get(const pairlist_t *p, const char *name) {
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->v[i].name, name) == 0)
            return p->v[i].value;
    return NULL;
}

void pairlist_free(pairlist_t *p) {
    for (size_t i = 0; i < p->n; i++) {
        free(p->v[i].name);
        free(p->v[i].value);
    }
    free(p->v);
    pairlist_init(p);
}

char *str_concat(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    char *p = xmalloc(na + nb + 1);
    memcpy(p, a, na);
    memcpy(p + na, b, nb + 1);
    return p;
}

char *path_join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    int slash = na > 0 && a[na - 1] != '/';
    char *p = xmalloc(na + (size_t)slash + nb + 1);
    memcpy(p, a, na);
    if (slash)
        p[na++] = '/';
    memcpy(p + na, b, nb + 1);
    return p;
}

/* Absolutize with realpath when the path exists, else by prefixing getcwd.
 * Deliberately no shell: `pwd` was a v1 workaround for Lua. */
char *path_abs(const char *p) {
    char *rp = realpath(p, NULL);
    if (rp != NULL)
        return rp;
    if (p[0] == '/')
        return xstrdup(p);
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) == NULL)
        return NULL;
    return path_join(cwd, p);
}

int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int is_symlink(const char *p) {
    struct stat st;
    return lstat(p, &st) == 0 && S_ISLNK(st.st_mode);
}

int mkdirs(const char *path) {
    char *copy = xstrdup(path);
    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/')
        copy[--n] = '\0';
    for (size_t i = 1; i <= n; i++) {
        if (copy[i] != '/' && copy[i] != '\0')
            continue;
        char saved = copy[i];
        copy[i] = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
            free(copy);
            return -1;
        }
        copy[i] = saved;
    }
    free(copy);
    return 0;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    size_t cap = 8192, n = 0;
    char *buf = xmalloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap + 1);
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) {
                fclose(f);
                free(buf);
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    buf[n] = '\0';
    if (len != NULL)
        *len = n;
    return buf;
}

size_t list_dir(const char *path, strlist_t *out) {
    DIR *d = opendir(path);
    if (d == NULL)
        return 0;
    size_t before = out->n;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        strlist_add(out, e->d_name);
    }
    closedir(d);
    size_t added = out->n - before;
    if (added > 1)
        qsort(out->v + before, added, sizeof *out->v, cmp_str);
    return added;
}

/* One space-separated string, for the places a shell script wants a word list. */
char *join_sp(const strlist_t *l) {
    size_t len = 1;
    for (size_t i = 0; i < l->n; i++)
        len += strlen(l->v[i]) + 1;
    char *out = xmalloc(len);
    size_t at = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (i > 0)
            out[at++] = ' ';
        size_t n = strlen(l->v[i]);
        memcpy(out + at, l->v[i], n);
        at += n;
    }
    out[at] = '\0';
    return out;
}

/* Modification time in nanoseconds, or 0 when the path is not there: a missing
 * path then never looks newer than an existing one. */
long long mtime_ns(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (long long)st.st_mtim.tv_sec * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}
