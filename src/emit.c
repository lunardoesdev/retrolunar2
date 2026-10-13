#define _GNU_SOURCE /* open_memstream */

#include "emit.h"

#include <ctype.h>

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "env.h"
#include "fingerprint.h"
#include "system.h"

/* The run's output override, when there is one: see emit_script_string_out. */
static const char *g_out_key = NULL;
static const char *g_out_dir = NULL;

/* A shell-quoted value, for the few places that build a line in C. */
static char *quote(const char *value) {
    size_t n = strlen(value) + 3;
    char *out = xmalloc(n);
    snprintf(out, n, "\"%s\"", value);
    return out;
}

/* Where a node's package directory is, as the shell sees it: the nest, or the
 * directory `build --out DIR` named for the one node it was asked about. */
static char *out_quoted(const pkg_t *p) {
    char key[512];
    snprintf(key, sizeof key, "%s@%s", p->name, p->sys);
    if (g_out_key != NULL && strcmp(key, g_out_key) == 0 && g_out_dir != NULL)
        return quote(g_out_dir);
    char *joined = str_concat("$NESTDIR/", key);
    char *out = quote(joined);
    free(joined);
    return out;
}

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
    char *out_line = out_quoted(p);
    fprintf(out,
            "    WORK=\"$(mktemp -d \"$NESTDIR/tmp/work-XXXXXX\")\"\n"
            "    STAGE=\"$(mktemp -d \"$NESTDIR/tmp/stage-XXXXXX\")\"\n"
            "    trap 'rm -rf \"$WORK\" \"$STAGE\"' EXIT\n"
            "    cd \"$WORK\"\n"
            "    OUTPKG=%s\n"
            "    DESTDIR=\"$STAGE\"\n"
            "    PREFIX=\"$DESTDIR$OUTPKG\"\n"
            "    RECIPEDIR=\"$PACKAGEDIR/%s\"\n"
            "    export OUTPKG PREFIX RECIPEDIR\n",
            out_line, p->name);
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
    /* A build system's recipe gives its fields to the driver as $BS_*, so the
     * plain names would be noise. A shell body is v1 code and still wants
     * `$version` and friends as they were. */
    if (p->buildsystem != NULL)
        return;
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
    /* Every name the recipe defines is exported, so a step it runs sees them. */
    if (p->version != NULL || p->vars.n > 0) {
        fputs("    export version", out);
        for (size_t i = 0; i < p->vars.n; i++)
            fprintf(out, " %s", p->vars.v[i].name);
        fputc('\n', out);
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
            "    # Publish the staged tree, which is the only thing the steps wrote:\n"
            "    # a package directory is replaced, never written into, so a build\n"
            "    # that failed halfway leaves the previous package in place.\n"
            "    mkdir -p \"$(dirname \"$OUTPKG\")\"\n"
            "    rm -rf \"$OUTPKG\"\n"
            "    if [ -d \"$PREFIX\" ]; then\n"
            "      mv \"$PREFIX\" \"$OUTPKG\"\n"
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

static void emit_text(const char *text, FILE *out) {
    if (text == NULL)
        return;
    fputs(text, out);
    size_t n = strlen(text);
    if (n > 0 && text[n - 1] != '\n')
        fputc('\n', out);
}

static void emit_body(pkg_t *p, FILE *out) {
    emit_text(p->build, out);
}

/* A recipe that names a build system is driven by that system's `script`, which
 * lives in the system's namespace as `buildsystem.<name>.script`, in the system's
 * own files. The recipe's
 * fields become $BS_* variables, so the script knows what to build without this
 * program knowing what cmake, meson, make or cargo are. */
/* The source trees a node builds from: every @source dependency, copied into
 * the work directory. v1's recipes each wrote this loop by hand; every one of
 * them said the same thing, so the tool says it once and a recipe that needs to
 * do something else sets `sources = false`. */
static void emit_sources(const graph_t *g, size_t i, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    if (!p->sources)
        return;
    strlist_t sources;
    strlist_init(&sources);
    for (size_t k = 0; k < p->deps.n; k++) {
        const char *at = strrchr(p->deps.v[k], '@');
        if (at == NULL || strcmp(at, "@source") != 0)
            continue;
        char *nm = xstrndup(p->deps.v[k], (size_t)(at - p->deps.v[k]));
        strlist_add_own(&sources, str_concat(str_concat("$NESTDIR/", nm), "@source"));
        free(nm);
    }
    if (sources.n == 0) {
        strlist_free(&sources);
        return;
    }
    char *srcs = join_sp(&sources);
    fprintf(out, "    BS_sources=\"%s\"\n", srcs);
    free(srcs);
    strlist_free(&sources);
    fputs("    for _src in $BS_sources; do cp -rf \"$_src\"/. . ; done\n", out);
}

static void emit_bs_vars(const graph_t *g, size_t i, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    for (size_t k = 0; k < p->vars.n; k++) {
        const char *val = p->vars.v[k].value;
        fputs("    BS_", out);
        fputs(p->vars.v[k].name, out);
        fputs("=\"", out);
        for (const char *q = val; *q != '\0'; q++) {
            if (*q == '"' || *q == '\\')
                fputc('\\', out);
            fputc(*q, out);
        }
        fputs("\"\n", out);
    }
}

/* Every $BS_ name the build system's own script uses, so that one a recipe does
 * not give is an empty variable instead of an unbound one under `set -u`. The
 * names come from the script's text: this program still knows nothing about
 * what any particular build system needs. */
static int bs_name_used(const pkg_t *p, const char *name, size_t n) {
    for (size_t k = 0; k < p->vars.n; k++)
        if (strlen(p->vars.v[k].name) == n && strncmp(p->vars.v[k].name, name, n) == 0)
            return 1;
    return 0;
}

static void emit_bs_defaults(const pkg_t *p, const char *text, strlist_t *seen, FILE *out) {
    for (const char *q = text; *q != '\0'; q++) {
        if (q[0] != '$' || q[1] != 'B' || q[2] != 'S' || q[3] != '_')
            continue;
        size_t n = 4;
        while (q[n] != '\0' && (isalnum((unsigned char)q[n]) || q[n] == '_'))
            n++;
        if (n == 4)
            continue;
        char name[256];
        if (n >= sizeof name)
            continue;
        memcpy(name, q + 4, n - 4);
        name[n - 4] = '\0';
        if (bs_name_used(p, name, n - 4) || strlist_has(seen, name))
            continue;
        strlist_add(seen, name);
        fprintf(out, "    BS_%s=\"${BS_%s:-}\"\n", name, name);
    }
}

static void emit_build(const graph_t *g, size_t i, const system_t *sys, FILE *out) {
    pkg_t *p = &g->nodes[i].pkg;
    if (p->buildsystem == NULL || sys == NULL) {
        emit_text(p->pre, out);
        emit_body(p, out);
        emit_text(p->post, out);
        return;
    }
    static const char *const parts[] = {"pre", "script", "post"};
    char *keys[3] = {NULL, NULL, NULL};
    char *texts[3] = {NULL, NULL, NULL};
    strlist_t seen;
    strlist_init(&seen);
    fprintf(out, "    BS_NAME=\"%s\"\n", p->buildsystem);
    for (size_t k = 0; k < 3; k++) {
        char key[512];
        snprintf(key, sizeof key, "buildsystem.%s.%s", p->buildsystem, parts[k]);
        keys[k] = xstrdup(key);
        const char *text = system_sym(sys, key);
        if (text == NULL)
            continue;
        texts[k] = system_expand(sys, text, key);
        if (texts[k] != NULL)
            emit_bs_defaults(p, texts[k], &seen, out);
    }
    strlist_free(&seen);
    emit_bs_vars(g, i, out);
    /* The recipe's own shell: what it needs before the standard steps, and what
     * it does after them. */
    emit_text(p->pre, out);
    for (size_t k = 0; k < 3; k++) {
        if (texts[k] == NULL) {
            free(keys[k]);
            continue;
        }
        fputs(texts[k], out);
        size_t n = strlen(texts[k]);
        if (n > 0 && texts[k][n - 1] != '\n')
            fputc('\n', out);
        free(texts[k]);
        free(keys[k]);
    }
    /* A recipe may still add shell of its own; it runs after the standard steps. */
    emit_text(p->post, out);
    emit_body(p, out);
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
        char *outpkg_here = out_quoted(p);
        fprintf(out, "  if [ \"$_stored\" = '%s' ] && [ -d %s ]; then\n", fps[i], outpkg_here);
        free(outpkg_here);
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
        emit_sources(g, i, out);
        emit_build(g, i, sys, out);
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
    char *outpkg_here = out_quoted(p);
    fprintf(out, "  if [ \"$_stored\" = '%s' ] && [ \"$_probes_stored\" = \"$_probes_now\" ] && [ -d %s ]; then\n",
            fps[i], outpkg_here);
    free(outpkg_here);
    fprintf(out, "    echo \"skip %s@%s (fresh)\"\n", p->name, p->sys);
    fputs("  else\n", out);
    emit_recipe_vars(p, out);
    emit_sources(g, i, out);
    emit_build(g, i, sys, out);
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

/* Every path whose change makes a node stale: the recipe files that take part,
 * the files of its system (and the directory itself, so adding one counts), and
 * the state files of its dependencies — a rebuild rewrites its state file, which
 * is what carries the cascade. */
static void collect_dir_files(const char *dir, strlist_t *out) {
    strlist_add(out, dir);
    strlist_t names;
    strlist_init(&names);
    list_dir(dir, &names);
    strlist_sort(&names);
    for (size_t i = 0; i < names.n; i++) {
        char *p = path_join(dir, names.v[i]);
        if (is_dir(p))
            collect_dir_files(p, out);
        else
            strlist_add(out, p);
        free(p);
    }
    strlist_free(&names);
}

static void collect_inputs(const graph_t *g, size_t i, strlist_t *out, strlist_t *states) {
    pkg_t *p = &g->nodes[i].pkg;
    if (p->file != NULL)
        strlist_add(out, p->file);
    char *src = path_join(p->dir, "source.toml");
    if (strcmp(p->sys, "source") != 0 && is_file(src))
        strlist_add(out, src);
    free(src);
    if (strcmp(p->sys, "source") != 0) {
        char *sysdir = str_concat("system@", p->sys);
        char *full = path_join(g->packages, sysdir);
        if (is_dir(full))
            collect_dir_files(full, out);
        free(full);
        free(sysdir);
    }
    for (size_t k = 0; k < p->deps.n; k++) {
        const char *at = strrchr(p->deps.v[k], '@');
        char key[512];
        if (at != NULL)
            snprintf(key, sizeof key, "%s", p->deps.v[k]);
        else
            snprintf(key, sizeof key, "%s@%s", p->deps.v[k], p->sys);
        strlist_add(states, key);
    }
}

/* The stored fingerprint, when nothing that feeds this node is newer than the
 * file it is stored in. Timestamps are the cheap test and the file's own mtime is
 * the stamp — the read of every input only happens when something moved. A
 * restored timestamp slips past this, which is what RETROLUNAR_VERIFY=1 is for. */
/* A whole second of slack: when an input was written within the same second as
 * the state file, the order of the two is not knowable from timestamps alone, and
 * the hashes decide. Every other case is free. */
#define MTIME_SLACK_NS 1000000000LL

static int node_index_of(const graph_t *g, const char *key) {
    for (size_t i = 0; i < g->n; i++)
        if (strcmp(g->nodes[i].key, key) == 0)
            return (int)i;
    return -1;
}

/* A dependency key as the graph spells it: a bare name inherits the system. */
static void dep_key(const pkg_t *p, const char *dep, char *out, size_t outsz) {
    const char *at = strrchr(dep, '@');
    if (at != NULL)
        snprintf(out, outsz, "%s", dep);
    else
        snprintf(out, outsz, "%s@%s", dep, p->sys);
}

/* True when every dependency is itself unchanged: a node whose dependency is
 * about to be rebuilt has to rebuild too, or the cascade stops at this node. */
static int deps_unchanged(const graph_t *g, size_t i, const unsigned char *quick) {
    const pkg_t *p = &g->nodes[i].pkg;
    for (size_t k = 0; k < p->deps.n; k++) {
        char key[512];
        dep_key(p, p->deps.v[k], key, sizeof key);
        int di = node_index_of(g, key);
        if (di >= 0 && !quick[di])
            return 0;
    }
    return 1;
}

static char *stored_when_unchanged(const graph_t *g, size_t i, const char *nestdir) {
    pkg_t *p = &g->nodes[i].pkg;
    char *state = xmalloc(strlen(nestdir) + strlen(p->sys) + strlen(p->name) + 64);
    sprintf(state, "%s/.retrolunar/state/%s/%s.toml", nestdir, p->sys, p->name);
    char *unchanged = NULL;
    if (is_file(state)) {
        if (mtime_ns(state) != 0) {
            strlist_t ins, states;
            strlist_init(&ins);
            strlist_init(&states);
            collect_inputs(g, i, &ins, &states);
            unchanged = xstrdup(state);
            long long stamp = mtime_ns(state);

            for (size_t k = 0; k < ins.n && unchanged != NULL; k++) {
                long long m = mtime_ns(ins.v[k]);
                if (m > stamp || (m != 0 && stamp - m < MTIME_SLACK_NS))
                    unchanged = NULL;
            }
            for (size_t k = 0; k < states.n && unchanged != NULL; k++) {
                char *dep = xmalloc(strlen(nestdir) + strlen(states.v[k]) + 64);
                const char *at = strrchr(states.v[k], '@');
                char pack[512], sys[512];
                snprintf(pack, sizeof pack, "%.*s", (int)(at - states.v[k]), states.v[k]);
                snprintf(sys, sizeof sys, "%s", at + 1);
                sprintf(dep, "%s/.retrolunar/state/%s/%s.toml", nestdir, sys, pack);
                long long dm = mtime_ns(dep);
                if (dm > stamp || (dm != 0 && stamp - dm < MTIME_SLACK_NS))
                    unchanged = NULL;
                free(dep);
            }
            strlist_free(&ins);
            strlist_free(&states);
        }
    }
    if (unchanged == NULL) {
        free(state);
        return NULL;
    }
    /* The stored value is the current one, so read it instead of hashing. */
    size_t len = 0;
    char *text = read_file(state, &len);
    free(state);
    if (text == NULL)
        return NULL;
    char *line = strstr(text, "fingerprint = \"");
    char *out = NULL;
    if (line != NULL) {
        line += strlen("fingerprint = \"");
        char *end = strchr(line, '"');
        if (end != NULL)
            out = xstrndup(line, (size_t)(end - line));
    }
    free(text);
    return out;
}

int emit_script(const graph_t *g, const char *nestdir, int cores, FILE *out) {
    for (size_t i = 0; i < g->n; i++) {
        pkg_t *p = &g->nodes[i].pkg;
        if (p->buildsystem == NULL)
            continue;
        const system_t *sys = graph_system(g, p->sys);
        char key[512];
        snprintf(key, sizeof key, "buildsystem.%s.script", p->buildsystem);
        if (sys != NULL && system_sym(sys, key) != NULL)
            continue;
        fprintf(stderr,
                "retrolunar: %s@%s: system '%s' defines no build system '%s'\n"
                "  a build system belongs to the system: put a "
                "`buildsystem.%s.script` key in %s/system@%s/%s.toml\n",
                p->name, p->sys, p->sys, p->buildsystem, p->buildsystem,
                g->packages, p->sys, p->buildsystem);
        return -1;
    }
    int verify = getenv("RETROLUNAR_VERIFY") != NULL;
    char **fps = xmalloc((g->n ? g->n : 1) * sizeof *fps);
    unsigned char *quick = xmalloc(g->n ? g->n : 1);
    for (size_t i = 0; i < g->n; i++) {
        fps[i] = NULL;
        quick[i] = 0;
    }
    for (size_t i = 0; i < g->n; i++) {
        if (!verify && deps_unchanged(g, i, quick))
            fps[i] = stored_when_unchanged(g, i, nestdir);
        if (fps[i] != NULL) {
            quick[i] = 1;
            continue;
        }
        /* Nodes come out in dependency order, so a dependency's fingerprint is
         * already known - from its stored value when it did not move. */
        fps[i] = fingerprint_node(g, i, fps);
    }
    free(quick);
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

/* See emit.h: one node publishes elsewhere, everything else stays in the nest. */
char *emit_script_string_out(const graph_t *g, const char *nestdir, int cores,
                             const char *out_key, const char *out_dir, size_t *len) {
    g_out_key = out_key;
    g_out_dir = out_dir;
    char *script = emit_script_string(g, nestdir, cores, len);
    g_out_key = NULL;
    g_out_dir = NULL;
    return script;
}
