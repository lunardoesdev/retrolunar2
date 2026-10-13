/* Reading and writing retrolunar.lock. See lock.h for why it exists. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "fingerprint.h"
#include "lock.h"
#include "resolve.h"
#include "toml_read.h"
#include "tree.h"
#include "util.h"

char *lock_path(const manifest_t *m) {
    return path_join(m->dir, "retrolunar.lock");
}

/* What a repository is at right now: a git checkout says so, and anything else
 * (a plain directory the tests use) has no revision to name. */
static char *git_head(const char *dir) {
    char *git = path_join(dir, ".git");
    int is_git = is_dir(git);
    free(git);
    if (!is_git)
        return NULL;

    size_t n = strlen(dir) + 64;
    char *cmd = xmalloc(n);
    snprintf(cmd, n, "git -C '%s' rev-parse HEAD 2>/dev/null", dir);
    FILE *f = popen(cmd, "r");
    free(cmd);
    if (f == NULL)
        return NULL;
    char buf[128];
    if (fgets(buf, sizeof buf, f) == NULL) {
        pclose(f);
        return NULL;
    }
    pclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return len > 0 ? xstrdup(buf) : NULL;
}

static void write_escaped(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

int lock_write(const manifest_t *m, const graph_t *g) {
    char *path = lock_path(m);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "retrolunar: cannot write %s\n", path);
        free(path);
        return -1;
    }

    fputs("# Written by retrolunar after a build. Not edited by hand: it says what\n"
          "# the manifest resolved to, so the next build starts from the same tree\n"
          "# even though the repositories have moved on.\n\n",
          f);

    for (size_t i = 0; i < m->repos.n; i++) {
        const repo_entry_t *r = &m->repos.v[i];
        fputs("[[repo]]\n", f);
        fputs("name = ", f);
        write_escaped(f, r->name);
        fputc('\n', f);
        fputs("url = ", f);
        write_escaped(f, r->url);
        fputc('\n', f);
        /* The revision the manifest pins, or where the checkout stands. */
        const char *where = r->url;
        char *dir = (r->url[0] == '/' || r->url[0] == '.') ? path_abs(r->url) : NULL;
        char *head = NULL;
        if (dir != NULL) {
            where = dir;
            head = git_head(dir);
        } else {
            char *base = default_under_home("repos");
            if (base != NULL) {
                char *clone = path_join(base, r->name);
                head = git_head(clone);
                free(clone);
                free(base);
            }
        }
        const char *rev = r->rev != NULL ? r->rev : head;
        if (rev != NULL) {
            fputs("rev = ", f);
            write_escaped(f, rev);
            fputc('\n', f);
        }
        free(head);
        free(dir);
        (void)where;
        fputc('\n', f);
    }

    char **fps = fingerprint_nodes(g);
    if (fps != NULL) {
        fputs("# Every node this project came out as: the package directory is\n"
              "# <nest>/store/<entry>.\n[entries]\n",
              f);
        for (size_t i = 0; i < g->n; i++) {
            char key[512];
            snprintf(key, sizeof key, "%s@%s", g->nodes[i].pkg.name, g->nodes[i].pkg.sys);
            write_escaped(f, key);
            fputs(" = ", f);
            write_escaped(f, fps[i] != NULL ? fps[i] : "");
            fputc('\n', f);
        }
        for (size_t i = 0; i < g->n; i++)
            free(fps[i]);
        free(fps);
    }

    if (fclose(f) != 0) {
        fprintf(stderr, "retrolunar: cannot write %s\n", path);
        free(path);
        return -1;
    }
    free(path);
    return 0;
}

int lock_read_revs(manifest_t *m) {
    char *path = lock_path(m);
    if (!is_file(path)) {
        free(path);
        return 1;
    }
    toml_doc_t doc;
    if (toml_doc_load(&doc, path) != 0) {
        free(path);
        return -1;
    }
    toml_datum_t root = toml_doc_root(&doc);
    toml_datum_t repos = toml_opt_table(root, "repo");
    if (repos.type == TOML_TABLE) {
        int n = (int)repos.u.arr.size;
        for (int i = 0; i < n; i++) {
            toml_datum_t e = repos.u.arr.elem[i];
            if (e.type != TOML_TABLE)
                continue;
            const char *name = toml_opt_str(e, "name");
            const char *rev = toml_opt_str(e, "rev");
            if (name == NULL || rev == NULL)
                continue;
            for (size_t k = 0; k < m->repos.n; k++) {
                /* The manifest is what the project says; the lock only fills in
                 * a revision it did not pin. */
                if (strcmp(m->repos.v[k].name, name) == 0 && m->repos.v[k].rev == NULL)
                    m->repos.v[k].rev = xstrdup(rev);
            }
        }
    }
    toml_doc_free(&doc);
    free(path);
    return 0;
}
