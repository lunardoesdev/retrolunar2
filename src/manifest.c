/* Reading a project manifest. See manifest.h for what the file is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manifest.h"
#include "toml_read.h"
#include "util.h"

/* The last component of a path, which is what names the package. */
static const char *base_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

int manifest_find(const char *dir, manifest_t *out) {
    memset(out, 0, sizeof *out);
    strlist_init(&out->shell_deps);

    char *where = NULL;
    if (dir != NULL) {
        where = path_abs(dir);
    } else {
        where = path_abs(".");
    }
    if (where == NULL) {
        fputs("retrolunar: cannot find the working directory\n", stderr);
        return -1;
    }

    char *file = path_join(where, "retrolunar.toml");
    if (!is_file(file)) {
        free(file);
        free(where);
        return 1;
    }

    toml_doc_t doc;
    if (toml_doc_load(&doc, file) != 0) {
        free(file);
        free(where);
        return -1;
    }
    toml_datum_t root = toml_doc_root(&doc);

    toml_datum_t shell = toml_opt_table(root, "shell");
    if (shell.type != TOML_UNKNOWN) {
        if (toml_opt_strarray(shell, "deps", &out->shell_deps) != 0)
            return -1;
        static const char *const known[] = {"deps", NULL};
        if (toml_check_keys(shell, known, "shell") != 0) {
            toml_doc_free(&doc);
            free(file);
            free(where);
            return -1;
        }
    }
    toml_datum_t repos = toml_opt_table(root, "repo");
    if (repos.type != TOML_UNKNOWN) {
        toml_iter_t it;
        toml_iter_init(&it, repos);
        const char *key = NULL;
        toml_datum_t val;
        while (toml_iter_next(&it, &key, &val)) {
            if (val.type != TOML_TABLE) {
                toml_err(val, "repo.%s: wants a table (url, and rev if it should be pinned)", key);
                toml_doc_free(&doc);
                return -1;
            }
            const char *url = toml_opt_str(val, "url");
            if (url == NULL) {
                toml_err(val, "repo.%s: needs a url", key);
                toml_doc_free(&doc);
                return -1;
            }
            static const char *const known[] = {"url", "rev", NULL};
            char ctx[256];
            snprintf(ctx, sizeof ctx, "repo.%s", key);
            if (toml_check_keys(val, known, ctx) != 0) {
                toml_doc_free(&doc);
                return -1;
            }
            if (out->repos.n == out->repos.cap) {
                out->repos.cap = out->repos.cap != 0 ? out->repos.cap * 2 : 4;
                out->repos.v = xrealloc(out->repos.v, out->repos.cap * sizeof *out->repos.v);
            }
            repo_entry_t *e = &out->repos.v[out->repos.n++];
            e->name = xstrdup(key);
            e->url = xstrdup(url);
            const char *rev = toml_opt_str(val, "rev");
            e->rev = rev != NULL ? xstrdup(rev) : NULL;
        }
    }
    toml_doc_free(&doc);

    out->dir = where;
    out->file = file;
    out->name = xstrdup(base_of(where));
    return 0;
}

void manifest_free(manifest_t *m) {
    for (size_t i = 0; i < m->repos.n; i++) {
        free(m->repos.v[i].name);
        free(m->repos.v[i].url);
        free(m->repos.v[i].rev);
    }
    free(m->repos.v);
    free(m->dir);
    free(m->file);
    free(m->name);
    strlist_free(&m->shell_deps);
    memset(m, 0, sizeof *m);
}
