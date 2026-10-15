/* export-tarball --to DIR <target>... - the release a project hands over.
 *
 * Collect a target and its dependencies (that is what `export` does), lay them
 * out as an installed prefix - lib/, include/, share/ - and pack that prefix.
 * --to names the archive when it looks like a file (anything with a dot in its
 * last component) and then takes one target; naming a directory packs each
 * target as <pack>-<sys>.tar.gz beside a SHA256SUMS. A version is never
 * invented: when the caller wants one in the name, the caller puts it there.
 *
 * Why the dependencies ride along: an FFmpeg archive names libvpx's, libopus's,
 * lame's and zlib's symbols, so packing the prefix alone links with twenty
 * unresolved references, measured. A hand-over that cannot link is not one.
 *
 * The .pc files are rewritten to point at the prefix's own lib/ instead of the
 * nest the libraries were built in, so pkg-config answers for a tarball
 * unpacked anywhere. Nothing lands in --to until every target is packed: a
 * failure leaves that directory as it was, checksums included.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cmds.h"
#include "config.h"
#include "hash.h"
#include "repos.h"
#include "resolve.h"
#include "tree.h"
#include "util.h"

/* The nest's spelling of a dependency's link path in a .pc file: the difference
 * between it and the path that ships is the whole rewrite. */
static int pc_flag_is_nest_path(const char *token, const char *sys) {
    char lib[512], share[512];
    snprintf(lib, sizeof lib, "@%s/lib/pkgconfig/../..", sys);
    snprintf(share, sizeof share, "@%s/share/pkgconfig/../..", sys);
    return strstr(token, lib) != NULL || strstr(token, share) != NULL;
}

/* Rewrite every -L flag that points into the nest, in place: -L${libdir} is
 * shorter than any path that ends in pkgconfig/../.., so the text only shrinks
 * and one pass over it is enough. A file that already names the prefix, or has
 * no such flag, comes out byte for byte as it was. */
static void rewrite_pc_dir(const char *dir, const char *sys) {
    strlist_t names;
    strlist_init(&names);
    list_dir(dir, &names);
    for (size_t i = 0; i < names.n; i++) {
        size_t len = strlen(names.v[i]);
        if (len < 4 || strcmp(names.v[i] + len - 3, ".pc") != 0)
            continue;
        char *path = path_join(dir, names.v[i]);
        size_t n = 0;
        char *text = read_file(path, &n);
        if (text != NULL) {
            int changed = 0;
            for (char *p = strstr(text, "-L"); p != NULL; p = strstr(p, "-L")) {
                char *end = p + 2;
                while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n')
                    end++;
                size_t tlen = (size_t)(end - p - 2);
                char token[1024];
                if (tlen < sizeof token) {
                    memcpy(token, p + 2, tlen);
                    token[tlen] = '\0';
                    if (pc_flag_is_nest_path(token, sys)) {
                        const char *repl = "-L${libdir}";
                        size_t rl = strlen(repl);
                        memmove(p, repl, rl);
                        memmove(p + rl, end, strlen(end) + 1);
                        p += rl;
                        changed = 1;
                        continue;
                    }
                }
                p = end;
            }
            if (changed) {
                FILE *f = fopen(path, "w");
                if (f != NULL) {
                    fwrite(text, 1, strlen(text), f);
                    fclose(f);
                }
            }
            free(text);
        }
        free(path);
    }
    strlist_free(&names);
}

/* Every archive a run of this command makes, moved into place only once all of
 * them are packed. */
static int one_target(const options_t *o, const char *nest, const char *work,
                      const char *key, const char *archive_name, FILE *sums) {
    const char *at = strchr(key, '@');
    const char *sys = at != NULL ? at + 1 : RETROLUNAR_DEFAULT_SYSTEM;

    char *name = xstrdup(archive_name);

    char *stage = path_join(work, key);
    char *closure = path_join(work, "closure");
    char *entry = path_join(nest, key);
    char *tarpath = path_join(work, name);
    int status = 1;

    if (mkdirs(stage) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", stage);
        goto done;
    }
    if (!is_dir(entry)) {
        fprintf(stderr, "retrolunar: %s is not built yet (%s)\n", key, entry);
        goto done;
    }

    /* The prefix as it was built. */
    char *entry_lib = path_join(entry, "lib");
    char *entry_inc = path_join(entry, "include");
    char *entry_share = path_join(entry, "share");
    char *stage_share = path_join(stage, "share");
    {
        char *argv[] = {(char *)"cp", (char *)"-a", entry_lib, entry_inc, stage, NULL};
        if (run_cmd(argv) != 0) {
            fprintf(stderr, "retrolunar: cannot copy %s into %s\n", entry_lib, stage);
            free(entry_lib); free(entry_inc); free(entry_share); free(stage_share);
            goto done;
        }
        if (is_dir(entry_share)) {
            char *argv2[] = {(char *)"cp", (char *)"-a", entry_share, stage_share, NULL};
            run_cmd(argv2);
        }
    }
    free(entry_lib); free(entry_inc); free(entry_share); free(stage_share);

    /* And what it links against, asked of the tool itself rather than kept as a
     * list here. A copy of the target's own libraries is not wanted: the entry
     * is the same build, and what it already has wins. */
    options_t sub;
    memset(&sub, 0, sizeof sub);
    sub.cmd = CMD_EXPORT;
    sub.packages = o->packages;
    sub.nest = o->nest;
    sub.to = closure;
    sub.rpath = 0;  /* leave the built files exactly as they were built */
    strlist_init(&sub.args);
    strlist_add(&sub.args, key);
    remove_tree(closure);
    int exported = cmd_export(&sub);
    strlist_free(&sub.args);
    if (exported != 0) {
        fprintf(stderr, "retrolunar: cannot collect the closure of %s\n", key);
        goto done;
    }

    char *stage_lib = path_join(stage, "lib");
    strlist_t files;
    strlist_init(&files);
    list_dir(closure, &files);
    for (size_t i = 0; i < files.n; i++) {
        char *from = path_join(closure, files.v[i]);
        char *into = path_join(stage_lib, files.v[i]);
        if (access(into, F_OK) != 0 && is_file(from)) {
            char *argv[] = {(char *)"cp", (char *)"-a", from, stage_lib, NULL};
            if (run_cmd(argv) != 0)
                fprintf(stderr, "retrolunar: cannot copy %s into %s\n", from, stage_lib);
        }
        free(from);
        free(into);
    }
    strlist_free(&files);

    rewrite_pc_dir(path_join(stage_lib, "pkgconfig"), sys);
    free(stage_lib);

    /* Fast: gzip at level 1. Pigz, when the machine has it, is faster still and
     * is one word away: RETROLUNAR_TARBALL_COMPRESS. */
    const char *compress = getenv("RETROLUNAR_TARBALL_COMPRESS");
    if (compress == NULL || compress[0] == '\0')
        compress = "gzip -1";
    char *argv[12];
    int a = 0;
    argv[a++] = (char *)"tar";
    argv[a++] = (char *)"-C";
    argv[a++] = stage;
    argv[a++] = (char *)"-cf";
    argv[a++] = tarpath;
    argv[a++] = (char *)"--use-compress-program";
    argv[a++] = (char *)compress;
    argv[a++] = (char *)"lib";
    argv[a++] = (char *)"include";
    char *stage_share_dir = path_join(stage, "share");
    if (is_dir(stage_share_dir))
        argv[a++] = (char *)"share";
    free(stage_share_dir);
    argv[a] = NULL;
    if (run_cmd(argv) != 0) {
        fprintf(stderr, "retrolunar: cannot pack %s\n", tarpath);
        goto done;
    }

    char hex[65];
    struct stat st;
    if (sha256_file_hex(tarpath, hex) != 0 || stat(tarpath, &st) != 0) {
        fprintf(stderr, "retrolunar: cannot read %s back\n", tarpath);
        goto done;
    }
    fprintf(sums, "%s  %s\n", hex, name);
    printf("%-33s %10lld bytes\n", name, (long long)st.st_size);
    status = 0;

done:
    remove_tree(stage);
    remove_tree(closure);
    free(stage);
    free(closure);
    free(entry);
    free(tarpath);
    free(name);
    return status;
}

int cmd_export_tarball(const options_t *o) {
    if (o->args.n < 1) {
        fputs("retrolunar: export-tarball wants a <pack[@sys]> to release\n", stderr);
        return 1;
    }
    char *to = xstrdup(o->to != NULL ? o->to : "dist");
    const char *base = strrchr(to, '/');
    base = base != NULL ? base + 1 : to;
    /* A path whose last component has a dot in it is the archive the caller
     * wants, whatever it is called: release.tar.gz, release.tgz, release.toml.
     * Anything else - an existing directory, a bare word, a trailing slash - is
     * a directory to pack into. */
    int file_mode = o->to != NULL && !is_dir(to) && strchr(base, '.') != NULL;
    if (file_mode && o->args.n != 1) {
        fprintf(stderr, "retrolunar: --to %s names one archive, so it takes one target\n", to);
        free(to);
        return 1;
    }
    /* Where the work happens: beside the archive, or in the directory that the
     * targets are packed into. */
    char *outdir = xstrdup(to);
    if (file_mode) {
        char *slash = strrchr(outdir, '/');
        if (slash != NULL)
            *slash = '\0';
        else {
            free(outdir);
            outdir = xstrdup(".");
        }
    }
    char *nest = tree_nest(o->nest);
    if (nest == NULL) {
        fputs("retrolunar: no nest directory (set HOME or pass --nest)\n", stderr);
        return 1;
    }
    char *packages = tree_packages(o->packages);
    if (packages == NULL) {
        free(nest);
        return 1;
    }

    graph_t g;
    if (resolve_targets(packages, (const char *const *)o->args.v, o->args.n, &g) != 0) {
        free(packages);
        free(nest);
        return 1;
    }

    /* Which nodes were named: a dependency rides along inside its target's
     * archive, it does not get one of its own. */
    strlist_t wanted;
    strlist_init(&wanted);
    for (size_t i = 0; i < o->args.n; i++) {
        char key[512];
        if (target_key(o->args.v[i], RETROLUNAR_DEFAULT_SYSTEM, key, sizeof key) == 0)
            strlist_add(&wanted, key);
    }

    if (mkdirs(outdir) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", outdir);
        strlist_free(&wanted);
        graph_free(&g);
        free(packages);
        free(nest);
        free(outdir);
        free(to);
        return 1;
    }
    char *work = path_join(outdir, ".tarball-staging");
    remove_tree(work);
    if (mkdirs(work) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", work);
        strlist_free(&wanted);
        graph_free(&g);
        free(packages);
        free(nest);
        free(work);
        return 1;
    }
    char *sums_path = path_join(work, "SHA256SUMS");
    FILE *sums = fopen(sums_path, "w");
    if (sums == NULL) {
        fprintf(stderr, "retrolunar: cannot write %s\n", sums_path);
        remove_tree(work);
        strlist_free(&wanted);
        graph_free(&g);
        free(packages);
        free(nest);
        free(work);
        free(sums_path);
        return 1;
    }

    int status = 0;
    for (size_t n = 0; n < g.n && status == 0; n++) {
        if (strcmp(g.nodes[n].pkg.sys, "source") == 0)
            continue;
        if (!strlist_has(&wanted, g.nodes[n].key))
            continue;
        if (file_mode) {
            status = one_target(o, nest, work, g.nodes[n].key, base, sums);
        } else {
            const char *at = strchr(g.nodes[n].key, '@');
            size_t packlen = at != NULL ? (size_t)(at - g.nodes[n].key)
                                        : strlen(g.nodes[n].key);
            const char *sys = at != NULL ? at + 1 : RETROLUNAR_DEFAULT_SYSTEM;
            char *nm = xmalloc(packlen + strlen(sys) + 10);
            sprintf(nm, "%.*s-%s.tar.gz", (int)packlen, g.nodes[n].key, sys);
            status = one_target(o, nest, work, g.nodes[n].key, nm, sums);
            free(nm);
        }
    }
    fclose(sums);

    if (status == 0 && file_mode) {
        /* One archive, named by the caller: it takes the place it was asked for
         * only after it is whole. */
        char *from = path_join(work, base);
        if (rename(from, to) != 0) {
            fprintf(stderr, "retrolunar: cannot place %s\n", to);
            status = 1;
        }
        free(from);
        if (status == 0) {
            char hex[65];
            if (sha256_file_hex(to, hex) == 0)
                printf("%s  %s\n", hex, to);
        }
    } else if (status == 0) {
        /* Every archive is packed, so now, and only now, they replace what is
         * in the output directory. */
        strlist_t made;
        strlist_init(&made);
        list_dir(work, &made);
        for (size_t i = 0; i < made.n; i++) {
            /* Only what this command produced: the staging area also holds the
             * per-target directories and the closure it collected in. */
            size_t len = strlen(made.v[i]);
            int is_tarball = len > 7 && strcmp(made.v[i] + len - 7, ".tar.gz") == 0;
            if (!is_tarball && strcmp(made.v[i], "SHA256SUMS") != 0)
                continue;
            char *from = path_join(work, made.v[i]);
            char *into = path_join(to, made.v[i]);
            if (rename(from, into) != 0)
                fprintf(stderr, "retrolunar: cannot place %s\n", into);
            free(from);
            free(into);
        }
        strlist_free(&made);
        printf("\nsha256, in %s/SHA256SUMS:\n", to);
        char *sums_in = path_join(to, "SHA256SUMS");
        size_t n = 0;
        char *text = read_file(sums_in, &n);
        if (text != NULL) {
            fputs(text, stdout);
            free(text);
        }
        free(sums_in);
    }
    remove_tree(work);
    free(work);
    free(sums_path);
    strlist_free(&wanted);
    graph_free(&g);
    free(packages);
    free(nest);
    free(outdir);
    free(to);
    return status;
}
