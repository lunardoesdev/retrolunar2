#include "system.h"
#include "tree.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SYSTEM_PREFIX "system@"

/* --- small helpers ------------------------------------------------------- */

static const char *base_name(const char *key) {
    const char *dot = strrchr(key, '.');
    return dot != NULL ? dot + 1 : key;
}

static int is_ident(const char *s) {
    if (s[0] == '\0')
        return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    for (const char *p = s + 1; *p != '\0'; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    return 1;
}

static int symtab_add(system_t *s, const char *key, const char *value, const char *file,
                      int line) {
    for (size_t i = 0; i < s->syms.n; i++) {
        if (strcmp(s->syms.v[i].key, key) != 0)
            continue;
        fprintf(stderr,
                "retrolunar: %s:%d: '%s' is already defined\n"
                "  first defined in %s:%d — peer files merge and may not collide\n",
                file, line, key, s->syms.v[i].file, s->syms.v[i].line);
        return -1;
    }
    if (s->syms.n == s->syms.cap) {
        s->syms.cap = s->syms.cap ? s->syms.cap * 2 : 32;
        s->syms.v = xrealloc(s->syms.v, s->syms.cap * sizeof *s->syms.v);
    }
    sym_t e;
    e.key = xstrdup(key);
    e.value = xstrdup(value);
    e.file = xstrdup(file);
    e.line = line;
    s->syms.v[s->syms.n++] = e;
    return 0;
}

static void emit_add(system_t *s, enum emit_kind kind, const char *name, const char *value,
                     const char *file, int line) {
    if (s->nemits == s->emitcap) {
        s->emitcap = s->emitcap ? s->emitcap * 2 : 32;
        s->emits = xrealloc(s->emits, s->emitcap * sizeof *s->emits);
    }
    emit_t e;
    e.kind = kind;
    e.name = name != NULL ? xstrdup(name) : NULL;
    e.value = xstrdup(value);
    e.file = xstrdup(file);
    e.line = line;
    e.multiline = strchr(value, '\n') != NULL;
    s->emits[s->nemits++] = e;
}

const char *system_sym(const system_t *s, const char *path) {
    for (size_t i = 0; i < s->syms.n; i++)
        if (strcmp(s->syms.v[i].key, path) == 0)
            return s->syms.v[i].value;
    return NULL;
}

/* --- walking one document ------------------------------------------------ */

/* Dotted path join: symbol keys are paths in the document tree, not in the
 * filesystem, so `system` + `triplet` is `system.triplet`. */
static char *dot_join(const char *prefix, const char *key) {
    if (prefix[0] == '\0')
        return xstrdup(key);
    size_t na = strlen(prefix), nb = strlen(key);
    char *p = xmalloc(na + nb + 2);
    memcpy(p, prefix, na);
    p[na] = '.';
    memcpy(p + na + 1, key, nb + 1);
    return p;
}

/* Two passes per file: `script` fragments first (they may define things the
 * variables below them use), then the scalar variables. Any defect sets *bad,
 * which makes the whole system load fail: a half-read system would emit a
 * script that builds something other than what the files say. */
static void walk(toml_datum_t tab, const char *prefix, int pass, const char *file,
                 system_t *mutable_s, int *bad, int fill) {
    toml_iter_t it;
    toml_iter_init(&it, tab);
    const char *key;
    toml_datum_t v;
    while (toml_iter_next(&it, &key, &v)) {
        char *path = dot_join(prefix, key);
        if (fill && system_sym(mutable_s, path) != NULL) {
            free(path);
            continue; /* the system's own value wins; the definition fills gaps */
        }
        if (v.type == TOML_TABLE) {
            walk(v, path, pass, file, mutable_s, bad, fill);
            free(path);
            continue;
        }
        if (v.type == TOML_ARRAY) {
            if (pass != 0) {
                free(path);
                continue;
            }
            if (strcmp(path, "system.fallbacks") == 0) {
                strlist_free(&mutable_s->fallbacks);
                strlist_init(&mutable_s->fallbacks);
                for (int i = 0; i < v.u.arr.size; i++) {
                    toml_datum_t e = v.u.arr.elem[i];
                    if (e.type != TOML_STRING) {
                        toml_err(e, "system.fallbacks[%d] must be a string", i);
                        *bad = 1;
                    } else {
                        strlist_add(&mutable_s->fallbacks, e.u.str.ptr);
                    }
                }
            } else if (strcmp(path, "system.probe") == 0) {
                strlist_free(&mutable_s->probes);
                strlist_init(&mutable_s->probes);
                for (int i = 0; i < v.u.arr.size; i++) {
                    toml_datum_t e = v.u.arr.elem[i];
                    if (e.type != TOML_STRING) {
                        toml_err(e, "system.probe[%d] must be a string", i);
                        *bad = 1;
                    } else {
                        strlist_add(&mutable_s->probes, e.u.str.ptr);
                    }
                }
                mutable_s->has_probe = 1;
            } else {
                strlist_t items;
                strlist_init(&items);
                for (int i = 0; i < v.u.arr.size; i++) {
                    toml_datum_t e = v.u.arr.elem[i];
                    if (e.type != TOML_STRING) {
                        toml_err(e, "'%s[%d]' must be a string", path, i);
                        *bad = 1;
                    } else {
                        strlist_add(&items, e.u.str.ptr);
                    }
                }
                char *joined = join_sp(&items);
                strlist_free(&items);
                /* A system's own value wins over a definition's. */
                if (!fill || system_sym(mutable_s, path) == NULL)
                    symtab_add(mutable_s, path, joined, file, v.lineno);
                free(joined);
            }
            free(path);
            continue;
        }
        if (strcmp(path, "script") == 0) {
            if (fill) {
                free(path);
                continue;
            }
            if (pass == 1) {
                if (v.type != TOML_STRING) {
                    toml_err(v, "'script' must be a string");
                    *bad = 1;
                } else {
                    emit_add(mutable_s, EMIT_SCRIPT, NULL, v.u.str.ptr, file, v.lineno);
                }
            }
            free(path);
            continue;
        }
        if (pass == 0) {
            free(path);
            continue;
        }
        char *text = toml_scalar_to_str(v);
        if (text == NULL) {
            toml_err(v, "'%s' must be a scalar", path);
            *bad = 1;
            free(path);
            continue;
        }
        const char *name = base_name(path);
        if (!is_ident(name)) {
            toml_err(v, "'%s' does not give a usable shell variable name", path);
            *bad = 1;
            free(text);
            free(path);
            continue;
        }
        if (symtab_add(mutable_s, path, text, file, v.lineno) != 0)
            *bad = 1;
        /* `buildsystem.<name>.script` (and its pre/post) is the build system's
         * own text: the emitter splices it where the recipe asks for that build
         * system, so it must not also appear as a shell variable. */
        int is_bs_text = strncmp(path, "buildsystem.", 12) == 0 &&
                         (strcmp(name, "script") == 0 || strcmp(name, "pre") == 0 ||
                          strcmp(name, "post") == 0);
        /* Also in fill mode: a default the definition supplies is a variable
         * like any other. It only gets here when the system had no such key. */
        if (!is_bs_text)
            emit_add(mutable_s, EMIT_VAR, name, text, file, v.lineno);
        free(text);
        free(path);
    }
}

/* system.toml is always read first: it holds the toolchain, the search paths
 * and the machine identities everything else refers to. The rest follow in
 * name order. */
static int cmp_files(const void *a, const void *b) {
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    int xs = strcmp(x, "system.toml") == 0;
    int ys = strcmp(y, "system.toml") == 0;
    if (xs != ys)
        return ys - xs;
    return strcmp(x, y);
}

static int system_load_from(const char *root, const char *name, system_t *out);

int system_load(const char *packages, const char *name, system_t *out) {
    /* A system comes from the first repository that defines one, like a
     * package: the repositories are one tree, in order. */
    char *dir_name = str_concat("system@", name);
    char *root = tree_root_with_entry(packages, dir_name, "system.toml");
    free(dir_name);
    if (root == NULL)
        return -1;
    int rc = system_load_from(root, name, out);
    free(root);
    return rc;
}

static int system_load_from(const char *root, const char *name, system_t *out) {
    memset(out, 0, sizeof *out);
    strlist_init(&out->fallbacks);
    strlist_init(&out->probes);
    char *dir_name = str_concat(SYSTEM_PREFIX, name);
    out->dir = path_join(root, dir_name);
    free(dir_name);
    if (!is_dir(out->dir)) {
        fprintf(stderr, "retrolunar: no system '%s' (no directory %s)\n", name, out->dir);
        return -1;
    }
    out->name = xstrdup(name);

    strlist_t all;
    strlist_init(&all);
    list_dir(out->dir, &all);
    strlist_t tomls;
    strlist_init(&tomls);
    for (size_t i = 0; i < all.n; i++) {
        size_t len = strlen(all.v[i]);
        if (len > 5 && strcmp(all.v[i] + len - 5, ".toml") == 0)
            strlist_add(&tomls, all.v[i]);
    }
    if (tomls.n > 1)
        qsort(tomls.v, tomls.n, sizeof *tomls.v, cmp_files);

    int rc = 0;
    out->docs = xmalloc((tomls.n ? tomls.n : 1) * sizeof *out->docs);
    out->files = xmalloc((tomls.n ? tomls.n : 1) * sizeof *out->files);
    for (size_t i = 0; i < tomls.n; i++)
        out->files[i] = NULL;
    for (size_t i = 0; i < tomls.n; i++) {
        char *path = path_join(out->dir, tomls.v[i]);
        toml_doc_t doc;
        if (toml_doc_load(&doc, path) != 0) {
            free(path);
            rc = -1;
            break;
        }
        out->docs[out->ndocs] = doc;
        out->files[out->ndocs] = path;
        out->ndocs++;
    }
    if (rc == 0) {
        int bad = 0;
        for (size_t i = 0; i < out->ndocs; i++) {
            walk(toml_doc_root(&out->docs[i]), "", 0, out->files[i], out, &bad, 0);
            walk(toml_doc_root(&out->docs[i]), "", 1, out->files[i], out, &bad, 0);
        }
        if (bad)
            rc = -1;
    }
    strlist_free(&all);
    strlist_free(&tomls);
    if (rc != 0)
        system_free(out);
    return rc;
}

void system_free(system_t *s) {
    for (size_t i = 0; i < s->ndocs; i++) {
        toml_doc_free(&s->docs[i]);
        free(s->files[i]);
    }
    free(s->docs);
    free(s->files);
    for (size_t i = 0; i < s->syms.n; i++) {
        free(s->syms.v[i].key);
        free(s->syms.v[i].value);
        free(s->syms.v[i].file);
    }
    free(s->syms.v);
    for (size_t i = 0; i < s->nemits; i++) {
        free(s->emits[i].name);
        free(s->emits[i].value);
        free(s->emits[i].file);
    }
    free(s->emits);
    strlist_free(&s->fallbacks);
    strlist_free(&s->probes);
    free(s->name);
    free(s->dir);
    memset(s, 0, sizeof *s);
}

/* --- ${...} expansion ---------------------------------------------------- */

char *system_expand(const system_t *s, const char *text, const char *where) {
    (void)where;
    size_t cap = strlen(text) + 64, n = 0;
    char *out = xmalloc(cap);
    const char *p = text;
    while (*p != '\0') {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end != NULL) {
                char *name = xstrndup(p + 2, (size_t)(end - p - 2));
                const char *val = system_sym(s, name);
                if (val != NULL) {
                    size_t need = n + strlen(val) + 1;
                    if (need > cap) {
                        while (cap < need)
                            cap *= 2;
                        out = xrealloc(out, cap);
                    }
                    memcpy(out + n, val, strlen(val));
                    n += strlen(val);
                    free(name);
                    p = end + 1;
                    continue;
                }
                /* Anything else is shell's business: parameter expansion with
                 * defaults or suffixes, or a plain variable this tool has never
                 * heard of. The script is made to be edited, so the shell, not
                 * this program, decides what its own syntax means. */
                free(name);
            }
        }
        if (n + 2 > cap) {
            cap *= 2;
            out = xrealloc(out, cap);
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return out;
}

/* Double-quoted shell assignment, v1 style: the runtime shell expands
 * $PREFIX/$OUT/$WORK at assignment time, so only quotes, backslashes and
 * backticks need escaping here. */
static void emit_quoted(FILE *out, const char *value) {
    fputc('"', out);
    /* Escape what the shell would read differently inside double quotes. A
     * backslash before anything else is left alone, because that is how a value
     * says "this dollar is literal": `\$ORIGIN` has to reach the linker as
     * `$ORIGIN`, and escaping the backslash would let the shell eat it. */
    for (const char *p = value; *p != '\0'; p++) {
        if (*p == '"' || *p == '`')
            fputc('\\', out);
        else if (*p == '\\' && (p[1] == '\0' || p[1] == '"' || p[1] == '`' || p[1] == '\\'))
            fputc('\\', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

/* Names a fragment's shell part assigns. Every name a fragment defines is
 * exported, so a fragment never has to say `export` for a program it goes on to
 * run (cmake's toolchain file reads $ENV{CC}, for one). */
static void collect_assignments(const char *text, strlist_t *out) {
    for (const char *p = text; *p != '\0';) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        size_t i = 0;
        while (i < len && (p[i] == ' ' || p[i] == '\t'))
            i++;
        if (len - i > 7 && strncmp(p + i, "export ", 7) == 0)
            i += 7;
        size_t j = i;
        while (j < len && (isalnum((unsigned char)p[j]) || p[j] == '_'))
            j++;
        if (j > i && j < len && p[j] == '=' && p[i] != '_' &&
            !isdigit((unsigned char)p[i])) {
            char name[128];
            size_t n = j - i < sizeof(name) - 1 ? j - i : sizeof(name) - 1;
            memcpy(name, p + i, n);
            name[n] = '\0';
            int seen = 0;
            for (size_t k = 0; k < out->n; k++)
                if (strcmp(out->v[k], name) == 0)
                    seen = 1;
            if (!seen)
                strlist_add(out, name);
        }
        p = eol ? eol + 1 : p + len;
    }
}

/* Does this fragment value read $name (or ${name})? */
static int value_uses(const emit_t *e, const char *name) {
    size_t n = strlen(name);
    for (const char *p = e->value; (p = strchr(p, '$')) != NULL; p++) {
        const char *q = p + 1;
        if (*q == '{')
            q++;
        if (strncmp(q, name, n) == 0 && q[n] != '_' && !isalnum((unsigned char)q[n]))
            return 1;
    }
    return 0;
}

void system_emit(const system_t *s, FILE *out) {
    for (size_t f = 0; f < s->ndocs; f++) {
        const char *file = s->files[f];
        strlist_t names;
        strlist_init(&names);
        /* Scripts first: they are where computation happens. */
        for (size_t i = 0; i < s->nemits; i++) {
            emit_t *e = &s->emits[i];
            if (e->kind != EMIT_SCRIPT || strcmp(e->file, file) != 0)
                continue;
            char *text = system_expand(s, e->value, e->file);
            if (text == NULL)
                continue;
            collect_assignments(text, &names);
            fputs(text, out);
            if (text[0] != '\0' && text[strlen(text) - 1] != '\n')
                fputc('\n', out);
            free(text);
        }
        /* Then the variables, with one export line for the file. A value may use
         * another value in the same file (`CMAKE_FLAGS` uses `CMAKE_MAKE_PROGRAM`),
         * and v1 wrote the definition first. Merging a fragment's appends into one
         * value can put the user before the definition, so order them by what they
         * use and keep the fragment's own order everywhere else. */
        size_t order[64];
        size_t norder = 0;
        for (size_t i = 0; i < s->nemits && norder < 64; i++) {
            emit_t *e = &s->emits[i];
            if (e->kind != EMIT_VAR || e->multiline || strcmp(e->file, file) != 0)
                continue;
            order[norder++] = i;
        }
        for (size_t done = 0; done < norder; done++) {
            size_t pick = done;
            for (size_t k = done; k < norder; k++) {
                int ready = 1;
                for (size_t j = 0; j < norder && ready; j++) {
                    if (j == k || !value_uses(&s->emits[order[k]], s->emits[order[j]].name))
                        continue;
                    int already = 0;
                    for (size_t m = 0; m < done; m++)
                        if (order[m] == order[j])
                            already = 1;
                    if (!already)
                        ready = 0;
                }
                if (ready) {
                    pick = k;
                    break;
                }
            }
            size_t swap = order[done];
            order[done] = order[pick];
            order[pick] = swap;
        }
        for (size_t o = 0; o < norder; o++) {
            emit_t *e = &s->emits[order[o]];
            char *text = system_expand(s, e->value, e->file);
            if (text == NULL)
                continue;
            fprintf(out, "%s=", e->name);
            emit_quoted(out, text);
            fputc('\n', out);
            free(text);
            strlist_add(&names, e->name);
        }
        /* Values written as raw shell keep their place in the fragment. They are
         * shell, so what they define is exported like anything else. */
        for (size_t i = 0; i < s->nemits; i++) {
            emit_t *e = &s->emits[i];
            if (e->kind != EMIT_VAR || !e->multiline || strcmp(e->file, file) != 0)
                continue;
            char *text = system_expand(s, e->value, e->file);
            if (text == NULL)
                continue;
            collect_assignments(text, &names);
            fputs(text, out);
            if (text[0] != '\0' && text[strlen(text) - 1] != '\n')
                fputc('\n', out);
            free(text);
        }
        if (names.n > 0) {
            fputs("export", out);
            for (size_t i = 0; i < names.n; i++)
                fprintf(out, " %s", names.v[i]);
            fputc('\n', out);
        }
        strlist_free(&names);
    }
}
