#include "package.h"
#include "tree.h"

#include <stdlib.h>
#include <string.h>

/* Keys a package file may hold besides its extra scalar fields. `build` and
 * `deps` are the recipe; `probe` feeds freshness; `version` is the v1 field,
 * and any other scalar is emitted as a shell variable too. */
char *package_build_file(const char *packages, const char *name, const char *sys,
                         const system_t *system) {
    /* Several repositories are searched in order: the first that has the
     * package is the one its recipe comes from. */
    char *dir = tree_root_with(packages, name);
    if (dir == NULL)
        return NULL;
    char *cand = NULL;
    if (sys != NULL && strcmp(sys, "source") != 0) {
        char fname[512];
        snprintf(fname, sizeof fname, "%s.toml", sys);
        char *p = path_join(dir, fname);
        if (is_file(p))
            cand = p;
        else
            free(p);
        if (cand == NULL && system != NULL) {
            for (size_t i = 0; i < system->fallbacks.n && cand == NULL; i++) {
                snprintf(fname, sizeof fname, "%s.toml", system->fallbacks.v[i]);
                p = path_join(dir, fname);
                if (is_file(p))
                    cand = p;
                else
                    free(p);
            }
        }
    }
    if (cand == NULL) {
        char *p = path_join(dir, "generic.toml");
        if (is_file(p))
            cand = p;
        else
            free(p);
    }
    free(dir);
    return cand;
}

/* The tables a project manifest adds to the recipe format. They are read by
 * manifest.c; the recipe reader only has to let them through, and it grows this
 * list in the same commit that teaches the manifest to use a table. */
/* Tables a project manifest carries besides the recipe keys. The manifest is a
 * recipe, so anything else it says belongs to the project rather than to the
 * build: where its packages come from, and (once there is a cache) where built
 * ones are kept. */
static int manifest_table(const char *key) {
    return strcmp(key, "shell") == 0 || strcmp(key, "repo") == 0 || strcmp(key, "cache") == 0;
}

static int load_doc(const char *file, pkg_t *out) {
    toml_doc_t doc;
    if (toml_doc_load(&doc, file) != 0)
        return -1;
    toml_datum_t root = toml_doc_root(&doc);

    int bad = 0;
    toml_iter_t it;
    toml_iter_init(&it, root);
    const char *key;
    toml_datum_t v;
    while (toml_iter_next(&it, &key, &v)) {
        if (strcmp(key, "deps") == 0) {
            strlist_free(&out->deps);
            strlist_init(&out->deps);
            if (toml_req_strarray(root, key, NULL, &out->deps) != 0)
                bad = 1;
            continue;
        }
        if (strcmp(key, "probe") == 0) {
            strlist_free(&out->probes);
            strlist_init(&out->probes);
            if (toml_req_strarray(root, key, NULL, &out->probes) != 0)
                bad = 1;
            else
                out->has_probe = 1;
            continue;
        }
        if (strcmp(key, "build") == 0 || strcmp(key, "script") == 0) {
            if (v.type != TOML_STRING) {
                toml_err(v, "'%s' must be a string", key);
                bad = 1;
                continue;
            }
            free(out->build);
            out->build = xstrdup(v.u.str.ptr);
            continue;
        }
        if (strcmp(key, "pre") == 0 || strcmp(key, "post") == 0) {
            if (v.type != TOML_STRING) {
                toml_err(v, "'%s' must be a string", key);
                bad = 1;
                continue;
            }
            char **slot = key[0] == 'p' && key[1] == 'r' ? &out->pre : &out->post;
            free(*slot);
            *slot = xstrdup(v.u.str.ptr);
            continue;
        }
        if (strcmp(key, "buildsystem") == 0) {
            if (v.type != TOML_STRING) {
                toml_err(v, "'buildsystem' must be a string naming a build system");
                bad = 1;
                continue;
            }
            free(out->buildsystem);
            out->buildsystem = xstrdup(v.u.str.ptr);
            continue;
        }
        if (strcmp(key, "sources") == 0) {
            if (v.type != TOML_BOOLEAN) {
                toml_err(v, "'sources' must be true or false");
                bad = 1;
                continue;
            }
            out->sources = v.u.boolean;
            continue;
        }
        if (strcmp(key, "version") == 0) {
            if (v.type != TOML_STRING) {
                toml_err(v, "'version' must be a string");
                bad = 1;
                continue;
            }
            free(out->version);
            out->version = xstrdup(v.u.str.ptr);
            continue;
        }
        /* An array of strings is one space-separated value: a build system's
         * script wants `-DA -DB` as one word-splittable string, not a list. */
        if (v.type == TOML_ARRAY) {
            strlist_t items;
            strlist_init(&items);
            if (toml_req_strarray(root, key, NULL, &items) != 0) {
                bad = 1;
                continue;
            }
            char *joined = join_sp(&items);
            strlist_free(&items);
            pairlist_add(&out->vars, key, joined);
            free(joined);
            continue;
        }
        /* Everything else has to be a scalar and becomes a shell variable,
         * which is how a v1 recipe carried `version`, `git` and `tag` into its
         * body. A table has no meaning here, so it is an error rather than
         * something silently ignored. */
        char *text = toml_scalar_to_str(v);
        if (text == NULL) {
            if (v.type == TOML_TABLE && manifest_table(key))
                continue;
            toml_err(v, "unknown key '%s' of type %s", key, toml_type_name(v.type));
            bad = 1;
            continue;
        }
        pairlist_add(&out->vars, key, text);
        free(text);
    }
    toml_doc_free(&doc);
    return bad ? -1 : 0;
}

int package_load(const char *packages, const char *name, const char *sys,
                 const system_t *system, pkg_t *out) {
    memset(out, 0, sizeof *out);
    out->sources = 1; /* a build starts from the source trees it depends on */
    strlist_init(&out->deps);
    strlist_init(&out->probes);
    pairlist_init(&out->vars);
    out->name = xstrdup(name);
    out->sys = xstrdup(sys != NULL ? sys : "");
    out->dir = tree_root_with(packages, name);

    if (out->dir == NULL) {
        fprintf(stderr, "retrolunar: no package '%s' in %s\n", name,
                packages != NULL ? packages : "(no tree)");
        package_free(out);
        return -1;
    }

    char *file = NULL;
    if (strcmp(out->sys, "source") == 0) {
        char *p = path_join(out->dir, "source.toml");
        if (is_file(p))
            file = p;
        else
            free(p);
    } else {
        file = package_build_file(packages, name, sys, system);
    }
    if (file == NULL) {
        fprintf(stderr,
                "retrolunar: package '%s' has no recipe for system '%s'%s\n"
                "  looked for %s/%s.toml, the system's fallbacks, and %s/generic.toml\n",
                name, out->sys, strcmp(out->sys, "source") == 0 ? " (needs source.toml)" : "",
                name, out->sys, name);
        package_free(out);
        return -1;
    }
    out->file = file;
    if (load_doc(file, out) != 0) {
        package_free(out);
        return -1;
    }
    return 0;
}

int package_load_file(const char *file, const char *name, const char *sys, pkg_t *out) {
    memset(out, 0, sizeof *out);
    out->sources = 1;
    strlist_init(&out->deps);
    strlist_init(&out->probes);
    pairlist_init(&out->vars);
    out->name = xstrdup(name);
    out->sys = xstrdup(sys != NULL ? sys : "");
    const char *slash = strrchr(file, '/');
    if (slash == NULL) {
        out->dir = xstrdup(".");
    } else {
        size_t n = (size_t)(slash - file);
        out->dir = xmalloc(n + 1);
        memcpy(out->dir, file, n);
        out->dir[n] = '\0';
    }
    out->file = xstrdup(file);
    if (load_doc(file, out) != 0) {
        package_free(out);
        return -1;
    }
    return 0;
}

void package_free(pkg_t *p) {
    free(p->name);
    free(p->sys);
    free(p->dir);
    free(p->file);
    free(p->version);
    free(p->build);
    free(p->pre);
    free(p->post);
    free(p->buildsystem);
    strlist_free(&p->deps);
    strlist_free(&p->probes);
    pairlist_free(&p->vars);
    memset(p, 0, sizeof *p);
}
