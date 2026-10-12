/* The one command left for a later phase: export (P5). It says so rather than
 * pretending to work. */
#include <stdio.h>

#include "cmds.h"

int cmd_export(const options_t *o) {
    (void)o;
    fputs("retrolunar: 'export' is not implemented yet "
          "(P5: runtime dependency collection)\n",
          stderr);
    return 1;
}
