/* Fetching repositories and turning them into one search path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "repos.h"
#include "tree.h"
#include "util.h"

/* `NAME=URL` or `NAME=URL@REV` from the command line. */
typedef struct {
    char *name;
    char *url;
    char *rev;
} entry_t;

static void entry_free(entry_t *e) {
    free(e->name);
    free(e->url);
    free(e->rev);
    memset(e, 0, sizeof *e);
}

/* A repository is local when its url names a directory: that is how a project
 * points at a checkout beside it, and how the tests run without a network. */
static int is_local(const char *url) {
    return url[0] == '/' || url[0] == '.' || url[0] == '~';
}

static char *expand_home(const char *p) {
    if (p[0] != '~')
        return xstrdup(p);
    const char *home = getenv("HOME");
    if (home == NULL)
        return xstrdup(p);
    return str_concat(home, p + 1);
}

static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Where a named repository is kept between runs. Repositories are shared state,
 * not project state: one clone serves every project that names it. */
static char *clone_dir(const char *name) {
    char *base = default_under_home("repos");
    if (base == NULL)
        return NULL;
    char *dir = path_join(base, name);
    free(base);
    if (mkdirs(dir) != 0) {
        fprintf(stderr, "retrolunar: cannot create %s\n", dir);
        free(dir);
        return NULL;
    }
    return dir;
}

/* Local repositories are used where they are. Remote ones are cloned once and
 * then only asked to change revision when a different one is wanted, so an
 * unchanged project never touches the network. */
static char *repo_dir(const entry_t *e) {
    char *url = expand_home(e->url);
    if (is_local(url)) {
        char *abs = path_abs(url);
        free(url);
        if (abs == NULL || !is_dir(abs)) {
            fprintf(stderr, "retrolunar: repository '%s' is not a directory: %s\n", e->name,
                    e->url);
            free(abs);
            return NULL;
        }
        return abs;
    }

    char *dir = clone_dir(e->name);
    if (dir == NULL) {
        free(url);
        return NULL;
    }
    char *git = path_join(dir, ".git");
    int have = is_dir(git);
    free(git);

    if (!have) {
        printf("retrolunar: fetching %s from %s\n", e->name, e->url);
        char *argv[] = {(char *)"git", (char *)"clone", (char *)"--quiet", url, dir, NULL};
        if (run(argv) != 0) {
            fprintf(stderr, "retrolunar: cannot clone %s from %s\n", e->name, e->url);
            free(dir);
            free(url);
            return NULL;
        }
    } else if (e->rev != NULL) {
        char *argv[] = {(char *)"git", (char *)"-C", dir, (char *)"checkout", (char *)"--quiet",
                        e->rev, NULL};
        if (run(argv) != 0) {
            /* Not in that checkout yet: fetch it, then try again. */
            char *fetch[] = {(char *)"git", (char *)"-C", dir, (char *)"fetch", (char *)"--quiet",
                             (char *)"--all", NULL};
            run(fetch);
            if (run(argv) != 0) {
                fprintf(stderr, "retrolunar: cannot check out %s in %s\n", e->rev, e->name);
                free(dir);
                free(url);
                return NULL;
            }
        }
    }
    free(url);
    return dir;
}

/* `NAME=URL[@REV]`: the last @ separates a revision, and a url may hold one
 * (an ssh url does), so only the tail after the last @ in the *name* is taken. */
static int split_cli(const char *spec, entry_t *out) {
    memset(out, 0, sizeof *out);
    const char *eq = strchr(spec, '=');
    if (eq == NULL || eq == spec) {
        fprintf(stderr, "retrolunar: --repo wants NAME=URL[@REV], not '%s'\n", spec);
        return -1;
    }
    out->name = xstrndup(spec, (size_t)(eq - spec));
    char *rest = xstrdup(eq + 1);
    char *slash = strrchr(rest, '/');
    char *at = strrchr(rest, '@');
    if (at != NULL && (slash == NULL || at > slash)) {
        *at = '\0';
        out->rev = xstrdup(at + 1);
    }
    out->url = rest;
    return 0;
}

char *repos_search_path(const manifest_t *m, const strlist_t *cli, int no_default) {
    entry_t *entries = NULL;
    size_t n = 0, cap = 0;

    if (m != NULL) {
        for (size_t i = 0; i < m->repos.n; i++) {
            if (n == cap) {
                cap = cap != 0 ? cap * 2 : 4;
                entries = xrealloc(entries, cap * sizeof *entries);
            }
            entry_t *e = &entries[n];
            memset(e, 0, sizeof *e);
            e->name = xstrdup(m->repos.v[i].name);
            e->url = xstrdup(m->repos.v[i].url);
            e->rev = m->repos.v[i].rev != NULL ? xstrdup(m->repos.v[i].rev) : NULL;
            n++;
        }
    }
    if (cli != NULL) {
        for (size_t i = 0; i < cli->n; i++) {
            if (n == cap) {
                cap = cap != 0 ? cap * 2 : 4;
                entries = xrealloc(entries, cap * sizeof *entries);
            }
            if (split_cli(cli->v[i], &entries[n]) != 0) {
                for (size_t k = 0; k < n; k++)
                    entry_free(&entries[k]);
                free(entries);
                return NULL;
            }
            n++;
        }
    }

    /* The built-in default, last: a project's repository overrides it. A
     * manifest overriding `default` itself replaces the url, which is how a
     * fork of the tree is used without naming it everywhere. */
    const char *default_url = RETROLUNAR_PACKAGES_REPO;
    const char *default_rev = RETROLUNAR_DEFAULT_REPO_REV;
    int have_default = 0;
    if (!no_default) {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(entries[i].name, "default") == 0) {
                default_url = entries[i].url;
                default_rev = entries[i].rev;
                have_default = 1;
                break;
            }
        }
    }

    strlist_t roots;
    strlist_init(&roots);
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (have_default && strcmp(entries[i].name, "default") == 0 && i < n - 1)
            continue; /* used last, whatever position it was declared in */
        char *dir = repo_dir(&entries[i]);
        if (dir == NULL) {
            for (size_t k = 0; k < n; k++)
                entry_free(&entries[k]);
            free(entries);
            strlist_free(&roots);
            return NULL;
        }
        strlist_add(&roots, dir);
        free(dir);
        used++;
    }
    if (!no_default && !have_default) {
        entry_t d = {.name = (char *)"default", .url = (char *)default_url,
                     .rev = (char *)default_rev};
        char *dir = repo_dir(&d);
        if (dir == NULL) {
            for (size_t k = 0; k < n; k++)
                entry_free(&entries[k]);
            free(entries);
            strlist_free(&roots);
            return NULL;
        }
        strlist_add(&roots, dir);
        free(dir);
    }
    for (size_t k = 0; k < n; k++)
        entry_free(&entries[k]);
    free(entries);
    (void)used;

    char *joined = join_sp(&roots);
    strlist_free(&roots);
    char *out = xstrdup(joined);
    free(joined);
    for (char *p = out; *p != '\0'; p++)
        if (*p == ' ')
            *p = ':';
    return out;
}
