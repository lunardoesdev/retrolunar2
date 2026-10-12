/* The environment of a node, compiled from its dependency closure.
 *
 * There is no shared prefix any more, so nothing may assume a directory holds
 * every dependency. Instead each node's dependencies contribute their own
 * paths: `-I<dep>/include` and `-L<dep>/lib` for the compiler and linker,
 * `<dep>/lib/pkgconfig` for pkg-config, `<dep>` itself for cmake's
 * find_package, and — for dependencies built for the host — `bin` and `lib` on
 * PATH and LD_LIBRARY_PATH so build-time tools can actually run.
 *
 * A dependency on another system is a host tool by definition: that is what
 * `perl@native` in a recipe means, and it is why a cross build never puts a
 * target binary on PATH.
 *
 * The lines are shell text that refers to `$NESTDIR`, so the same emitter
 * serves the generated script and `retrolunar env`. */
#ifndef RETROLUNAR_ENV_H
#define RETROLUNAR_ENV_H

#include "resolve.h"
#include "util.h"

/* Append, to `out`, the shell lines that define this node's environment.
 *
 * `include_self` adds the node's own directory to the search paths. A build
 * wants it off: a package is not one of its own dependencies. A person asking
 * `env <target>` wants it on, because they are about to use that package, and
 * that is what v1's "the prefix is a normal prefix" used to give them. */
void env_lines(const graph_t *g, size_t idx, int include_self, strlist_t *out);

/* The dependency directories of a node, in dependency order, as shell
 * expressions ("$NESTDIR/pack@sys"). Used by `env` and the emitter's DEPS. */
void env_dep_dirs(const graph_t *g, size_t idx, strlist_t *out);

#endif /* RETROLUNAR_ENV_H */
