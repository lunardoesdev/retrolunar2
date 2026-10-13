/* Reading a project manifest. See manifest.h for what the file is. */
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
    toml_doc_free(&doc);

    out->dir = where;
    out->file = file;
    out->name = xstrdup(base_of(where));
    return 0;
}

void manifest_free(manifest_t *m) {
    free(m->dir);
    free(m->file);
    free(m->name);
    strlist_free(&m->shell_deps);
    memset(m, 0, sizeof *m);
}
