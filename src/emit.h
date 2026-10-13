/* The script emitter: the graph in, an auditable POSIX sh script out.
 *
 * Every node gets one block. The block first compares its recorded
 * fingerprint with the one computed when this script was generated — content
 * hashes, never mtimes — and skips when they agree. A node whose system (or
 * whose own recipe) declares probes cannot be judged from files alone, so it
 * runs the system setup, runs the probes, and compares their hash too.
 *
 * Inside the block, the order is: scratch and staging directories, the
 * closure's environment, the system's own fragments, the recipe's variables,
 * the build body verbatim, and only then publishing. A failed body leaves the
 * previous package directory untouched and the staging tree is thrown away.
 */
#ifndef RETROLUNAR_EMIT_H
#define RETROLUNAR_EMIT_H

#include <stdio.h>

#include "resolve.h"

int emit_script(const graph_t *g, const char *nestdir, int cores, FILE *out);

/* The same script as a string, for the caller that wants to write it or hand
 * it to sh. Caller frees. */
char *emit_script_string(const graph_t *g, const char *nestdir, int cores, size_t *len);

/* The same, but the node named by `out_key` publishes into `out_dir` instead of
 * into the nest: that is `build --out DIR`, where the package the project
 * describes lands where the project asked, and its dependencies stay where every
 * other build put them. */
char *emit_script_string_out(const graph_t *g, const char *nestdir, int cores,
                             const char *out_key, const char *out_dir, size_t *len);

#endif /* RETROLUNAR_EMIT_H */
