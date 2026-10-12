/* sha256, implemented here so the tool has no libcrypto dependency.
 * Content hashes are the whole freshness story, so this has to be right. */
#ifndef RETROLUNAR_HASH_H
#define RETROLUNAR_HASH_H

#include <stddef.h>

typedef struct {
    unsigned int h[8];
    unsigned long long bits;
    unsigned char buf[64];
    size_t buflen;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final_hex(sha256_t *s, char out[65]);

/* One-shot digest of a buffer. */
void sha256_hex(const void *data, size_t len, char out[65]);

/* Digest of a file's contents. Returns 0 on success, -1 when unreadable. */
int sha256_file_hex(const char *path, char out[65]);

#endif /* RETROLUNAR_HASH_H */
