/* Command line: one parser for every command, not v1's three copies plus a
 * pre-scan whose flag lists had to be kept in sync by hand. */
#ifndef RETROLUNAR_CLI_H
#define RETROLUNAR_CLI_H

#include <stdio.h>

#include "util.h"

enum cmd {
    CMD_NONE = 0,
    CMD_INSTALL,
    CMD_GENERATE,
    CMD_DEPS,
    CMD_SEARCH,
    CMD_ENV,
    CMD_EXPORT
};

typedef struct {
    enum cmd cmd;
    const char *packages; /* --packages DIR */
    const char *nest;     /* --nest DIR */
    const char *output;   /* -o FILE, --output FILE */
    const char *to;       /* --to DIR, export only */
    int cores;            /* --cores N, defaults to 1 */
    int execute;          /* -x */
    int strict;           /* --strict, export only */
    int rpath;            /* 0 when --no-rpath was given */
    int help;             /* -h/--help anywhere */
    int version;          /* --version */
    strlist_t args;       /* positionals after the command */
} options_t;

/* 0 on success, 1 when usage should be printed and the run stopped, -1 on a
 * real parse error (message already written to stderr). */
int cli_parse(int argc, char **argv, options_t *o);
void cli_free(options_t *o);
void cli_usage(FILE *out, const char *prog);

const char *cmd_name(enum cmd c);

#endif /* RETROLUNAR_CLI_H */
