/* A project manifest: `retrolunar.toml` in a directory of its own.
 *
 * It is the recipe format plus a few tables, so a project does not learn a
 * second language to describe itself. The recipe half is read by the recipe
 * reader — the file is the package whose name is its directory's name — and the
 * extra tables are read here.
 *
 *   deps = ["libpng@clang-native"]   # the package's, and the shell's
 *   buildsystem = "cmake"
 *   args = ["-DPNG_SHARED=ON"]
 *
 *   [shell]
 *   deps = ["gdb@clang-native"]      # the shell's alone: a debugger is not a
 *                                    # build dependency
 */
#ifndef RETROLUNAR_MANIFEST_H
#define RETROLUNAR_MANIFEST_H

#include "util.h"

/* A repository the project resolves against. `rev` is a revision to check out
 * (a tag, a commit): with it, the same manifest builds the same tree. */
typedef struct {
    char *name;
    char *url;
    char *rev;
} repo_entry_t;

typedef struct {
    repo_entry_t *v;
    size_t n;
    size_t cap;
} repolist_t;

typedef struct {
    char *dir;        /* the directory holding retrolunar.toml */
    char *file;       /* .../retrolunar.toml */
    char *name;       /* the package's name: the directory's */
    strlist_t shell_deps; /* [shell] deps, empty when there is no such table */
    repolist_t repos;     /* [repo.*], in the order the file declares them */
    char *cache_url;      /* [cache] url: where built packages are kept */
} manifest_t;

/* Look for a manifest in `dir` (NULL means the working directory).
 * 0 = found, 1 = there is none here, -1 = the file is broken (reported). */
int manifest_find(const char *dir, manifest_t *out);
void manifest_free(manifest_t *m);

#endif /* RETROLUNAR_MANIFEST_H */
