#define _GNU_SOURCE /* open_memstream */

#include "emit.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "env.h"
#include "fingerprint.h"
#include "system.h"

/* --- shell quoting ------------------------------------------------------- */

/* Single quotes for literals: nothing inside is special. */
static char *sq(const char *s) {
    size_t cap = strlen(s) * 4 + 3;
    char *out = xmalloc(cap);
    size_t n = 0;
    out[n++] = '\'';
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *p;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
    return out;
}

/* --- one block ----------------------------------------------------------- */

static int node_has_probes(const graph_t *g, size_t i) {
    pkg_t *p = &g->nodes[i].pkg;
    if (p->has_probe)
        return 1;
    const system_t *s = graph_system(g, p->sys);
    return s != NULL && s->has_probe;
}

static void emit_probe_hash(const graph_t *g, size_t i, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    const system_t *s = graph_system(g, p->sys);
    strlist_t probes;
    strlist_init(&probes);
    if (s != NULL)
        for (size_t k = 0; k < s->probes.n; k++)
            strlist_add(&probes, s->probes.v[k]);
    for (size_t k = 0; k < p->probes.n; k++)
        strlist_add(&probes, p->probes.v[k]);

    /* The probes run in the environment the setup just built, so a probe may
     * use anything the system computed ($NDK, $CC). Their combined output is
     * hashed, which is what makes a host-side change visible to a file-based
     * freshness check. A failing probe is a change, not a build failure. */
    fputs("    _probes_now=\"$( {", out);
    for (size_t k = 0; k < probes.n; k++) {
        char *text = system_expand(s, probes.v[k], "probe");
        if (text == NULL)
            continue;
        fprintf(out, "\n        (%s) 2>&1 || true", text);
        free(text);
    }
    fputs("\n    } | sha256sum | cut -d' ' -f1 )\"\n", out);
    strlist_free(&probes);
}

static void emit_prelude(const graph_t *g, size_t i, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    char *outpkg = xmalloc(strlen(p->name) + strlen(p->sys) + 2);
    sprintf(outpkg, "%s@%s", p->name, p->sys);
    fprintf(out,
            "    WORK=\"$(mktemp -d \"$NESTDIR/tmp/work-XXXXXX\")\"\n"
            "    STAGE=\"$(mktemp -d \"$NESTDIR/tmp/stage-XXXXXX\")\"\n"
            "    trap 'rm -rf \"$WORK\" \"$STAGE\"' EXIT\n"
            "    cd \"$WORK\"\n"
            "    OUTPKG=\"$NESTDIR/%s\"\n"
            "    DESTDIR=\"$STAGE\"\n"
            "    PREFIX=\"$DESTDIR$OUTPKG\"\n"
            "    RECIPEDIR=\"$PACKAGEDIR/%s\"\n"
            "    export OUTPKG DESTDIR PREFIX RECIPEDIR\n",
            outpkg, p->name);
    if (strcmp(p->sys, "source") != 0) {
        char *sysdir = xmalloc(strlen(p->sys) + 8);
        sprintf(sysdir, "system@%s", p->sys);
        fprintf(out, "    SYSDIR=\"$PACKAGEDIR/%s\"\n    export SYSDIR\n", sysdir);
        free(sysdir);
    }
    free(outpkg);
}

/* Recipe fields become shell variables, v1's rule: `version` and any other
 * scalar the file carries. */
static void emit_recipe_vars(pkg_t *p, FILE *out) {
    if (p->version != NULL) {
        char *v = sq(p->version);
        fprintf(out, "    version=%s\n", v);
        free(v);
    }
    for (size_t i = 0; i < p->vars.n; i++) {
        char *v = sq(p->vars.v[i].value);
        fprintf(out, "    %s=%s\n", p->vars.v[i].name, v);
        free(v);
    }
}

static void emit_state_path(const graph_t *g, size_t i, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    fprintf(out, "  _state=\"$NESTDIR/.retrolunar/state/%s/%s.toml\"\n", p->sys, p->name);
}

static void emit_read_state(FILE *out, const char *field, const char *var) {
    fprintf(out,
            "  %s=\"$(sed -n 's/^%s = \"\\(.*\\)\"$/\\1/p' \"$_state\" 2>/dev/null || true)\"\n",
            var, field);
}

static void emit_publish(const graph_t *g, size_t i, const char *fp, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    const char *ind = "    ";
    fprintf(out,
            "    # Publish only now, and only what the build actually staged: a failed\n"
            "    # body left the previous directory in place.\n"
            "    mkdir -p \"$(dirname \"$OUTPKG\")\"\n"
            "    rm -rf \"$OUTPKG\"\n"
            "    if [ -d \"$DESTDIR$OUTPKG\" ]; then\n"
            "      mv \"$DESTDIR$OUTPKG\" \"$OUTPKG\"\n"
            "    else\n"
            "      mkdir -p \"$OUTPKG\"\n"
            "    fi\n");
    fprintf(out, "%s# Record what this package was built from and in what environment.\n", ind);
    fprintf(out, "%smkdir -p \"$NESTDIR/.retrolunar/state/%s\"\n", ind, p->sys);
    if (node_has_probes(g, i)) {
        fprintf(out,
                "%sprintf 'fingerprint = \"%%s\"\\nprobes = \"%%s\"\\n' '%s' \"$_probes_now\" > "
                "\"$_state.tmp\"\n",
                ind, fp);
    } else {
        fprintf(out,
                "%sprintf 'fingerprint = \"%%s\"\\nprobes = \"%%s\"\\n' '%s' \"\" > "
                "\"$_state.tmp\"\n",
                ind, fp);
    }
    fprintf(out, "%smv \"$_state.tmp\" \"$_state\"\n", ind);
    fprintf(out, "%srm -rf \"$WORK\" \"$STAGE\"\n%strap - EXIT\n", ind, ind);
}

static void emit_body(pkg_t *p, FILE *out) {
    if (p->build == NULL)
        return;
    fputs(p->build, out);
    size_t n = strlen(p->build);
    if (n > 0 && p->build[n - 1] != '\n')
        fputc('\n', out);
}

static void emit_block(const graph_t *g, size_t i, char **fps, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    const system_t *sys = graph_system(g, p->sys);
    int probes = node_has_probes(g, i);

    fprintf(out, "\n# --- %s@%s (%zu/%zu) ---\n", p->name, p->sys, i + 1, g->n);
    emit_state_path(g, i, out);
    emit_read_state(out, "fingerprint", "_stored");
    if (probes)
        emit_read_state(out, "probes", "_probes_stored");

    if (!probes) {
        fprintf(out, "  if [ \"$_stored\" = '%s' ]; then\n", fps[i]);
        fprintf(out, "    echo \"skip %s@%s (fresh)\"\n", p->name, p->sys);
        fputs("  else\n", out);
        emit_prelude(g, i, out);
        strlist_t lines;
        strlist_init(&lines);
        env_lines(g, i, 0, &lines);
        for (size_t k = 0; k < lines.n; k++)
            fprintf(out, "    %s\n", lines.v[k]);
        strlist_free(&lines);
        if (sys != NULL)
            system_emit(sys, out);
        emit_recipe_vars(p, out);
        emit_body(p, out);
        emit_publish(g, i, fps[i], out);
        fputs("  fi\n", out);
        return;
    }

    /* Probes: files alone cannot tell whether this is fresh, so the block sets
     * the environment up, runs them, and only then decides. */
    emit_prelude(g, i, out);
    strlist_t lines;
    strlist_init(&lines);
    env_lines(g, i, 0, &lines);
    for (size_t k = 0; k < lines.n; k++)
        fprintf(out, "    %s\n", lines.v[k]);
    strlist_free(&lines);
    if (sys != NULL)
        system_emit(sys, out);
    emit_probe_hash(g, i, out);
    fprintf(out, "  if [ \"$_stored\" = '%s' ] && [ \"$_probes_stored\" = \"$_probes_now\" ]; then\n",
            fps[i]);
    fprintf(out, "    echo \"skip %s@%s (fresh)\"\n", p->name, p->sys);
    fputs("  else\n", out);
    emit_recipe_vars(p, out);
    emit_body(p, out);
    emit_publish(g, i, fps[i], out);
    fputs("  fi\n", out);
}

/* --- the whole script ---------------------------------------------------- */

static void emit_header(const char *nestdir, const char *packages, int cores, FILE *out) {
    char *n = sq(nestdir), *p = sq(packages);
    fprintf(out,
            "#!/bin/sh\n"
            "# Generated by retrolunar %s. Edit it if you like: it is the build.\n"
            "set -eu\n"
            "\n"
            "NESTDIR=%s\n"
            "PACKAGEDIR=%s\n"
            "export NESTDIR PACKAGEDIR\n"
            "export CORES=%d\n"
            "\n"
            "mkdir -p \"$NESTDIR/tmp\" \"$NESTDIR/.retrolunar/state\"\n"
            "if ! command -v flock >/dev/null 2>&1; then\n"
            "  echo \"retrolunar: flock is required (install util-linux)\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "if ! command -v sha256sum >/dev/null 2>&1; then\n"
            "  echo \"retrolunar: sha256sum is required (install coreutils)\" >&2\n"
            "  exit 1\n"
            "fi\n"
            "# One build at a time per nest: the second one fails instead of queueing.\n"
            "exec 9>>\"$NESTDIR/.retrolunar.lock\"\n"
            "if ! flock -n 9; then\n"
            "  echo \"retrolunar: another build is using NESTDIR: $NESTDIR\" >&2\n"
            "  exit 1\n"
            "fi\n",
            RETROLUNAR_VERSION, n, p, cores);
    free(n);
    free(p);
}

static void emit_summary(const graph_t *g, FILE *out) {
    fputs("\n# Where the results landed, when someone is watching.\n"
          "if [ -t 1 ]; then\n"
          "  echo\n"
          "  echo \"retrolunar: packages under: $NESTDIR\"\n",
          out);
    for (size_t i = 0; i < g->n; i++) {
        pkg_t *p = &g->nodes[i].pkg;
        if (strcmp(p->sys, "source") == 0)
            continue;
        fprintf(out,
                "  [ -d \"$NESTDIR/%s@%s\" ] && echo \"retrolunar:   %s@%s: "
                "$NESTDIR/%s@%s\" || true\n",
                p->name, p->sys, p->name, p->sys, p->name, p->sys);
    }
    fputs("fi\n", out);
}

int emit_script(const graph_t *g, const char *nestdir, int cores, FILE *out) {
    char **fps = fingerprint_nodes(g);
    emit_header(nestdir, g->packages, cores, out);
    for (size_t i = 0; i < g->n; i++)
        emit_block(g, i, fps, out);
    emit_summary(g, out);
    for (size_t i = 0; i < g->n; i++)
        free(fps[i]);
    free(fps);
    return ferror(out) ? -1 : 0;
}

char *emit_script_string(const graph_t *g, const char *nestdir, int cores, size_t *len) {
    char *buf = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&buf, &size);
    if (f == NULL)
        return NULL;
    if (emit_script(g, nestdir, cores, f) != 0) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (len != NULL)
        *len = size;
    return buf;
}
