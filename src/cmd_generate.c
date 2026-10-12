/* generate [-o FILE] [-x] [--cores N] — the same script as install, with
 * somewhere to put it and the option to run it.
 *
 * With -o the script becomes a real, executable file; without it and with -x
 * it is piped straight into sh, so nothing is left behind. -x hands the
 * script's own exit status back, so a failed build cannot look like a
 * successful generate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "cmds.h"
#include "resolve.h"

int build_script_for(const options_t *o, char **packages_out, graph_t *g, char **script,
                     size_t *len);

static int run_script(const char *path, const char *script, size_t len) {
    int rc;
    if (path != NULL) {
        size_t need = strlen(path) + 16;
        char *cmd = xmalloc(need);
        snprintf(cmd, need, "sh '%s'", path);
        rc = system(cmd);
        free(cmd);
    } else {
        FILE *p = popen("sh", "w");
        if (p == NULL) {
            fputs("retrolunar: cannot run sh\n", stderr);
            return 1;
        }
        if (len > 0 && fwrite(script, 1, len, p) != len) {
            fputs("retrolunar: cannot pipe the script to sh\n", stderr);
            pclose(p);
            return 1;
        }
        rc = pclose(p);
    }
    if (rc == -1) {
        fputs("retrolunar: cannot run sh\n", stderr);
        return 1;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0)
        return 0;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return 1;
}

int cmd_generate(const options_t *o) {
    graph_t g;
    char *packages = NULL, *script = NULL;
    size_t len = 0;
    if (build_script_for(o, &packages, &g, &script, &len) != 0)
        return 1;

    int rc = 0;
    if (o->output == NULL && !o->execute) {
        if (len > 0 && fwrite(script, 1, len, stdout) != len) {
            fputs("retrolunar: write failed\n", stderr);
            rc = 1;
        }
    } else if (o->output != NULL) {
        FILE *f = fopen(o->output, "wb");
        if (f == NULL) {
            fprintf(stderr, "retrolunar: cannot write %s\n", o->output);
            rc = 1;
        } else if (len > 0 && fwrite(script, 1, len, f) != len) {
            fprintf(stderr, "retrolunar: short write to %s\n", o->output);
            fclose(f);
            rc = 1;
        } else if (fclose(f) != 0) {
            fprintf(stderr, "retrolunar: cannot close %s\n", o->output);
            rc = 1;
        } else {
            if (chmod(o->output, 0755) != 0)
                fprintf(stderr, "retrolunar: warning: cannot chmod +x %s\n", o->output);
            fprintf(stderr, "retrolunar: wrote %s\n", o->output);
            if (o->execute)
                rc = run_script(o->output, script, len);
        }
    } else {
        rc = run_script(NULL, script, len);
    }

    free(script);
    graph_free(&g);
    free(packages);
    return rc;
}
