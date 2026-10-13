#include "cli.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"

static const struct {
    const char *name;
    enum cmd cmd;
} COMMANDS[] = {
    {"install", CMD_INSTALL}, {"generate", CMD_GENERATE}, {"deps", CMD_DEPS},
    {"search", CMD_SEARCH},   {"env", CMD_ENV},           {"export", CMD_EXPORT},
    {"shell", CMD_SHELL},     {"run", CMD_RUN},
};

const char *cmd_name(enum cmd c) {
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++)
        if (COMMANDS[i].cmd == c)
            return COMMANDS[i].name;
    return "?";
}

/* A flag that needs the next argument, else the message names it. Returns the
 * value, or NULL when the argument is missing (already reported). */
static const char *value_of(int argc, char **argv, int *i, const char *flag) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "retrolunar: %s wants a value\n", flag);
        return NULL;
    }
    return argv[++*i];
}

static int int_of(const char *s, int *out, const char *flag) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 1024) {
        fprintf(stderr, "retrolunar: %s wants a number from 1 to 1024\n", flag);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int cli_parse(int argc, char **argv, options_t *o) {
    memset(o, 0, sizeof *o);
    o->cores = 1;
    o->rpath = 1;
    strlist_init(&o->args);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            o->help = 1;
            continue;
        }
        if (strcmp(a, "--version") == 0) {
            o->version = 1;
            continue;
        }
        if (strcmp(a, "--packages") == 0) {
            o->packages = value_of(argc, argv, &i, a);
            if (o->packages == NULL)
                return -1;
            continue;
        }
        if (strcmp(a, "--nest") == 0) {
            o->nest = value_of(argc, argv, &i, a);
            if (o->nest == NULL)
                return -1;
            continue;
        }
        if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
            o->output = value_of(argc, argv, &i, a);
            if (o->output == NULL)
                return -1;
            continue;
        }
        if (strcmp(a, "--to") == 0) {
            o->to = value_of(argc, argv, &i, a);
            if (o->to == NULL)
                return -1;
            continue;
        }
        if (strcmp(a, "--cores") == 0) {
            const char *v = value_of(argc, argv, &i, a);
            if (v == NULL)
                return -1;
            if (int_of(v, &o->cores, a) != 0)
                return -1;
            continue;
        }
        if (strcmp(a, "-x") == 0 || strcmp(a, "--execute") == 0) {
            o->execute = 1;
            continue;
        }
        if (strcmp(a, "--strict") == 0) { /* the only behaviour there is */
            o->strict = 1;
            continue;
        }
        if (strcmp(a, "--no-rpath") == 0) {
            o->rpath = 0;
            continue;
        }
        /* Everything after `--` is an argument, never an option: that is how a
         * command carried by `run` keeps its own flags, and how extra build
         * arguments reach a driver. */
        if (strcmp(a, "--") == 0) {
            ++i;
            for (; i < argc; i++)
                strlist_add(&o->args, argv[i]);
            break;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "retrolunar: unknown option '%s'\n", a);
            return -1;
        }
        if (o->cmd == CMD_NONE) {
            for (size_t c = 0; c < sizeof COMMANDS / sizeof COMMANDS[0]; c++) {
                if (strcmp(a, COMMANDS[c].name) == 0) {
                    o->cmd = COMMANDS[c].cmd;
                    break;
                }
            }
            if (o->cmd == CMD_NONE) {
                fprintf(stderr, "retrolunar: unknown command '%s'\n", a);
                return -1;
            }
            continue;
        }
        strlist_add(&o->args, a);
    }
    return 0;
}

void cli_free(options_t *o) { strlist_free(&o->args); }

void cli_usage(FILE *out, const char *prog) {
    fprintf(out,
            "usage: %s <command> [options] <pack[@sys]>...\n"
            "       %s --help\n"
            "\n"
            "Commands:\n"
            "  install   print a POSIX sh script that builds the targets and their\n"
            "            dependencies, each into its own directory under --nest.\n"
            "  generate  the same script, with somewhere to put it: -o FILE writes\n"
            "            it there and makes it executable; -x runs it with sh after\n"
            "            generating and exits with the script's own status.\n"
            "  deps      print the resolved graph, dependencies first, then exit.\n"
            "  search    list packages and systems in the tree whose name contains\n"
            "            QUERY, case-insensitively. An empty QUERY lists everything.\n"
            "  env       print the closure's build environment as export lines:\n"
            "            eval \"$(%s env libpng@native)\"\n"
            "  export    collect a target's binaries and shared libraries plus what\n"
            "            they load, flat, into --to DIR, ready to hand over.\n"
            "\n"
            "Options:\n"
            "  --packages DIR   packages tree to resolve recipes from; defaults to\n"
            "                   $HOME/.cache/retrolunar/packages, cloned from\n"
            "                   %s on first use\n"
            "  --nest DIR       output root; defaults to\n"
            "                   $HOME/.cache/retrolunar/nestdir\n"
            "  --cores N        job count for recipes that honour $CORES (default 1)\n"
            "  -o, --output F   write the script to F (generate)\n"
            "  -x, --execute    run the generated script (generate)\n"
            "  --to DIR         where export writes its bundle\n"
            "  --strict         export: treat an unresolved library as an error\n"
            "  --no-rpath       export: do not bake an $ORIGIN rpath into ELF files\n"
            "  -h, --help       show this help and exit\n"
            "\n"
            "A target is 'pack' or 'pack@sys'. A bare name uses the compile-time\n"
            "default system (%s); '@native' is an alias for that same system.\n",
            prog, prog, prog, RETROLUNAR_PACKAGES_REPO, RETROLUNAR_DEFAULT_SYSTEM);
}
