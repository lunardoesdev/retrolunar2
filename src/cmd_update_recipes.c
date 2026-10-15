/* update-recipes — move the packages tree forward.
 *
 * A tree is a git checkout when it came from the built-in repository or from
 * --repo, and it can be anything when --packages named it. This asks the first
 * kind to move: fetch, then a fast-forward pull, so local work is never
 * rewritten. A directory with nothing to pull from is told so and that is the
 * whole answer.
 *
 * It is the same work --refresh-packages does before a command that needs the
 * tree: that flag is for one run, this is for the tree itself.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cmds.h"
#include "repos.h"
#include "tree.h"

int cmd_update_recipes(const options_t *o) {
    char *dir = tree_packages(o->packages);
    if (dir == NULL)
        return 1;
    int rc = repo_refresh(dir);
    if (rc == 0)
        printf("retrolunar: recipes in %s are up to date\n", dir);
    free(dir);
    return rc == 0 ? 0 : 1;
}
