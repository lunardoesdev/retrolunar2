/* search [--packages DIR] QUERY — what is in the tree.
 * Reads directories and file names only: no recipe is parsed and nothing is
 * built, exactly like v1, but without shelling out to `ls`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmds.h"
#include "tree.h"

int cmd_search(const options_t *o) {
    if (o->args.n != 1) {
        fputs("retrolunar: search wants exactly one QUERY "
              "(use '' to list everything)\n",
              stderr);
        return 1;
    }
    char *packages = tree_packages(o->packages);
    if (packages == NULL)
        return 1;

    tree_list_t found;
    memset(&found, 0, sizeof found);
    tree_search(packages, o->args.v[0], &found);

    for (size_t i = 0; i < found.n; i++) {
        tree_entry_t *e = &found.v[i];
        if (e->is_system) {
            printf("%-24s system\n", e->name);
            continue;
        }
        /* Recipe column, comma-separated, source included when it exists. */
        char *src = path_join(e->dir, "source.toml");
        int have_source = is_file(src);
        free(src);
        if (e->recipes.n == 0 && !have_source) {
            printf("%-24s package\n", e->name);
            continue;
        }
        printf("%-24s package  ", e->name);
        int first = 1;
        if (e->recipes.n > 0) {
            printf("%s", e->recipes.v[0]);
            first = 0;
        }
        if (have_source) {
            printf("%ssource", first ? "" : ", ");
            first = 0;
        }
        printf("\n");
    }

    tree_list_free(&found);
    free(packages);
    return ferror(stdout) ? 1 : 0;
}
