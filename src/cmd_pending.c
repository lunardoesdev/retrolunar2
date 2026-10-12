/* Commands whose implementation lands in the phases PLAN.md lays out.
 *
 * They live here so the binary never pretends a command works: each one says
 * which phase brings it. `deps` is P1 (resolver), install/generate are P2
 * (environment compiler and emitter), `env` and `export` are P5. */
#include <stdio.h>

#include "cmds.h"

static int pending(const char *name, const char *phase) {
    fprintf(stderr, "retrolunar: '%s' is not implemented yet (%s)\n", name, phase);
    return 1;
}

int cmd_deps(const options_t *o) {
    (void)o;
    return pending("deps", "P1: model and resolver — see PLAN.md");
}

int cmd_install(const options_t *o) {
    (void)o;
    return pending("install", "P2: environment compiler and script emitter");
}

int cmd_generate(const options_t *o) {
    (void)o;
    return pending("generate", "P2: environment compiler and script emitter");
}

int cmd_env(const options_t *o) {
    (void)o;
    return pending("env", "P5: environment output from the same closure code");
}

int cmd_export(const options_t *o) {
    (void)o;
    return pending("export", "P5: runtime dependency collection");
}
