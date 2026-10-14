/* cache — a package built here, handed to another nest without building it again.
 *
 * A store entry is named after everything that goes into it, so a nest that has
 * the recipe, the system and the same dependencies is *about to* build exactly
 * that directory. The cache carries the result:
 *
 *   <base>/<entry>.tar.gz   the entry directory, as `tar -C store` wrote it
 *   <base>/<entry>.sha256   the digest of those bytes, so a download is checked
 *
 * `<base>` is a directory or an http(s) url: a directory is copied to, a url is
 * PUT to and fetched with curl. Nothing is sent that has not been built, and
 * nothing is unpacked that does not match its digest.
 *
 * What the digest cannot prove is that the bytes are the bytes *this* project
 * would have produced: a fingerprint covers inputs, and no input names an
 * output. So the first entry a nest takes records what came out, and a later
 * entry with the same name and different contents is refused — the same inputs
 * do not quietly turn into a different package under the same name. That is a
 * promise about this nest, not a signature, and it is the strongest one that can
 * be made without signing anything.
 */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli.h"
#include "cmds.h"
#include "fingerprint.h"
#include "hash.h"
#include "manifest.h"
#include "resolve.h"
#include "tree.h"
#include "util.h"

/* <nest>/.retrolunar/entries: `<entry> <content digest>`, one per line, written
 * as entries are accepted. */
static char *index_path(const char *nest) {
    char *state = path_join(nest, ".retrolunar");
    mkdirs(state);
    char *p = path_join(state, "entries");
    free(state);
    return p;
}

static int index_lookup(const char *nest, const char *entry, char out[65]) {
    char *p = index_path(nest);
    FILE *f = fopen(p, "r");
    free(p);
    if (f == NULL)
        return 1;
    char name[512], hex[65];
    int found = 1;
    while (fscanf(f, "%511s %64s", name, hex) == 2) {
        if (strcmp(name, entry) == 0) {
            memcpy(out, hex, strlen(hex) + 1);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static int index_record(const char *nest, const char *entry, const char *content) {
    char *p = index_path(nest);
    FILE *f = fopen(p, "a");
    free(p);
    if (f == NULL)
        return -1;
    fprintf(f, "%s %s\n", entry, content);
    return fclose(f) == 0 ? 0 : -1;
}

/* One shell line: tar, curl and cp are what a cache is made of, and a script is
 * what the rest of this tool runs. */
static int sh_line(const char *fmt, ...) {
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

static int is_url(const char *s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/* The digest of a file, as the cache publishes it. */
static int digest_of(const char *path, char out[65]) {
    return sha256_file_hex(path, out);
}

/* The cache to use: --cache, or [cache] url in the manifest. */
static char *cache_base(const options_t *o) {
    if (o->cache != NULL)
        return xstrdup(o->cache);
    manifest_t m;
    if (manifest_find(NULL, &m) != 0)
        return NULL;
    char *url = m.cache_url != NULL ? xstrdup(m.cache_url) : NULL;
    manifest_free(&m);
    if (url == NULL)
        fprintf(stderr, "retrolunar: no cache: pass --cache DIR|URL, or set [cache] url\n");
    return url;
}

/* `tar -C store -czf FILE ENTRY`: the archive holds the entry's own directory,
 * so unpacking into a store gives a store. */
static int tar_entry(const char *store, const char *entry, const char *out) {
    return sh_line("tar -C '%s' -czf '%s' '%s'", store, out, entry);
}

static int untar_entry(const char *store, const char *entry, const char *archive,
                       const char *tmpdir) {
    if (mkdirs(tmpdir) != 0)
        return -1;
    if (sh_line("tar -xzf '%s' -C '%s'", archive, tmpdir) != 0)
        return -1;
    char *inside = path_join(tmpdir, entry);
    if (!is_dir(inside)) {
        free(inside);
        return -1;
    }
    char *dest = path_join(store, entry);
    remove_tree(dest);
    if (sh_line("mv '%s' '%s'", inside, dest) != 0) {
        free(inside);
        free(dest);
        return -1;
    }
    free(inside);
    free(dest);
    remove_tree(tmpdir);
    return 0;
}

/* A pulled entry gets the name it goes by, the same way a built one does: the
 * name is a link into the store, and a directory from before there was a store
 * is moved in rather than thrown away. */
static void ensure_name(const char *nest, const char *key, const char *entry) {
    char *store = path_join(nest, "store");
    char *dest = path_join(store, entry);
    char *name = path_join(nest, key);
    struct stat st;
    if (lstat(name, &st) == 0 && !S_ISLNK(st.st_mode)) {
        if (!is_dir(dest))
            sh_line("mv '%s' '%s' 2>/dev/null || true", name, dest);
        else
            remove_tree(name);
    }
    if (sh_line("ln -sfn '%s' '%s'", dest, name) != 0)
        fprintf(stderr, "retrolunar: could not link %s to its entry\n", key);
    free(name);
    free(dest);
    free(store);
}

int cmd_cache(const options_t *o) {
    if (o->args.n < 2) {
        fputs("retrolunar: cache push|pull TARGET...\n", stderr);
        return 1;
    }
    const char *sub = o->args.v[0];
    int pushing = strcmp(sub, "push") == 0;
    if (!pushing && strcmp(sub, "pull") != 0) {
        fprintf(stderr, "retrolunar: cache wants push or pull, not '%s'\n", sub);
        return 1;
    }

    char *nest = tree_nest(o->nest);
    /* Entries are moved here, so this waits for the running builds in that nest. */
    if (nest != NULL && nest_lock_exclusive(nest) < 0)
        return 1;
    if (nest == NULL) {
        fputs("retrolunar: no nest directory (set HOME or pass --nest)\n", stderr);
        return 1;
    }
    char *store = path_join(nest, "store");
    char *base = cache_base(o);
    if (base == NULL) {
        free(store);
        free(nest);
        return 1;
    }
    char *packages = tree_packages(o->packages);
    if (packages == NULL) {
        free(base);
        free(store);
        free(nest);
        return 1;
    }

    graph_t g;
    if (resolve_targets(packages, (const char *const *)o->args.v + 1, o->args.n - 1, &g) != 0) {
        free(packages);
        free(base);
        free(store);
        free(nest);
        return 1;
    }
    char **fps = fingerprint_nodes(&g);

    char remote = is_url(base);
    char *tmpdir = NULL;
    size_t moved = 0, skipped = 0, absent = 0, refused = 0;
    for (size_t i = 0; i < g.n; i++) {
        const char *entry = fps[i];
        char *dir = path_join(store, entry);
        char *sha = NULL, *archive = NULL;

        if (pushing) {
            if (!is_dir(dir)) {
                absent++;
                free(dir);
                continue;
            }
            char content[65];
            char *now = fingerprint_dir(dir);
            snprintf(content, sizeof content, "%s", now);
            free(now);
            char recorded[65];
            if (index_lookup(nest, entry, recorded) == 0 && strcmp(recorded, content) != 0) {
                fprintf(stderr,
                        "retrolunar: %s: this nest recorded different contents for that name; "
                        "not pushing it\n",
                        g.nodes[i].key);
                refused++;
                free(dir);
                continue;
            }

            size_t n = strlen(base) + strlen(entry) + 64;
            archive = xmalloc(n);
            sha = xmalloc(n);
            snprintf(archive, n, "%s.tmp-%d.tar.gz", entry, (int)getpid());
            if (tar_entry(store, entry, archive) != 0) {
                fprintf(stderr, "retrolunar: cannot archive %s\n", g.nodes[i].key);
                free(archive);
                free(sha);
                free(dir);
                continue;
            }
            char hex[65];
            if (digest_of(archive, hex) != 0) {
                fprintf(stderr, "retrolunar: cannot read back %s\n", archive);
                remove_tree(archive);
                free(archive);
                free(sha);
                free(dir);
                continue;
            }

            if (remote) {
                char target[n];
                snprintf(target, n, "%s/%s.tar.gz", base, entry);
                if (sh_line("curl -fsS -T '%s' '%s'", archive, target) != 0) {
                    fprintf(stderr, "retrolunar: could not send %s\n", target);
                    remove_tree(archive);
                    free(archive);
                    free(sha);
                    free(dir);
                    continue;
                }
                snprintf(target, n, "%s/%s.sha256", base, entry);
                if (sh_line("printf '%%s\\n' '%s' > '%s' && "
                            "curl -fsS -T '%s' '%s'",
                            hex, sha, sha, target) != 0)
                    fprintf(stderr, "retrolunar: sent the archive but not its digest\n");
                remove_tree(sha);
            } else {
                mkdirs(base);
                char target[n + 16];
                snprintf(target, sizeof target, "%s/%s.sha256", base, entry);
                FILE *f = fopen(target, "wb");
                if (f == NULL || fprintf(f, "%s\n", hex) < 0 || fclose(f) != 0) {
                    fprintf(stderr, "retrolunar: cannot write %s\n", target);
                    remove_tree(archive);
                    free(archive);
                    free(sha);
                    free(dir);
                    continue;
                }
                snprintf(target, sizeof target, "%s/%s.tar.gz", base, entry);
                if (sh_line("mv '%s' '%s'", archive, target) != 0) {
                    fprintf(stderr, "retrolunar: cannot write %s\n", target);
                    free(archive);
                    free(sha);
                    free(dir);
                    continue;
                }
            }
            remove_tree(archive);
            if (index_record(nest, entry, content) != 0)
                fprintf(stderr, "retrolunar: cannot record %s\n", g.nodes[i].key);
            moved++;
            free(archive);
            free(sha);
            free(dir);
            continue;
        }

        /* pull */
        if (is_dir(dir)) {
            char recorded[65];
            if (index_lookup(nest, entry, recorded) == 0) {
                char *now = fingerprint_dir(dir);
                if (strcmp(now, recorded) != 0) {
                    fprintf(stderr,
                            "retrolunar: %s: %s holds different contents than this nest "
                            "recorded; not touching it\n",
                            g.nodes[i].key, dir);
                    refused++;
                    free(now);
                    free(dir);
                    continue;
                }
                free(now);
            }
            skipped++;
            free(dir);
            continue;
        }

        size_t n = strlen(base) + strlen(entry) + 64;
        archive = xmalloc(n);
        sha = xmalloc(n);
        snprintf(archive, n, "%s.tmp-%d", entry, (int)getpid());
        snprintf(sha, n, "%s.tmp-%d.sha256", entry, (int)getpid());
        char target[n + 16];
        int got;
        if (remote) {
            snprintf(target, sizeof target, "%s/%s.tar.gz", base, entry);
            got = sh_line("curl -fsS -o '%s' '%s'", archive, target);
            snprintf(target, sizeof target, "%s/%s.sha256", base, entry);
            if (got == 0)
                got = sh_line("curl -fsS -o '%s' '%s'", sha, target);
        } else {
            snprintf(target, sizeof target, "%s/%s.sha256", base, entry);
            got = sh_line("cp '%s' '%s'", target, sha);
            snprintf(target, sizeof target, "%s/%s.tar.gz", base, entry);
            if (got == 0)
                got = sh_line("cp '%s' '%s'", target, archive);
        }
        if (got != 0) {
            absent++;
            remove_tree(archive);
            remove_tree(sha);
            free(archive);
            free(sha);
            free(dir);
            continue;
        }

        /* The bytes have to be the bytes that were published. */
        char want[80] = {0}, have[65];
        FILE *f = fopen(sha, "r");
        if (f == NULL || fscanf(f, "%79s", want) != 1) {
            if (f != NULL)
                fclose(f);
            fprintf(stderr, "retrolunar: %s: no digest came with the entry\n", g.nodes[i].key);
            absent++;
            remove_tree(archive);
            remove_tree(sha);
            free(archive);
            free(sha);
            free(dir);
            continue;
        }
        fclose(f);
        if (digest_of(archive, have) != 0 || strcmp(want, have) != 0) {
            fprintf(stderr, "retrolunar: %s: the entry does not match its digest; not using it\n",
                    g.nodes[i].key);
            absent++;
            remove_tree(archive);
            remove_tree(sha);
            free(archive);
            free(sha);
            free(dir);
            continue;
        }

        if (tmpdir == NULL) {
            tmpdir = path_join(store, ".unpack");
            mkdirs(store);
        }
        if (untar_entry(store, entry, archive, tmpdir) != 0) {
            fprintf(stderr, "retrolunar: %s: could not unpack the entry\n", g.nodes[i].key);
            absent++;
            remove_tree(archive);
            remove_tree(sha);
            free(archive);
            free(sha);
            free(dir);
            continue;
        }
        remove_tree(archive);
        remove_tree(sha);

        /* What came out, and whether this nest has seen this name before. */
        char *content_now = fingerprint_dir(dir);
        char recorded[65];
        if (index_lookup(nest, entry, recorded) == 0 && strcmp(recorded, content_now) != 0) {
            fprintf(stderr,
                    "retrolunar: %s: the entry holds different contents than this nest "
                    "recorded for the same name; removing it\n",
                    g.nodes[i].key);
            remove_tree(dir);
            refused++;
            free(content_now);
            free(dir);
            free(archive);
            free(sha);
            continue;
        }
        if (index_record(nest, entry, content_now) != 0)
            fprintf(stderr, "retrolunar: cannot record %s\n", g.nodes[i].key);
        free(content_now);
        ensure_name(nest, g.nodes[i].key, entry);
        moved++;
        free(archive);
        free(sha);
        free(dir);
    }

    printf("retrolunar: %s: %zu entr%s %s, %zu already here, %zu not in the cache%s\n", nest,
           moved, moved == 1 ? "y" : "ies", pushing ? "sent" : "fetched", skipped, absent,
           refused > 0 ? ", refused to touch 1 or more" : "");

    if (tmpdir != NULL) {
        remove_tree(tmpdir);
        free(tmpdir);
    }
    for (size_t i = 0; i < g.n; i++)
        free(fps[i]);
    free(fps);
    graph_free(&g);
    free(packages);
    free(base);
    free(store);
    free(nest);
    return absent > 0 && !pushing ? 1 : 0;
}
