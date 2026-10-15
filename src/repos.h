/* Repositories: the trees a project resolves against, in order.
 *
 * A project's `retrolunar.toml` may name repositories, and the command line may
 * add one for a single run. They are searched in the order they are declared,
 * with the built-in `default` last, so a project's own repository overrides the
 * one the tool ships with — and `--no-default-repo` drops the built-in one
 * entirely, which is how a build shows it does not quietly depend on it.
 *
 * The result is a colon-separated path, the same shape the rest of the tool
 * already searches (see tree_root_with).
 */
#ifndef RETROLUNAR_REPOS_H
#define RETROLUNAR_REPOS_H

#include "manifest.h"
#include "util.h"

/* The search path, malloc'd: the manifest's repositories, then the ones from
 * the command line, then the built-in default unless `no_default`. Remote
 * repositories are cloned or updated under $HOME/.cache/retrolunar/repos.
 * NULL when nothing could be used (the reason is on stderr). */
char *repos_search_path(const manifest_t *m, const strlist_t *cli, int no_default);

/* `git clone --quiet URL DIR`, 0 on success. Exposed for the built-in packages
 * tree, which keeps its own directory rather than a repository entry. */
int repo_clone(const char *url, const char *dir);

/* Update a checkout in place: fetch, then a fast-forward pull that fails rather
 * than merges, so local work is never rewritten. 0 on success; a directory that
 * is not a checkout is reported and returns -1. */
int repo_refresh(const char *dir);

#endif /* RETROLUNAR_REPOS_H */
