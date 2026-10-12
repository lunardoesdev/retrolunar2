/* retrolunar 2 — dispatch.
 *
 * Every command is a function in its own file; this file only parses the
 * command line, answers --help/--version, and maps a command to its
 * implementation. Commands still to be written say so plainly rather than
 * pretending to work. */
#include <stdio.h>

#include "cli.h"
#include "cmds.h"
#include "config.h"

/* Commands that arrive in the phases PLAN.md lays out (P1 resolver, P2 script
 * emitter and environment, P3 state, P5 env and export). */
static int not_yet(enum cmd c) {
    fprintf(stderr,
            "retrolunar: '%s' is not implemented yet\n"
            "  see PLAN.md — the resolver and `deps` come first\n",
            cmd_name(c));
    return 1;
}

int main(int argc, char **argv) {
    options_t o;
    if (cli_parse(argc, argv, &o) != 0) {
        cli_usage(stderr, argv[0]);
        cli_free(&o);
        return 1;
    }
    if (o.version) {
        printf("retrolunar %s\n", RETROLUNAR_VERSION);
        cli_free(&o);
        return 0;
    }
    if (o.help || o.cmd == CMD_NONE) {
        cli_usage(stdout, argv[0]);
        cli_free(&o);
        return 0;
    }

    int status;
    switch (o.cmd) {
    case CMD_SEARCH:
        status = cmd_search(&o);
        break;
    case CMD_DEPS:
        status = cmd_deps(&o);
        break;
    case CMD_INSTALL:
        status = cmd_install(&o);
        break;
    case CMD_GENERATE:
        status = cmd_generate(&o);
        break;
    case CMD_ENV:
        status = cmd_env(&o);
        break;
    case CMD_EXPORT:
        status = cmd_export(&o);
        break;
    default:
        status = not_yet(o.cmd);
        break;
    }
    cli_free(&o);
    return status;
}
