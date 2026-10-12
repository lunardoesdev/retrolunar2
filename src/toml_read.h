/* A typed reader on top of tomlc17.
 *
 * tomlc17 hands back a generic tree and validates no schema, so every lookup
 * that matters goes through here: a missing key, a wrong type or an unknown
 * key becomes a message naming the file, the line and the key. Nothing in
 * the resolver reads a toml_datum_t directly. */
#ifndef RETROLUNAR_TOML_READ_H
#define RETROLUNAR_TOML_READ_H

#include <stdarg.h>

#include "tomlc17.h"

#include "util.h"

typedef struct {
    char *path;       /* the file the document came from */
    toml_result_t res; /* parser result; valid while the document is */
} toml_doc_t;

/* 0 on success. On failure the reason is on stderr and the document must not
 * be used. */
int toml_doc_load(toml_doc_t *d, const char *path);
void toml_doc_free(toml_doc_t *d);
toml_datum_t toml_doc_root(const toml_doc_t *d);

/* Message: "<file>:<line>: <text>". */
void toml_err(toml_datum_t where, const char *fmt, ...);

/* Sub-table lookups. `ctx` names what is being read, e.g. "system@mingw64". */
toml_datum_t toml_req_table(toml_datum_t parent, const char *key, const char *ctx);
toml_datum_t toml_opt_table(toml_datum_t parent, const char *key);

/* Scalars. Numbers and booleans are stringified on request, so a recipe can
 * say `version = 1.3` only if it wants to spell it that way. */
const char *toml_req_str(toml_datum_t parent, const char *key, const char *ctx);
const char *toml_opt_str(toml_datum_t parent, const char *key);

/* Arrays of strings. */
int toml_req_strarray(toml_datum_t parent, const char *key, const char *ctx,
                      strlist_t *out);
int toml_opt_strarray(toml_datum_t parent, const char *key, strlist_t *out);

/* Reject keys the caller does not know: `known` is NULL-terminated and the
 * message names the first key that is not in it. Returns the number of
 * rejected keys, all of them reported. */
int toml_check_keys(toml_datum_t table, const char *const *known, const char *ctx);

/* Iterate a table of scalars: used for `${system.*}` symbol tables and for the
 * v1-style "extra fields become shell variables" rule. */
typedef struct {
    toml_datum_t tab;
    int i;
} toml_iter_t;

void toml_iter_init(toml_iter_t *it, toml_datum_t table);
int toml_iter_next(toml_iter_t *it, const char **key, toml_datum_t *value);

/* Scalar datum to a malloc'd string, or NULL when it is not a scalar. */
char *toml_scalar_to_str(toml_datum_t d);

const char *toml_type_name(toml_type_t t);

#endif /* RETROLUNAR_TOML_READ_H */
