/* retrolunar.lock — what a resolution actually used.
 *
 * A manifest says what the project wants; the lock says what it got. Two things
 * matter enough to write down:
 *
 *   - the revision each repository resolved to, so the next build starts from
 *     the same tree even though the repository has moved on;
 *   - the store entry every node came out as, which is a content hash over the
 *     recipe, the system, the dependencies transitively and the probe output.
 *
 * The lock is an output, not an input the user maintains: it is written after a
 * successful build, and read for the repository revisions. A package's entry is
 * not "requested" from the lock — what to build follows from the tree — but a
 * changed entry is what tells `why different` that something upstream moved.
 */
#ifndef RETROLUNAR_LOCK_H
#define RETROLUNAR_LOCK_H

#include "manifest.h"
#include "resolve.h"

/* The file beside the manifest. */
char *lock_path(const manifest_t *m);

/* Write it: the repositories and the entries the graph came out with. 0 on
 * success (a failure is reported and is not fatal to the build). */
int lock_write(const manifest_t *m, const graph_t *g);

/* Read the revisions back into the manifest's repositories, for the ones the
 * manifest does not pin itself. 0 when there was a lock, 1 when there was none,
 * -1 when it is unreadable. */
int lock_read_revs(manifest_t *m);

#endif /* RETROLUNAR_LOCK_H */
