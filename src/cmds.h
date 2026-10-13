/* One function per subcommand. Each returns a process exit status. */
#ifndef RETROLUNAR_CMDS_H
#define RETROLUNAR_CMDS_H

#include "cli.h"

int cmd_search(const options_t *o);
int cmd_deps(const options_t *o);
int cmd_install(const options_t *o);
int cmd_generate(const options_t *o);
int cmd_env(const options_t *o);
int cmd_export(const options_t *o);
int cmd_shell(const options_t *o);
int cmd_build(const options_t *o);
int cmd_gc(const options_t *o);
int cmd_cache(const options_t *o);

#endif /* RETROLUNAR_CMDS_H */
