/* export --to DIR <target | path> — runtime shipping, not an SDK.
 *
 * Take a package's binaries and shared libraries, read what they load at run
 * time, find those files inside the nest, and gather the lot flat into DIR so
 * it can be handed over on a flash drive. Each file is resolved by name, so a
 * versioned chain (libz.so.1 -> libz.so.1.3.1) comes along whole, and the
 * whole thing is recursive: a library's own libraries are collected too.
 *
 * Anything that cannot be found is classified rather than ignored: what the
 * platform or the toolchain provides is fine to leave out, anything else is
 * reported, and --strict turns that into a failure. An export that is quietly
 * broken is worse than one that refuses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bin.h"
#include "cmds.h"
#include "resolve.h"
#include "tree.h"

/* --- an index of the nest: file name -> path ----------------------------- */

typedef struct {
    char *name; /* basename */
    char *path;
} idxent_t;

typedef struct {
    idxent_t *v;
    size_t n, cap;
} index_t;

static void index_add(index_t *ix, const char *name, const char *path) {
    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 64;
        ix->v = xrealloc(ix->v, ix->cap * sizeof *ix->v);
    }
    ix->v[ix->n].name = xstrdup(name);
    ix->v[ix->n].path = xstrdup(path);
    ix->n++;
}

static void index_free(index_t *ix) {
    for (size_t i = 0; i < ix->n; i++) {
        free(ix->v[i].name);
        free(ix->v[i].path);
    }
    free(ix->v);
    memset(ix, 0, sizeof *ix);
}

static const char *index_find(const index_t *ix, const char *name) {
    for (size_t i = 0; i < ix->n; i++)
        if (strcmp(ix->v[i].name, name) == 0)
            return ix->v[i].path;
    return NULL;
}

/* Everything loadable under a directory: lib, lib64, bin and the directory
 * itself, one level deep. v2 lays each package out under its own name; a v1
 * nest lays them out by system, and both are indexed, because export works on
 * whatever nest it is pointed at. */
static void index_dir(index_t *ix, const char *dir, int depth) {
    if (depth > 2 || !is_dir(dir))
        return;
    strlist_t names;
    strlist_init(&names);
    list_dir(dir, &names);
    for (size_t i = 0; i < names.n; i++) {
        char *full = path_join(dir, names.v[i]);
        if (is_dir(full)) {
            const char *n = names.v[i];
            if (strcmp(n, "lib") == 0 || strcmp(n, "lib64") == 0 ||
                strcmp(n, "bin") == 0 || strcmp(n, "sbin") == 0)
                index_dir(ix, full, depth + 1);
        } else if (is_file(full) || is_symlink(full)) {
            if (index_find(ix, names.v[i]) == NULL)
                index_add(ix, names.v[i], full);
        }
        free(full);
    }
    strlist_free(&names);
}

static void index_nest(index_t *ix, const char *nest) {
    strlist_t pkgs;
    strlist_init(&pkgs);
    list_dir(nest, &pkgs);
    for (size_t i = 0; i < pkgs.n; i++) {
        char *dir = path_join(nest, pkgs.v[i]);
        if (is_dir(dir))
            index_dir(ix, dir, 1);
        free(dir);
    }
    strlist_free(&pkgs);
}

/* --- collecting ---------------------------------------------------------- */

typedef struct {
    strlist_t files;      /* absolute paths to copy, in collection order */
    strlist_t seen;       /* what the walk has already looked at */
    strlist_t unresolved; /* names nothing provides */
    strlist_t platform;   /* names the platform provides: reported, not copied */
} bundle_t;

static void collect(const index_t *ix, bundle_t *b, const char *path, int depth);

static void collect_needed(const index_t *ix, bundle_t *b, const char *name, int depth) {
    const char *found = index_find(ix, name);
    if (found == NULL) {
        if (bin_name_is_platform(name)) {
            if (!strlist_has(&b->platform, name))
                strlist_add(&b->platform, name);
        } else if (!strlist_has(&b->unresolved, name)) {
            strlist_add(&b->unresolved, name);
        }
        return;
    }
    collect(ix, b, found, depth);
}

static void collect(const index_t *ix, bundle_t *b, const char *path, int depth) {
    /* `seen` is what the walk visited; `files` is what gets copied. They are
     * not the same list: an artifact added up front still has to be walked, and
     * without separating them the walk stops at the file it was asked about. */
    if (depth > 32 || strlist_has(&b->seen, path))
        return;
    strlist_add(&b->seen, path);
    if (!strlist_has(&b->files, path))
        strlist_add(&b->files, path);

    bin_t bin;
    if (bin_open(path, &bin) != 0)
        return; /* a plain data file: nothing it loads */
    strlist_t needed;
    strlist_init(&needed);
    if (bin_needed(&bin, &needed) != 0) {
        strlist_free(&needed);
        bin_close(&bin);
        return;
    }
    for (size_t i = 0; i < needed.n; i++)
        collect_needed(ix, b, needed.v[i], depth + 1);
    strlist_free(&needed);
    bin_close(&bin);
}

/* Every regular file under bin/, lib/, lib64/ that could be shipped: an
 * executable, or something with a library's name. */
static void add_artifacts(const char *pkgdir, bundle_t *b) {
    static const char *const SUB[] = {"bin", "lib", "lib64", "sbin"};
    for (size_t s = 0; s < sizeof SUB / sizeof SUB[0]; s++) {
        char *dir = path_join(pkgdir, SUB[s]);
        if (is_dir(dir)) {
            strlist_t names;
            strlist_init(&names);
            list_dir(dir, &names);
            for (size_t i = 0; i < names.n; i++) {
                char *full = path_join(dir, names.v[i]);
                if (!is_file(full) && !is_symlink(full)) {
                    free(full);
                    continue;
                }
                const char *n = names.v[i];
                int lib = strstr(n, ".so") != NULL || strstr(n, ".dll") != NULL ||
                          strstr(n, ".dylib") != NULL;
                int exe = access(full, X_OK) == 0;
                if ((lib || exe) && !strlist_has(&b->files, full))
                    strlist_add(&b->files, full);
                free(full);
            }
            strlist_free(&names);
        }
        free(dir);
    }
}

/* --- copying ------------------------------------------------------------- */

static char *target_path(const char *to, const char *src) {
    const char *base = strrchr(src, '/');
    base = base != NULL ? base + 1 : src;
    return path_join(to, base);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (in == NULL)
        return -1;
    FILE *out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    unsigned char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (n == 0 && ferror(in))
        rc = -1;
    fclose(in);
    if (fclose(out) != 0)
        rc = -1;
    if (rc == 0) {
        struct stat st;
        if (stat(from, &st) == 0)
            chmod(to, st.st_mode & 07777);
    }
    return rc;
}

/* A copy of everything that shares a versioned name with `src`, symlink chain
 * and all: libz.so.1.3.1 is useless without the libz.so.1 that points at it. */
static void copy_chain(const char *src, const char *to_dir, strlist_t *copied) {
    char *dir = xstrdup(src);
    char *slash = strrchr(dir, '/');
    char *base = NULL;
    if (slash != NULL) {
        *slash = '\0';
        base = slash + 1;
    } else {
        free(dir);
        return;
    }
    /* The real file, when src is itself a symlink. */
    char target[4096];
    ssize_t l = readlink(src, target, sizeof target - 1);
    char *real = NULL;
    if (l > 0) {
        target[l] = '\0';
        if (target[0] == '/')
            real = xstrdup(target);
        else
            real = path_join(dir, target);
    } else {
        real = xstrdup(src);
    }

    const char *realbase = strrchr(real, '/');
    realbase = realbase != NULL ? realbase + 1 : real;
    char *dest_real = path_join(to_dir, realbase);
    if (!strlist_has(copied, real)) {
        if (copy_file(real, dest_real) == 0)
            strlist_add(copied, real);
        else
            fprintf(stderr, "retrolunar: cannot copy %s\n", real);
    }
    free(dest_real);

    if (strcmp(realbase, base) != 0) {
        char *dest_link = path_join(to_dir, base);
        unlink(dest_link);
        if (symlink(realbase, dest_link) != 0)
            fprintf(stderr, "retrolunar: cannot link %s -> %s\n", base, realbase);
        free(dest_link);
    }
    free(real);
    free(dir);
}

/* An ELF artifact finds its own libraries when the loader's search path holds
 * $ORIGIN. An existing RUNPATH/RPATH string is rewritten in place; with no such
 * entry the tool adds one, using a free dynamic slot and the padding after a
 * loaded segment. When neither is available the file is left alone and the
 * caller is told, because a bundle whose libraries are not the ones it ships is
 * worse than one that says so. */
static int bake_rpath(const char *path, int enabled) {
    bin_t bin;
    if (bin_open(path, &bin) != 0)
        return 0;
    if (bin.kind != BIN_ELF) {
        /* mingw: a DLL beside the .exe is found by the loader that is already
         * running, so there is nothing to bake. */
        bin_close(&bin);
        return 0;
    }
    int ok = 0;
    char value[4096];
    size_t off = 0, len = 0;
    int rc = bin_rpath(&bin, value, sizeof value, &off, &len);
    if (rc == 0) {
        if (strcmp(value, "$ORIGIN") == 0) {
            ok = 1;
        } else if (enabled) {
            if (bin_patch_rpath(path, &bin, off, len, "$ORIGIN") == 0) {
                printf("retrolunar: %s: rpath rewritten to $ORIGIN (was \"%s\")\n", path,
                       value);
                ok = 1;
            } else {
                printf("retrolunar: %s: rpath \"%s\" is too short for $ORIGIN, left alone\n",
                       path, value);
            }
        }
    } else if (enabled) {
        char note[256];
        int added = bin_add_origin_rpath(path, &bin, note, sizeof note);
        if (added == 0) {
            printf("retrolunar: %s: %s\n", path, note);
            ok = 1;
        } else if (added == 1) {
            printf("retrolunar: %s: no $ORIGIN rpath (%s)\n", path, note);
        }
    }
    bin_close(&bin);
    return ok;
}

/* --- the command -------------------------------------------------------- */

int cmd_export(const options_t *o) {
    if (o->args.n != 1) {
        fputs("retrolunar: export wants exactly one <pack[@sys]> or a file path\n", stderr);
        return 1;
    }
    if (o->to == NULL) {
        fputs("retrolunar: export wants --to DIR\n", stderr);
        return 1;
    }

    char *nest = tree_nest(o->nest);
    if (nest == NULL) {
        fputs("retrolunar: no nest directory (set HOME or pass --nest)\n", stderr);
        return 1;
    }

    index_t ix;
    memset(&ix, 0, sizeof ix);
    index_nest(&ix, nest);

    bundle_t b;
    memset(&b, 0, sizeof b);
    strlist_init(&b.files);
    strlist_init(&b.seen);
    strlist_init(&b.unresolved);
    strlist_init(&b.platform);

    graph_t g;
    int have_graph = 0;
    const char *arg = o->args.v[0];

    if (strchr(arg, '/') != NULL || is_file(arg)) {
        /* A path: ship that file and what it loads. */
        char *abs = path_abs(arg);
        if (abs == NULL || !is_file(abs)) {
            fprintf(stderr, "retrolunar: %s is not a file\n", arg);
            free(abs);
            free(nest);
            index_free(&ix);
            return 1;
        }
        collect(&ix, &b, abs, 0);
        free(abs);
    } else {
        char *packages = tree_packages(o->packages);
        if (packages == NULL) {
            index_free(&ix);
            free(nest);
            return 1;
        }
        if (resolve_targets(packages, &arg, 1, &g) != 0) {
            free(packages);
            index_free(&ix);
            free(nest);
            return 1;
        }
        have_graph = 1;
        pkg_t *p = &g.nodes[g.n - 1].pkg;
        /* The package's own directory in the nest, not its recipe directory:
         * that is where the artifacts are. */
        char *name_sys = xmalloc(strlen(p->name) + strlen(p->sys) + 2);
        sprintf(name_sys, "%s@%s", p->name, p->sys);
        char *pkgdir = path_join(nest, name_sys);
        if (!is_dir(pkgdir))
            fprintf(stderr, "retrolunar: %s is not built yet (%s)\n", arg, pkgdir);
        add_artifacts(pkgdir, &b);
        free(pkgdir);
        free(name_sys);
        /* Name each artifact after where it will live, so the walk adds its
         * libraries next to it. */
        for (size_t i = 0; i < b.files.n; i++)
            collect(&ix, &b, b.files.v[i], 0);
        free(packages);
    }

    if (mkdirs(o->to) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", o->to);
        goto fail;
    }

    strlist_t copied;
    strlist_init(&copied);
    /* Dependencies first: a library is in place before the file that wants it. */
    for (size_t i = 0; i < b.files.n; i++)
        copy_chain(b.files.v[i], o->to, &copied);

    int rc = 0;
    printf("retrolunar: exported %zu file(s) from %zu package file(s) to %s\n", copied.n,
           b.files.n, o->to);
    if (b.platform.n > 0) {
        printf("retrolunar: provided by the platform or toolchain, not copied:");
        for (size_t i = 0; i < b.platform.n; i++)
            printf(" %s", b.platform.v[i]);
        printf("\n");
    }
    if (b.unresolved.n > 0) {
        printf("retrolunar: NOT FOUND anywhere in %s:", nest);
        for (size_t i = 0; i < b.unresolved.n; i++)
            printf(" %s", b.unresolved.v[i]);
        printf("\n");
        if (o->strict) {
            fputs("retrolunar: --strict: refusing to call this bundle complete\n", stderr);
            rc = 1;
        }
    }

    if (o->rpath) {
        size_t missed = 0;
        for (size_t i = 0; i < copied.n; i++) {
            char *dest = target_path(o->to, copied.v[i]);
            bin_t probe;
            int is_elf = bin_open(dest, &probe) == 0 && probe.kind == BIN_ELF;
            if (is_elf)
                bin_close(&probe);
            if (is_elf) {
                if (bake_rpath(dest, 1) == 0) /* 1 when it took an $ORIGIN path */
                    missed++;
            }
            free(dest);
        }
        if (missed > 0) {
            /* A flash drive has no LD_LIBRARY_PATH, so the bundle carries one:
             * a launcher that points the loader at its own directory. Written
             * only when something needs it, because when every file took an
             * $ORIGIN path there is nothing to launch through. */
            char *launcher = path_join(o->to, "run.sh");
            FILE *lf = fopen(launcher, "w");
            if (lf != NULL) {
                fputs("#!/bin/sh\n"
                      "# Everything this bundle loads is next to this script. Export could\n"
                      "# not bake an $ORIGIN runtime path into every file, so the loader is\n"
                      "# told where to look instead. Usage: ./run.sh <program> [args...]\n"
                      "_dir=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
                      "LD_LIBRARY_PATH=\"${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$_dir\"\n"
                      "export LD_LIBRARY_PATH\n"
                      "exec \"$@\"\n", lf);
                fclose(lf);
                chmod(launcher, 0755);
                printf("retrolunar: wrote %s (no runtime path could be baked into %zu file(s))\n",
                       launcher, missed);
            }
            printf("retrolunar: run from %s as: ./run.sh <program>\n", o->to);
            free(launcher);
        }
    }

    strlist_free(&copied);
    if (have_graph)
        graph_free(&g);
    strlist_free(&b.files);
    strlist_free(&b.seen);
    strlist_free(&b.unresolved);
    strlist_free(&b.platform);
    index_free(&ix);
    free(nest);
    return rc;

fail:
    if (have_graph)
        graph_free(&g);
    strlist_free(&b.files);
    strlist_free(&b.seen);
    strlist_free(&b.unresolved);
    strlist_free(&b.platform);
    index_free(&ix);
    free(nest);
    return 1;
}
