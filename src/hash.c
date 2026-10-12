#include "hash.h"

#include <stdio.h>
#include <string.h>

static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static unsigned int ror(unsigned int x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void compress(sha256_t *s, const unsigned char *p) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned int a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    unsigned int e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[i] + w[i];
        unsigned int S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        unsigned int mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667u;
    s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu;
    s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu;
    s->h[7] = 0x5be0cd19u;
    s->bits = 0;
    s->buflen = 0;
}

void sha256_update(sha256_t *s, const void *data, size_t len) {
    const unsigned char *p = data;
    s->bits += (unsigned long long)len * 8;
    while (len > 0) {
        size_t take = 64 - s->buflen;
        if (take > len)
            take = len;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take;
        p += take;
        len -= take;
        if (s->buflen == 64) {
            compress(s, s->buf);
            s->buflen = 0;
        }
    }
}

void sha256_final_hex(sha256_t *s, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned long long bits = s->bits;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->buflen != 56)
        sha256_update(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            unsigned char byte = (unsigned char)(s->h[i] >> (24 - j * 8));
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 15];
        }
    }
    *out = '\0';
}

void sha256_hex(const void *data, size_t len, char out[65]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final_hex(&s, out);
}

int sha256_file_hex(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    sha256_t s;
    sha256_init(&s);
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&s, buf, got);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha256_final_hex(&s, out);
    return 0;
}
