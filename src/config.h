/* Compile-time configuration. The default system is the native one: a bare
 * target resolves to it, and '@native' is an alias for the same identity. */
#ifndef RETROLUNAR_CONFIG_H
#define RETROLUNAR_CONFIG_H

#ifndef RETROLUNAR_DEFAULT_SYSTEM
#define RETROLUNAR_DEFAULT_SYSTEM "clang-native"
#endif

#define RETROLUNAR_PACKAGES_REPO "https://github.com/lunardoesdev/retrolunar-packages"

/* The built-in repository follows its own default branch: the tool ships with a
 * tree, not with a copy of it, so a fresh clone is what a project gets. The
 * first build writes the commit it used into retrolunar.lock, and from then on
 * the project builds that commit — which is where reproducibility comes from,
 * rather than from the tool guessing a revision. `--no-default-repo` is how a
 * build says it does not want the built-in repository at all. */
#define RETROLUNAR_DEFAULT_REPO_REV NULL

#define RETROLUNAR_VERSION "2.0.0"

#endif /* RETROLUNAR_CONFIG_H */
