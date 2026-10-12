/* Reading what a binary needs at run time, in the tool itself.
 *
 * `export` has to work on whatever host it is run from, so it does not shell
 * out to readelf or objdump: it reads the ELF dynamic section and the PE
 * import table directly. Only little-endian ELF is handled, which is every
 * target this project builds. */
#ifndef RETROLUNAR_BIN_H
#define RETROLUNAR_BIN_H

#include "util.h"

typedef enum { BIN_UNKNOWN = 0, BIN_ELF, BIN_PE } bin_kind_t;

typedef struct {
    bin_kind_t kind;
    int bits;      /* 32 or 64 */
    unsigned char *data;
    size_t len;
} bin_t;

/* 0 on success; -1 when the file is not a recognisable ELF or PE. */
int bin_open(const char *path, bin_t *out);
void bin_close(bin_t *b);

/* Absolute file offset of the string table entry `offset_in_strtab`, or 0. */
size_t bin_dynstr_string(const bin_t *b, unsigned long long strtab_addr,
                         unsigned long long offset_in_strtab, char *out, size_t outsz);

/* Names this binary loads at run time, appended to `needed`. Returns 0 when the
 * list is complete (possibly empty). */
int bin_needed(const bin_t *b, strlist_t *needed);

/* The existing RUNPATH/RPATH string and where it lives, when there is one: the
 * only place an $ORIGIN can be written without rebuilding the file. */
int bin_rpath(const bin_t *b, char *value, size_t valuesz, size_t *str_off, size_t *str_len);

/* Overwrite that string with `replacement` (NUL padded) if it fits. 0 when it
 * was written, 1 when it did not fit, -1 on error. */
int bin_patch_rpath(const char *path, bin_t *b, size_t str_off, size_t str_len,
                    const char *replacement);

/* Give the file an $ORIGIN runtime path when there is a free dynamic slot and
 * room for the string in a loaded segment. 0 when it was written, 1 when the
 * file was left untouched (with the reason in `note`), -1 on a write error. */
int bin_add_origin_rpath(const char *path, bin_t *b, char *note, size_t notesz);

/* Is this name provided by the platform or the toolchain rather than by us?
 * bionic's libc/libm/libdl, the Windows API, libgcc/libstdc++ from mingw. */
int bin_name_is_platform(const char *name);

#endif /* RETROLUNAR_BIN_H */
