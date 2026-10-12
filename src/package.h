/* One package resolved for one system.
 *
 * The build file is chosen, never merged: `<sys>.toml`, then the system's
 * `fallbacks` in order, then `generic.toml` — the rule v1 applied to
 * `generic.lua` / `android.lua`. `@source` is the fetch pseudo-system: it has
 * no toolchain, and its recipe is `source.toml`. */
#ifndef RETROLUNAR_PACKAGE_H
#define RETROLUNAR_PACKAGE_H

#include "system.h"
#include "util.h"

typedef struct {
    char *name;  /* zlib */
    char *sys;   /* mingw64, or "source" */
    char *dir;   /* <packages>/zlib */
    char *file;  /* the build file that won, absolute; NULL for @source */
    char *version;
    strlist_t deps;    /* raw edges, as written */
    char *build;       /* shell body, NULL when the file defines none */
    strlist_t probes;  /* package-level probes */
    int has_probe;
    pairlist_t vars;   /* extra scalar fields, emitted as shell variables */
} pkg_t;

/* `system` is the system the package is being built for; it may be NULL for
 * the `source` pseudo-system. */
int package_load(const char *packages, const char *name, const char *sys,
                 const system_t *system, pkg_t *out);
void package_free(pkg_t *p);

/* Which build file wins, as an absolute path, or NULL when the package has
 * none for this system. Reported by the caller with its own message. */
char *package_build_file(const char *packages, const char *name, const char *sys,
                         const system_t *system);

#endif /* RETROLUNAR_PACKAGE_H */
