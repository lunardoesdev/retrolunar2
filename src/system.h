/* A system: the directory `system@<name>` and the peer TOML files inside it.
 *
 * Peers merge into one namespace. The same dotted key in two files is an
 * error, never a silent override. Every single-line scalar becomes a shell
 * variable in the generated script (its name is the key's last component),
 * and every such key is addressable as `${<dotted path>}` from any other
 * fragment. A value containing a newline, or a `script` key, is emitted
 * verbatim as shell — that is where arbitrary computation lives (globbing a
 * directory, `$(command -v make)`), exactly like v1's setup fragment. */
#ifndef RETROLUNAR_SYSTEM_H
#define RETROLUNAR_SYSTEM_H

#include <stdio.h>

#include "toml_read.h"
#include "util.h"

typedef struct {
    char *key;   /* dotted path from the document root: system.triplet.host */
    char *value; /* still holds ${...} references, resolved on emission */
    char *file;
    int line;
} sym_t;

typedef struct {
    sym_t *v;
    size_t n, cap;
} symtab_t;

enum emit_kind { EMIT_SCRIPT, EMIT_VAR };

typedef struct {
    enum emit_kind kind;
    char *name;  /* shell variable name (EMIT_VAR) */
    char *value; /* shell text: a value, or the whole script fragment */
    char *file;
    int line;
    int multiline;
} emit_t;

typedef struct {
    char *name; /* short name, e.g. mingw64 */
    char *dir;  /* absolute path of system@<name> */
    toml_doc_t *docs;
    char **files;
    size_t ndocs;
    symtab_t syms;
    emit_t *emits;
    size_t nemits, emitcap;
    strlist_t fallbacks;
    strlist_t probes;
    int has_probe;
} system_t;

/* 0 on success; every failure prints a message naming the file and line. */
int system_load(const char *packages, const char *name, system_t *out);
void system_free(system_t *s);

/* Symbol lookup by dotted path, or NULL. */
const char *system_sym(const system_t *s, const char *path);

/* Resolve ${...} references in `text`. A name that is not in the symbol table
 * and is not a plain shell variable is an error, so a typo fails here rather
 * than in the middle of a build. Returns a malloc'd string, or NULL on error. */
char *system_expand(const system_t *s, const char *text, const char *where);

/* Write the shell fragment: script parts verbatim, then the variable
 * assignments of each file with one `export` line per file. */
void system_emit(const system_t *s, FILE *out);

#endif /* RETROLUNAR_SYSTEM_H */
