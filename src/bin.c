#include "bin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- file access --------------------------------------------------------- */

static int read_whole(const char *path, unsigned char **out, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t cap = 1 << 16, n = 0;
    unsigned char *buf = xmalloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            if (ferror(f)) {
                fclose(f);
                free(buf);
                return -1;
            }
            break;
        }
    }
    fclose(f);
    *out = buf;
    *outlen = n;
    return 0;
}

void bin_close(bin_t *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* --- little-endian readers ---------------------------------------------- */

static unsigned int rd16(const unsigned char *p) { return p[0] | ((unsigned int)p[1] << 8); }

static unsigned long long rd32(const unsigned char *p) {
    return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) |
           ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24);
}

static unsigned long long rd64(const unsigned char *p) {
    return rd32(p) | (rd32(p + 4) << 32);
}

static void wr32(unsigned char *p, unsigned long long v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void wr64(unsigned char *p, unsigned long long v) {
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

/* --- ELF ---------------------------------------------------------------- */

#define ELF_PT_LOAD 1
#define ELF_PT_DYNAMIC 2
#define ELF_DT_NULL 0
#define ELF_DT_NEEDED 1
#define ELF_DT_STRTAB 5
#define ELF_DT_STRSZ 10
#define ELF_DT_RPATH 15
#define ELF_DT_RUNPATH 29

typedef struct {
    unsigned long long vaddr;
    int is64;
} dyn_ref_t;

typedef struct {
    size_t off, vaddr, filesz, memsz;
} load_t;

/* The ELF layout, as far as this tool cares about it. */
typedef struct {
    int is64;
    const unsigned char *ph;
    int phnum, phentsize;
    const unsigned char *sh;
    int shnum, shentsize;
    load_t loads[16];
    int nloads;
} elf_t;

static void elf_parse(const bin_t *b, elf_t *e) {
    memset(e, 0, sizeof *e);
    e->is64 = b->bits == 64;
    e->phnum = (int)rd16(b->data + (e->is64 ? 56 : 44));
    e->phentsize = (int)rd16(b->data + (e->is64 ? 54 : 42));
    size_t phoff = e->is64 ? (size_t)rd64(b->data + 32) : (size_t)rd32(b->data + 28);
    e->ph = b->data + phoff;
    e->shnum = (int)rd16(b->data + (e->is64 ? 60 : 48));
    e->shentsize = (int)rd16(b->data + (e->is64 ? 58 : 46));
    size_t shoff = e->is64 ? (size_t)rd64(b->data + 40) : (size_t)rd32(b->data + 32);
    e->sh = shoff != 0 && shoff < b->len ? b->data + shoff : NULL;
    for (int i = 0; i < e->phnum; i++) {
        const unsigned char *p = e->ph + (size_t)i * e->phentsize;
        if ((unsigned int)rd32(p) != ELF_PT_LOAD)
            continue;
        load_t l;
        if (e->is64) {
            l.off = (size_t)rd64(p + 8);
            l.vaddr = (size_t)rd64(p + 16);
            l.filesz = (size_t)rd64(p + 32);
            l.memsz = (size_t)rd64(p + 40);
        } else {
            l.off = (size_t)rd32(p + 4);
            l.vaddr = (size_t)rd32(p + 8);
            l.filesz = (size_t)rd32(p + 16);
            l.memsz = (size_t)rd32(p + 20);
        }
        if (e->nloads < (int)(sizeof e->loads / sizeof e->loads[0]))
            e->loads[e->nloads++] = l;
    }
}

/* Where in the file a virtual address lives: found through PT_LOAD, which is
 * how a dynamic string table is addressed in a dynamic section. */
static int vaddr_to_off(const elf_t *e, unsigned long long addr, size_t *off) {
    for (int i = 0; i < e->nloads; i++) {
        const load_t *l = &e->loads[i];
        if (addr >= l->vaddr && addr < l->vaddr + l->filesz) {
            *off = (size_t)(addr - l->vaddr + l->off);
            return 0;
        }
    }
    return -1;
}

size_t bin_dynstr_string(const bin_t *b, unsigned long long strtab_addr,
                         unsigned long long index, char *out, size_t outsz) {
    if (b->kind != BIN_ELF)
        return 0;
    elf_t e;
    elf_parse(b, &e);
    size_t off = 0;
    if (vaddr_to_off(&e, strtab_addr + index, &off) != 0)
        return 0;
    if (off >= b->len)
        return 0;
    size_t n = 0;
    while (off + n < b->len && b->data[off + n] != '\0' && n + 1 < outsz) {
        out[n] = (char)b->data[off + n];
        n++;
    }
    out[n] = '\0';
    return n;
}

/* The dynamic table: its location, how many entries are used, and how many fit.
 * Two passes, because DT_NEEDED comes before DT_STRTAB and a name cannot be read
 * before the string table is known. */
static int elf_dynamic(const bin_t *b, unsigned long long *strtab,
                       unsigned long long *rpath_addr, strlist_t *needed, size_t *dyn_off,
                       size_t *null_index, size_t *capacity) {
    elf_t e;
    elf_parse(b, &e);
    *strtab = 0;
    *rpath_addr = 0;
    *dyn_off = 0;
    *null_index = 0;
    *capacity = 0;

    size_t doff = 0, dsz = 0;
    for (int i = 0; i < e.phnum; i++) {
        const unsigned char *p = e.ph + (size_t)i * e.phentsize;
        if ((unsigned int)rd32(p) != ELF_PT_DYNAMIC)
            continue;
        doff = e.is64 ? (size_t)rd64(p + 8) : (size_t)rd32(p + 4);
        dsz = e.is64 ? (size_t)rd64(p + 32) : (size_t)rd32(p + 16);
        break;
    }
    if (doff == 0)
        return 0;
    *dyn_off = doff;

    size_t dent = e.is64 ? 16 : 8;
    /* Capacity prefers the section header, which is exact; a stripped file has
     * none, and the segment size is then the only thing to go on. The gap
     * between the used entries and the capacity is where a new entry fits. */
    size_t cap_entries = dsz / dent;
    if (e.sh != NULL) {
        for (int i = 0; i < e.shnum; i++) {
            const unsigned char *sh = e.sh + (size_t)i * e.shentsize;
            if ((unsigned int)rd32(sh + 4) != 6) /* SHT_DYNAMIC */
                continue;
            size_t size = e.is64 ? (size_t)rd64(sh + 32) : (size_t)rd32(sh + 20);
            size_t n = size / dent;
            if (n > cap_entries)
                cap_entries = n;
            break;
        }
    }
    *capacity = cap_entries;

    for (int pass = 0; pass < 2; pass++) {
        size_t used = 0;
        for (size_t o = 0; o + dent <= dsz && doff + o + dent <= b->len; o += dent) {
            const unsigned char *d = b->data + doff + o;
            long long tag = e.is64 ? (long long)rd64(d) : (long long)rd32(d);
            unsigned long long val = e.is64 ? rd64(d + 8) : rd32(d + 4);
            used++;
            if (tag == ELF_DT_NULL) {
                if (pass == 0)
                    *null_index = used - 1;
                break;
            }
            if (pass == 0) {
                if (tag == ELF_DT_STRTAB)
                    *strtab = val;
                else if (tag == ELF_DT_RUNPATH || tag == ELF_DT_RPATH)
                    *rpath_addr = val;
            } else if (tag == ELF_DT_NEEDED) {
                char name[512];
                if (bin_dynstr_string(b, *strtab, val, name, sizeof name) > 0 &&
                    !strlist_has(needed, name))
                    strlist_add(needed, name);
            }
        }
    }
    return 0;
}

int bin_needed(const bin_t *b, strlist_t *needed) {
    if (b->kind == BIN_ELF) {
        unsigned long long strtab = 0, rpath_addr = 0;
        size_t doff = 0, null_index = 0, capacity = 0;
        return elf_dynamic(b, &strtab, &rpath_addr, needed, &doff, &null_index, &capacity);
    }
    if (b->kind != BIN_PE)
        return -1;

    /* PE: the import table is directory entry 1, and each descriptor names one
     * DLL. Delay-loaded DLLs are in directory 13 and are read as well, because
     * a program that needs one at run time still needs the file beside it. */
    size_t pe = (size_t)rd32(b->data + 0x3c);
    if (pe + 24 > b->len || memcmp(b->data + pe, "PE\0\0", 4) != 0)
        return -1;
    const unsigned char *coff = b->data + pe + 4;
    int nsec = (int)rd16(coff + 2);
    int optsize = (int)rd16(coff + 16);
    const unsigned char *opt = coff + 20;
    int magic = (int)rd16(opt);
    int pe32plus = magic == 0x20b;
    size_t ddir = pe32plus ? 112 : 96;
    const unsigned char *sections = opt + optsize;

    struct {
        size_t rva, size;
    } dirs[16];
    memset(dirs, 0, sizeof dirs);
    for (int i = 0; i < 16; i++) {
        if (ddir + (size_t)i * 8 + 8 > (size_t)optsize)
            break;
        dirs[i].rva = (size_t)rd32(opt + ddir + (size_t)i * 8);
        dirs[i].size = (size_t)rd32(opt + ddir + (size_t)i * 8 + 4);
    }

    /* RVA to file offset through the section table. */
    for (int pass = 0; pass < 2; pass++) {
        size_t rva = pass == 0 ? dirs[1].rva : dirs[13].rva;
        size_t size = pass == 0 ? dirs[1].size : dirs[13].size;
        if (rva == 0)
            continue;
        size_t off = 0;
        int found = 0;
        for (int s = 0; s < nsec && !found; s++) {
            const unsigned char *sh = sections + (size_t)s * 40;
            size_t va = (size_t)rd32(sh + 12);
            size_t rawsize = (size_t)rd32(sh + 16);
            size_t rawptr = (size_t)rd32(sh + 20);
            if (rva >= va && rva < va + rawsize) {
                off = rva - va + rawptr;
                found = 1;
            }
        }
        if (!found || off >= b->len)
            continue;
        size_t count = pass == 0 ? size / 20 : size / 32;
        for (size_t i = 0; i < count; i++) {
            size_t erec = pass == 0 ? 20 : 32;
            size_t d = off + i * erec;
            if (d + erec > b->len)
                break;
            size_t name_rva = (size_t)rd32(b->data + d + (pass == 0 ? 12 : 4));
            /* The table is terminated by an all-zero descriptor: the directory's
             * declared size covers more than the descriptors themselves, and
             * walking past the terminator reads whatever follows it. */
            if (name_rva == 0)
                break;
            /* Name RVA is in the header's own section (usually .rdata). */
            size_t noff = 0;
            int nf = 0;
            for (int s = 0; s < nsec && !nf; s++) {
                const unsigned char *sh = sections + (size_t)s * 40;
                size_t va = (size_t)rd32(sh + 12);
                size_t rawsize = (size_t)rd32(sh + 16);
                size_t rawptr = (size_t)rd32(sh + 20);
                if (name_rva >= va && name_rva < va + rawsize) {
                    noff = name_rva - va + rawptr;
                    nf = 1;
                }
            }
            if (!nf || noff >= b->len)
                continue;
            const char *name = (const char *)b->data + noff;
            size_t maxlen = b->len - noff;
            size_t l = strnlen(name, maxlen);
            if (l > 0 && l < 256 && !strlist_has(needed, name))
                strlist_add(needed, name);
        }
    }
    return 0;
}

int bin_rpath(const bin_t *b, char *value, size_t valuesz, size_t *str_off, size_t *str_len) {
    if (b->kind != BIN_ELF)
        return -1;
    unsigned long long strtab = 0, rpath_addr = 0;
    strlist_t ignored;
    strlist_init(&ignored);
    size_t doff = 0, null_index = 0, capacity = 0;
    elf_dynamic(b, &strtab, &rpath_addr, &ignored, &doff, &null_index, &capacity);
    strlist_free(&ignored);
    if (rpath_addr == 0)
        return 1; /* no entry to rewrite */
    elf_t e;
    elf_parse(b, &e);
    size_t off = 0;
    if (vaddr_to_off(&e, rpath_addr, &off) != 0)
        return -1;
    size_t n = 0;
    while (off + n < b->len && b->data[off + n] != '\0' && n + 1 < valuesz) {
        value[n] = (char)b->data[off + n];
        n++;
    }
    value[n] = '\0';
    *str_off = off;
    *str_len = n;
    return 0;
}

int bin_patch_rpath(const char *path, bin_t *b, size_t str_off, size_t str_len,
                    const char *replacement) {
    size_t need = strlen(replacement) + 1;
    if (need > str_len + 1)
        return 1; /* does not fit: leaving a half-written string would be worse */
    /* Rewriting in place only touches this file, and only this string. */
    memcpy(b->data + str_off, replacement, need - 1);
    memset(b->data + str_off + need - 1, 0, str_len + 1 - (need - 1));
    FILE *f = fopen(path, "r+b");
    if (f == NULL)
        return -1;
    int rc = 0;
    if (fseek(f, (long)str_off, SEEK_SET) != 0 ||
        fwrite(b->data + str_off, 1, str_len + 1, f) != str_len + 1)
        rc = -1;
    fclose(f);
    return rc;
}

/* --- adding an $ORIGIN runtime path that is not there yet --------------- */

#define MAX_SEC 128

/* Find room for a string inside a loaded segment.
 *
 * Only padding counts: the gaps between the sections the segment holds, and the
 * tail after the last one, all inside the bytes the segment actually reads from
 * the file. Writing into a section would be writing into somebody's data, and
 * the loader reads exactly p_filesz bytes, so a hole in there is the one place
 * this is safe. The bytes must be zero already, which is what makes it padding
 * rather than something in use. */
static int find_room(const bin_t *b, const elf_t *e, const load_t *l, size_t want_len,
                     size_t *file_off, unsigned long long *vaddr) {
    size_t beg[MAX_SEC], end[MAX_SEC];
    int n = 0;
    /* The ELF header, the program header table and the section header table are
     * not sections, so they would otherwise look like the roomiest padding in
     * the file. Overwriting the first of them is how a working binary becomes a
     * segfault. */
    size_t fixed[3][2];
    fixed[0][0] = 0;
    fixed[0][1] = e->is64 ? 64 : 52;
    fixed[1][0] = (size_t)(e->ph - b->data);
    fixed[1][1] = fixed[1][0] + (size_t)e->phnum * (size_t)e->phentsize;
    int nfixed = 2;
    if (e->sh != NULL) {
        fixed[2][0] = (size_t)(e->sh - b->data);
        fixed[2][1] = fixed[2][0] + (size_t)e->shnum * (size_t)e->shentsize;
        nfixed = 3;
    }
    for (int i = 0; i < nfixed && n < MAX_SEC; i++) {
        if (fixed[i][0] < l->off || fixed[i][0] > l->off + l->filesz)
            continue;
        beg[n] = fixed[i][0];
        end[n] = fixed[i][1] > l->off + l->filesz ? l->off + l->filesz : fixed[i][1];
        n++;
    }
    if (e->sh != NULL) {
        for (int i = 0; i < e->shnum && n < MAX_SEC; i++) {
            const unsigned char *sh = e->sh + (size_t)i * e->shentsize;
            unsigned int type = (unsigned int)rd32(sh + 4);
            if (type == 0 || type == 8) /* SHT_NULL, SHT_NOBITS */
                continue;
            size_t off = e->is64 ? (size_t)rd64(sh + 24) : (size_t)rd32(sh + 16);
            size_t size = e->is64 ? (size_t)rd64(sh + 32) : (size_t)rd32(sh + 20);
            if (size == 0 || off < l->off || off + size > l->off + l->filesz)
                continue;
            beg[n] = off;
            end[n] = off + size;
            n++;
        }
    }
    for (int i = 1; i < n; i++) { /* insertion sort: section counts are small */
        size_t kb = beg[i], ke = end[i];
        int j = i - 1;
        while (j >= 0 && beg[j] > kb) {
            beg[j + 1] = beg[j];
            end[j + 1] = end[j];
            j--;
        }
        beg[j + 1] = kb;
        end[j + 1] = ke;
    }

    size_t seg_end = l->off + l->filesz;
    for (int i = 0; i <= n; i++) {
        size_t from = i == 0 ? l->off : end[i - 1];
        size_t to = i == n ? seg_end : beg[i];
        if (to <= from || to - from < want_len)
            continue;
        for (size_t o = from; o + want_len <= to; o++) {
            int zero = 1;
            for (size_t k = 0; k < want_len; k++) {
                if (b->data[o + k] != 0) {
                    zero = 0;
                    break;
                }
            }
            if (!zero)
                continue;
            *file_off = o;
            *vaddr = (unsigned long long)(l->vaddr + (o - l->off));
            return 0;
        }
    }
    return -1;
}

/* Give an ELF file an $ORIGIN runtime path, so a bundle of files finds its own
 * libraries wherever it is copied. Nothing is rebuilt and nothing grows: a free
 * dynamic slot and the padding after a loaded segment are used, and the file is
 * left byte-for-byte identical when either is missing. */
int bin_add_origin_rpath(const char *path, bin_t *b, char *note, size_t notesz) {
    if (b->kind != BIN_ELF) {
        snprintf(note, notesz, "not an ELF file");
        return 1;
    }
    elf_t e;
    elf_parse(b, &e);
    unsigned long long strtab = 0, rpath_addr = 0;
    strlist_t ignored;
    strlist_init(&ignored);
    size_t doff = 0, null_index = 0, capacity = 0;
    elf_dynamic(b, &strtab, &rpath_addr, &ignored, &doff, &null_index, &capacity);
    strlist_free(&ignored);
    if (rpath_addr != 0) {
        snprintf(note, notesz, "already has a runtime path");
        return 1;
    }
    size_t dent = e.is64 ? 16 : 8;
    if (capacity < null_index + 2) {
        /* A new entry would have to shift the table and every address in it.
         * Report rather than gamble with a binary. */
        snprintf(note, notesz, "dynamic table is full (%zu of %zu slots)", null_index + 1,
                 capacity);
        return 1;
    }

    const char *want = "$ORIGIN";
    size_t want_len = strlen(want) + 1;
    size_t str_file_off = 0;
    unsigned long long str_vaddr = 0;
    int placed = 0;
    for (int i = 0; i < e.nloads && !placed; i++) {
        if (find_room(b, &e, &e.loads[i], want_len, &str_file_off, &str_vaddr) == 0)
            placed = 1;
    }
    if (!placed) {
        snprintf(note, notesz, "no room for the string in a loaded segment");
        return 1;
    }

    /* The value follows what the system linker writes for this tag: an offset
     * into the string table, so the loader finds the text at DT_STRTAB + value.
     * Writing a virtual address here is what puts a working binary into
     * decompose_rpath() with a pointer into nowhere. */
    unsigned long long rel = (unsigned long long)((long long)str_vaddr - (long long)strtab);
    memcpy(b->data + str_file_off, want, want_len);
    unsigned char *slot = b->data + doff + null_index * dent;
    if (e.is64) {
        wr64(slot, ELF_DT_RUNPATH);
        wr64(slot + 8, rel);
        wr64(slot + dent, ELF_DT_NULL);
        wr64(slot + dent + 8, 0);
    } else {
        wr32(slot, ELF_DT_RUNPATH);
        wr32(slot + 4, rel);
        wr32(slot + dent, ELF_DT_NULL);
        wr32(slot + dent + 4, 0);
    }

    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        snprintf(note, notesz, "cannot reopen for writing");
        return -1;
    }
    int rc = 0;
    if (fseek(f, (long)str_file_off, SEEK_SET) != 0 ||
        fwrite(b->data + str_file_off, 1, want_len, f) != want_len)
        rc = -1;
    if (rc == 0 && fseek(f, (long)(doff + null_index * dent), SEEK_SET) != 0)
        rc = -1;
    if (rc == 0 && fwrite(slot, 1, 2 * dent, f) != 2 * dent)
        rc = -1;
    fclose(f);
    if (rc != 0) {
        snprintf(note, notesz, "write failed");
        return -1;
    }
    snprintf(note, notesz, "$ORIGIN added to the runtime path");
    return 0;
}

/* Case-insensitive: the PE import table spells Windows libraries in capitals
 * (`KERNEL32.dll`) while a .pc file or an ELF name would not. */
static int name_eq(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
    }
    return *a == '\0' && *b == '\0';
}

int bin_name_is_platform(const char *name) {
    static const char *const PLATFORM[] = {
        /* bionic, and the NDK's own runtime libraries */
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so",
        "libstdc++.so", "libc++_shared.so", "libunwind.so", "libz.so",
        "ld-android.so", "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0",
        "librt.so.1", "libgcc_s.so", "libgcc_s.so.1", "libutil.so.1",
        /* Windows */
        "kernel32.dll", "user32.dll", "advapi32.dll", "gdi32.dll", "shell32.dll",
        "ole32.dll", "oleaut32.dll", "ws2_32.dll", "crypt32.dll", "iphlpapi.dll",
        "msvcrt.dll", "ntdll.dll", "shlwapi.dll", "version.dll", "winmm.dll",
        "comdlg32.dll", "userenv.dll", "bcrypt.dll", "secur32.dll", "wintrust.dll",
        "dbghelp.dll", "psapi.dll", "setupapi.dll", "rpcrt4.dll", "dnsapi.dll",
        "normaliz.dll", "wldap32.dll", "winhttp.dll", "mswsock.dll", "netapi32.dll",
        "powrprof.dll", "uxtheme.dll", "gdiplus.dll", "winspool.drv", "wtsapi32.dll",
        "mpr.dll", "authz.dll", "cfgmgr32.dll", "propsys.dll", "imm32.dll",
        "libgcc_s_seh-1.dll", "libgcc_s_dw2-1.dll", "libstdc++-6.dll",
        "libwinpthread-1.dll",
    };
    for (size_t i = 0; i < sizeof PLATFORM / sizeof PLATFORM[0]; i++)
        if (name_eq(name, PLATFORM[i]))
            return 1;
    /* Windows API sets: their real bodies live in the OS. */
    if (strncmp(name, "api-ms-win-", 11) == 0 || strncmp(name, "ext-ms-win-", 11) == 0)
        return 1;
    return 0;
}

/* --- opening ------------------------------------------------------------ */

int bin_open(const char *path, bin_t *out) {
    memset(out, 0, sizeof *out);
    if (read_whole(path, &out->data, &out->len) != 0)
        return -1;
    if (out->len > 4 && out->data[0] == 0x7f && out->data[1] == 'E' &&
        out->data[2] == 'L' && out->data[3] == 'F') {
        out->kind = BIN_ELF;
        out->bits = out->data[4] == 2 ? 64 : 32;
        if (out->data[5] != 1) { /* not little-endian */
            bin_close(out);
            return -1;
        }
        return 0;
    }
    if (out->len > 0x40 && out->data[0] == 'M' && out->data[1] == 'Z') {
        out->kind = BIN_PE;
        out->bits = 0;
        return 0;
    }
    bin_close(out);
    return -1;
}
