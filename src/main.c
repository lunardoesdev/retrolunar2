/* retrolunar 2 — dispatch.
 *
 * Every command is a function in its own file; this file only parses the
 * command line, answers --help/--version, and maps a command to its
 * implementation. Commands still to be written say so plainly rather than
 * pretending to work. */
#include <stdio.h>

#include <stdlib.h>

#include "cli.h"
#include "cmds.h"
#include "config.h"
#include "lock.h"
#include "manifest.h"
#include "repos.h"

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

    /* Where packages come from. `--packages DIR` (or a list, separated by
     * colons) is what the caller said, and wins. Without it, a manifest's
     * repositories are searched, then the built-in `default` — so a project
     * resolves against what it declared, and a plain run stays offline. */
    char *repo_path = NULL;
    if (o.packages == NULL) {
        manifest_t m;
        int found = manifest_find(NULL, &m);
        if (found == 0) {
            if (lock_read_revs(&m) < 0) {
                manifest_free(&m);
                cli_free(&o);
                return 1;
            }
            repo_path = repos_search_path(&m, &o.repos, o.no_default_repo);
            manifest_free(&m);
        } else if (found < 0) {
            cli_free(&o);
            return 1;
        } else if (o.repos.n > 0 || o.no_default_repo) {
            /* No manifest, but the command line named repositories: they are
             * the tree, and the built-in one joins them unless it was refused. */
            repo_path = repos_search_path(NULL, &o.repos, o.no_default_repo);
        }
        if (repo_path == NULL && o.repos.n > 0) {
            cli_free(&o);
            return 1;
        }
        if (repo_path != NULL)
            o.packages = repo_path;
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
    case CMD_GC:
        status = cmd_gc(&o);
        break;
    case CMD_CACHE:
        status = cmd_cache(&o);
        break;
    case CMD_BUILD:
        status = cmd_build(&o);
        break;
    case CMD_SHELL:
    case CMD_RUN:
        status = cmd_shell(&o);
        break;
    case CMD_EXPORT:
        status = cmd_export(&o);
        break;
    default:
        status = not_yet(o.cmd);
        break;
    }
    cli_free(&o);
    free(repo_path);
    return status;
}
