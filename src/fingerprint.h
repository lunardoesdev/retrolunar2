/* Content fingerprints — the whole freshness story.
 *
 * Nothing here looks at mtimes: v1 compared a stamp against a directory's
 * mtime, which misses an edit to a file inside it, and that is the defect this
 * replaces. A system's fingerprint is the names and contents of every file in
 * its directory, so touching the cmake toolchain file really does make every
 * package of that system stale.
 *
 * A package's fingerprint is its recipe files, its own system's fingerprint,
 * and its dependencies' fingerprints recursively — the agreed cascade: a
 * changed dependency rebuilds the dependent.
 */
#ifndef RETROLUNAR_FINGERPRINT_H
#define RETROLUNAR_FINGERPRINT_H

#include "resolve.h"

/* Hex sha256 of every file under `dir`, recursively, in sorted order; the
 * names go into the digest too, so renaming changes it. An unreadable or
 * missing directory yields the digest of nothing but its name, which is not an
 * error: `@source` has no system directory. */
char *fingerprint_dir(const char *dir);

/* One hex fingerprint per node, in the graph's order. Nodes are topologically
 * sorted, so a node's dependencies already have theirs. */
char **fingerprint_nodes(const graph_t *g);

/* One node's digest. `done` holds the digests already computed for earlier nodes
 * (they come in dependency order), so a dependency's value can be folded in. */
char *fingerprint_node(const graph_t *g, size_t i, char **done);

#endif /* RETROLUNAR_FINGERPRINT_H */
