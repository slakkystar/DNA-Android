/*
 * dna - clean-room replacement CLI for DNA-Android
 * Written from the command contract used in assets/kr-script/dna.xml and
 * assets/samples (shell scripts) (not derived from the original binary).
 *
 */
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/sendfile.h>
#include <sched.h>
#include <strings.h>
#ifdef DNA_HAVE_XZ
#include "xz.h"
#endif

#define VERSION "1.0"
#define EXTRACT_UNSUPPORTED (-2)

static int g_debug = 0;
static int g_repack_quiet = 0;
static struct timespec g_part_t0;
static const char *g_sysroot = "";
static int g_thermal_limit_hot = 0;

static const char *syspath(char *buf, size_t n, const char *rel) {
    snprintf(buf, n, "%s%s", g_sysroot, rel);
    return buf;
}

static uint64_t meminfo_kb(const char *key);

#define VPRINTF(...) do { if (g_debug) printf(__VA_ARGS__); } while (0)
#define VWARN(...) do { if (g_debug) fprintf(stderr, __VA_ARGS__); } while (0)

static void part_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_part_t0);
}

static void part_done_line(void) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - g_part_t0.tv_sec) +
                  (double)(t1.tv_nsec - g_part_t0.tv_nsec) / 1000000000.0;
    printf("> Done in: %.2f secs\n", secs);
}

/* ---------- format detection ---------- */

static int rd(FILE *f, long off, void *buf, size_t n) {
    if (fseeko(f, off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, f) == n;
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Returns a short lowercase tag; the shell scripts compare against these. */
static const char *detect_type(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "unknown";
    uint8_t h[8] = {0};
    const char *t = "unknown";

    if (!rd(f, 0, h, 8)) goto out;

    if (le32(h) == 0xED26FF3Au)               { t = "sparse"; goto out; }
    if (!memcmp(h, "ANDROID!", 8))            { t = "boot"; goto out; }
    if (!memcmp(h, "VNDRBOOT", 8))            { t = "vendor_boot"; goto out; }
    if (!memcmp(h, "AVB0", 4))                { t = "vbmeta"; goto out; }
    if (!memcmp(h, "CrAU", 4))                { t = "payload"; goto out; }
    if (h[0] == 'P' && h[1] == 'K')           { t = "zip"; goto out; }
    if (h[0] == 0xD7 && h[1] == 0xB7 && h[2] == 0xAB && h[3] == 0x1E) { t = "dtbo"; goto out; }

    uint8_t b[4];
    if (rd(f, 1024, b, 4)) {
        uint32_t m = le32(b);
        if (m == 0xE0F5E1E2u) { t = "erofs"; goto out; }
        if (m == 0xF2F52010u) { t = "f2fs";  goto out; }
    }
    if (rd(f, 1080, b, 2) && b[0] == 0x53 && b[1] == 0xEF) { t = "ext"; goto out; }
    if (rd(f, 4096, b, 4) && le32(b) == 0x616c4467u)       { t = "super"; goto out; }
out:
    fclose(f);
    return t;
}

static int cmd_gettype(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: dna gettype <file>\n"); return 2; }
    puts(detect_type(argv[0]));
    return 0;
}


/* ---------- random-access image reader (raw or Android sparse) ---------- */

typedef struct { uint64_t out_off, len; uint32_t type, fill; uint64_t file_off; } chunk_t;

typedef struct {
    FILE *f;
    int sparse;
    uint64_t size;          /* logical size */
    chunk_t *ch; size_t nch;
} img_t;

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static int img_open(img_t *im, const char *path) {
    memset(im, 0, sizeof *im);
    im->f = fopen(path, "rb");
    if (!im->f) return -1;
    uint8_t h[28];
    if (rd(im->f, 0, h, 28) && le32(h) == 0xED26FF3Au) {
        uint16_t fh = le16(h + 8), chh = le16(h + 10);
        uint32_t bs = le32(h + 12), nchunks = le32(h + 20);
        im->sparse = 1;
        im->ch = calloc(nchunks ? nchunks : 1, sizeof(chunk_t));
        uint64_t out = 0, pos = fh;
        for (uint32_t i = 0; i < nchunks; i++) {
            uint8_t c[12];
            if (!rd(im->f, pos, c, 12)) return -1;
            uint16_t type = le16(c); uint32_t blks = le32(c + 4), tot = le32(c + 8);
            chunk_t *k = &im->ch[im->nch];
            k->type = type; k->out_off = out; k->len = (uint64_t)blks * bs;
            k->file_off = pos + chh;
            if (type == 0xCAC2) { uint8_t fv[4]; if (!rd(im->f, k->file_off, fv, 4)) return -1; k->fill = le32(fv); }
            if (type == 0xCAC1 || type == 0xCAC2 || type == 0xCAC3) { im->nch++; out += k->len; }
            pos += tot;
        }
        im->size = out;
    } else {
        fseeko(im->f, 0, SEEK_END);
        im->size = (uint64_t)ftello(im->f);
    }
    return 0;
}

static void img_close(img_t *im) { if (im->f) fclose(im->f); free(im->ch); }

/* read n bytes at logical offset; holes/don't-care read as zero */
static int img_read(img_t *im, uint64_t off, void *buf, size_t n) {
    uint8_t *o = buf;
    if (off + n > im->size) return 0;
    if (!im->sparse) return rd(im->f, (long)off, o, n);
    size_t lo = 0, hi = im->nch;                  /* binary search first chunk */
    while (lo < hi) { size_t m = (lo + hi) / 2; if (im->ch[m].out_off + im->ch[m].len <= off) lo = m + 1; else hi = m; }
    while (n && lo < im->nch) {
        chunk_t *k = &im->ch[lo++];
        uint64_t in = off - k->out_off, take = k->len - in;
        if (take > n) take = n;
        if (k->type == 0xCAC1) { if (!rd(im->f, (long)(k->file_off + in), o, take)) return 0; }
        else if (k->type == 0xCAC2) { for (uint64_t i = 0; i < take; i++) o[i] = ((uint8_t *)&k->fill)[(in + i) & 3]; }
        else memset(o, 0, take);
        o += take; off += take; n -= take;
    }
    return n == 0;
}

/* ---------- liblp (super.img) metadata ---------- */

typedef struct { char name[37]; uint32_t first_ext, num_ext; uint64_t sectors; } part_t;
typedef struct { uint64_t sectors; uint32_t type; uint64_t data; } ext_t;

typedef struct { part_t *p; size_t np; ext_t *e; size_t ne; } lp_t;

static int lp_load(img_t *im, lp_t *lp) {
    uint8_t g[52];
    if (!img_read(im, 4096, g, 52) || le32(g) != 0x616c4467u) return -1;
    uint32_t max_meta = le32(g + 40);
    (void)max_meta;
    uint8_t hd[256];
    uint64_t hoff = 4096 + 4096 * 2;                       /* primary slot 0 */
    if (!img_read(im, hoff, hd, 128) || le32(hd) != 0x414C5030u) return -1;
    uint32_t hsz = le32(hd + 8), tsz = le32(hd + 44);
    uint32_t po = le32(hd + 80), pn = le32(hd + 84), ps = le32(hd + 88);
    uint32_t eo = le32(hd + 92), en = le32(hd + 96), es = le32(hd + 100);
    if (ps < 52 || es < 24 || tsz > (64u << 20)) return -1;
    uint8_t *t = malloc(tsz ? tsz : 1);
    if (!img_read(im, hoff + hsz, t, tsz)) { free(t); return -1; }
    if ((uint64_t)po + (uint64_t)pn * ps > tsz || (uint64_t)eo + (uint64_t)en * es > tsz) { free(t); return -1; }
    lp->np = pn; lp->ne = en;
    lp->p = calloc(pn ? pn : 1, sizeof(part_t));
    lp->e = calloc(en ? en : 1, sizeof(ext_t));
    for (uint32_t i = 0; i < en; i++) {
        const uint8_t *x = t + eo + i * es;
        lp->e[i].sectors = le32(x) | ((uint64_t)le32(x + 4) << 32);
        lp->e[i].type = le32(x + 8);
        lp->e[i].data = le32(x + 12) | ((uint64_t)le32(x + 16) << 32);
    }
    for (uint32_t i = 0; i < pn; i++) {
        const uint8_t *x = t + po + i * ps;
        memcpy(lp->p[i].name, x, 36); lp->p[i].name[36] = 0;
        lp->p[i].first_ext = le32(x + 40); lp->p[i].num_ext = le32(x + 44);
        for (uint32_t j = 0; j < lp->p[i].num_ext && lp->p[i].first_ext + j < en; j++)
            lp->p[i].sectors += lp->e[lp->p[i].first_ext + j].sectors;
    }
    free(t);
    return 0;
}

static int in_list(const char *list, const char *name) {
    if (!list || !*list) return 1;                          /* empty = all */
    size_t n = strlen(name);
    for (const char *s = list; *s; ) {
        const char *e = strchr(s, ','); size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == n && !strncmp(s, name, n)) return 1;
        if (!e) break;
        s = e + 1;
    }
    return 0;
}

/* flag value helper: "--delete 1" / "--delete 0" / bare "--delete" */
static int flag_val(int argc, char **argv, int *i) {
    if (*i + 1 < argc && (!strcmp(argv[*i + 1], "0") || !strcmp(argv[*i + 1], "1")))
        return argv[++*i][0] == '1';
    return 1;
}

static void delete_path(const char *path) {
    if (access(path, F_OK)) return;
    printf("> Deleting: %s\n", path); fflush(stdout);
    if (remove(path)) fprintf(stderr, "> Warning: cannot delete %s: %s\n", path, strerror(errno));
}

/* Slot handling. mode: "none" (default) = slot-a/unsuffixed partitions, suffix stripped (system_a -> system);
 * "a" / "b" = only that slot, suffix stripped; "both" = every partition, names kept as in the super.
 * Returns 1 if the partition is included and writes the output name into out. */
static int slot_pick(const char *mode, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    int sa = l > 2 && !strcmp(name + l - 2, "_a"), sb = l > 2 && !strcmp(name + l - 2, "_b");
    snprintf(out, n, "%s", name);
    if (!strcmp(mode, "both")) return 1;
    if (!sa && !sb) return 1;                                  /* slot-less partition */
    if (!strcmp(mode, "b") ? !sb : !sa) return 0;              /* "none" and "a" take slot a */
    out[l - 2] = 0;
    return 1;
}

static int extract_one(const char *path, int auto_unpack, int delete_src);

static int cmd_lpunpack(int argc, char **argv) {
    const char *parts = NULL, *file = NULL;
    int list = 0, del = 0, autox = 0; const char *slot = "none";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--partition") && i + 1 < argc) parts = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) slot = argv[++i];
        else if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (!strcmp(argv[i], "--auto")) autox = flag_val(argc, argv, &i);
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (strcmp(slot, "none") && strcmp(slot, "a") && strcmp(slot, "b") && strcmp(slot, "both")) {
        fprintf(stderr, "> Error: --slot must be none, a, b or both\n"); return 2;
    }
    if (!file) { fprintf(stderr, "usage: dna lpunpack [--list] [--slot none|a|b|both] [--partition a,b] [--delete 0|1] [--auto 0|1] super.img\n"); return 2; }

    img_t im; lp_t lp;
    if (img_open(&im, file) || lp_load(&im, &lp)) { fprintf(stderr, "> Error: %s is not a valid super image\n", file); img_close(&im); return 1; }

    if (list) {
        for (size_t i = 0; i < lp.np; i++) { char dn[64]; if (lp.p[i].sectors && slot_pick(slot, lp.p[i].name, dn, sizeof dn)) puts(dn); }
        img_close(&im); return 0;
    }

    char dir[4096]; snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, ".");

    int rc = 0, done = 0, auto_fail = 0;
    uint8_t *buf = malloc(1 << 20);
    for (size_t i = 0; i < lp.np; i++) {
        part_t *p = &lp.p[i];
        char dn[64];
        if (!p->sectors || !slot_pick(slot, p->name, dn, sizeof dn) || !in_list(parts, dn)) continue;
        char out[4400]; snprintf(out, sizeof out, "%s/%s.img", dir, dn);
        printf("> Extracting: %s\n", dn); fflush(stdout);
        FILE *o = fopen(out, "wb");
        if (!o) { fprintf(stderr, "> Error: cannot write %s\n", out); rc = 1; continue; }
        for (uint32_t j = 0; j < p->num_ext && !rc; j++) {
            ext_t *x = &lp.e[p->first_ext + j];
            uint64_t len = x->sectors * 512, off = x->data * 512;
            while (len) {
                size_t n = len > (1 << 20) ? (1 << 20) : (size_t)len;
                if (x->type == 1) memset(buf, 0, n);
                else if (!img_read(&im, off, buf, n)) { fprintf(stderr, "> Error: read failed in %s\n", p->name); rc = 1; break; }
                if (fwrite(buf, 1, n, o) != n) { fprintf(stderr, "> Error: write failed (disk full?)\n"); rc = 1; break; }
                off += n; len -= n;
            }
        }
        fclose(o);
        if (rc) { remove(out); break; }
        printf("> Done: %s\n", out); done++;
        if (autox) {
            printf("> Auto-unpacking: %s\n", dn); fflush(stdout);
            int erc = extract_one(out, 1, del);
            if (erc == EXTRACT_UNSUPPORTED) {
                fprintf(stderr, "> Warning: unsupported image type: %s\n", out);
                if (del) delete_path(out);
            } else if (erc) {
                fprintf(stderr, "> Warning: auto-unpack failed: %s\n", out);
                auto_fail = 1;
            } else if (del) {
                delete_path(out);
            }
        }
    }
    free(buf); img_close(&im);
    if (!rc && !done) { fprintf(stderr, "> Error: no matching partitions\n"); rc = 1; }
    if (!rc && del) delete_path(file);
    return rc ? rc : auto_fail;
}


/* ---------- generic helpers ---------- */

static int run(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        if (g_repack_quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
            }
        }
        execvp(argv[0], argv); fprintf(stderr, "> Error: cannot run %s: %s\n", argv[0], strerror(errno)); _exit(127);
    }
    int st; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Same as run(), but hides the tool's stdout (progress/banner). stderr stays visible.
 * In debug mode nothing is hidden; in normal repacking both streams are hidden. */
static int run_quiet(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        if (!g_debug) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                if (g_repack_quiet) dup2(fd, STDERR_FILENO);
                if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
            }
        }
        execvp(argv[0], argv); fprintf(stderr, "> Error: cannot run %s: %s\n", argv[0], strerror(errno)); _exit(127);
    }
    int st; waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int mkdir_p(const char *path) {
    char t[4096]; snprintf(t, sizeof t, "%s", path);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0755); *s = '/'; }
    return mkdir(t, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void base_name(const char *path, char *out, size_t n) {   /* file name without dir and .img */
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    snprintf(out, n, "%s", b);
    size_t l = strlen(out);
    if (l > 4 && !strcmp(out + l - 4, ".img")) out[l - 4] = 0;
}

static void part_name(const char *path, char *out, size_t n) {   /* system_a.img -> system */
    base_name(path, out, n);
    size_t l = strlen(out);
    if (l > 2 && (!strcmp(out + l - 2, "_a") || !strcmp(out + l - 2, "_b"))) out[l - 2] = 0;
}

static const char *tmp_dir(void) { const char *t = getenv("TMPDIR"); return t && *t ? t : "/tmp"; }

/* ---------- sparse -> raw ---------- */

static int sparse_to_raw(const char *src, const char *dst) {
    img_t im; if (img_open(&im, src)) return -1;
    FILE *o = fopen(dst, "wb"); if (!o) { img_close(&im); return -1; }
    uint8_t *buf = malloc(1 << 20); uint64_t off = 0; int rc = 0;
    while (off < im.size) {
        size_t n = im.size - off > (1 << 20) ? (1 << 20) : (size_t)(im.size - off);
        if (!img_read(&im, off, buf, n) || fwrite(buf, 1, n, o) != n) { rc = -1; break; }
        off += n;
    }
    free(buf); fclose(o); img_close(&im);
    if (rc) remove(dst);
    return rc;
}

/* ---------- native ext2/3/4 reader ---------- */

typedef struct {
    img_t *im; uint32_t bs, ipg, isz, dsz; uint64_t nblocks; uint8_t *gdt; uint32_t ngroups;
    FILE *cfg, *ctx; char pname[256]; char outroot[4096];
    uint32_t *seen_ino; char **seen_path; size_t nseen, capseen;
    uint64_t nfiles;
} efs_t;

typedef struct { uint8_t raw[1024]; uint16_t mode, uid, gid; uint32_t flags; uint64_t size, acl; } einode_t;

static int e_read(efs_t *e, uint64_t blk, void *buf, size_t n) { return img_read(e->im, blk * e->bs, buf, n); }

static int e_inode(efs_t *e, uint32_t ino, einode_t *in) {
    uint32_t g = (ino - 1) / e->ipg, i = (ino - 1) % e->ipg;
    if (g >= e->ngroups) return -1;
    const uint8_t *gd = e->gdt + (size_t)g * e->dsz;
    uint64_t it = le32(gd + 8); if (e->dsz >= 64) it |= (uint64_t)le32(gd + 0x28) << 32;
    size_t n = e->isz > sizeof in->raw ? sizeof in->raw : e->isz;
    if (!img_read(e->im, it * e->bs + (uint64_t)i * e->isz, in->raw, n)) return -1;
    const uint8_t *r = in->raw;
    in->mode = le16(r); in->flags = le32(r + 32);
    in->uid = le16(r + 2) | (e->isz > 128 ? (le16(r + 120) << 16) : 0);
    in->gid = le16(r + 24) | (e->isz > 128 ? (le16(r + 122) << 16) : 0);
    in->size = le32(r + 4) | ((uint64_t)le32(r + 108) << 32);
    in->acl = le32(r + 104) | ((uint64_t)le16(r + 118) << 32);
    return 0;
}

typedef struct { FILE *out; uint8_t *buf; uint64_t cap; efs_t *e; } wctx_t;

static int e_put(wctx_t *w, uint64_t lblk, uint64_t pblk, uint64_t len, int uninit) {
    efs_t *e = w->e; uint64_t bs = e->bs;
    if (uninit) return 0;
    uint8_t *tmp = malloc(1 << 20);
    for (uint64_t done = 0; done < len; ) {
        uint64_t nb = len - done; if (nb > (1 << 20) / bs) nb = (1 << 20) / bs; if (!nb) nb = 1;
        size_t n = nb * bs;
        if (!e_read(e, pblk + done, tmp, n)) { free(tmp); return -1; }
        uint64_t lo = (lblk + done) * bs;
        if (w->out) { fseeko(w->out, lo, SEEK_SET); if (fwrite(tmp, 1, n, w->out) != n) { free(tmp); return -1; } }
        else if (lo < w->cap) { size_t c = n; if (lo + c > w->cap) c = w->cap - lo; memcpy(w->buf + lo, tmp, c); }
        done += nb;
    }
    free(tmp); return 0;
}

static int e_walk_ext(efs_t *e, const uint8_t *node, size_t nlen, wctx_t *w, int depth) {
    if (nlen < 12 || le16(node) != 0xF30A) return -1;
    uint16_t ne = le16(node + 2), dep = le16(node + 6);
    if (depth > 6 || 12 + (size_t)ne * 12 > nlen) return -1;
    for (uint16_t i = 0; i < ne; i++) {
        const uint8_t *x = node + 12 + i * 12;
        if (dep == 0) {
            uint32_t lb = le32(x); uint32_t l = le16(x + 4); int un = 0;
            if (l > 32768) { l -= 32768; un = 1; }
            uint64_t pb = le32(x + 8) | ((uint64_t)le16(x + 6) << 32);
            if (e_put(w, lb, pb, l, un)) return -1;
        } else {
            uint64_t leaf = le32(x + 4) | ((uint64_t)le16(x + 8) << 32);
            uint8_t *b = malloc(e->bs);
            if (!e_read(e, leaf, b, e->bs) || e_walk_ext(e, b, e->bs, w, depth + 1)) { free(b); return -1; }
            free(b);
        }
    }
    return 0;
}

static int e_walk_map(efs_t *e, uint32_t blk, int level, uint64_t *lb, wctx_t *w) {
    if (level == 0) { if (blk) { if (e_put(w, *lb, blk, 1, 0)) return -1; } (*lb)++; return 0; }
    uint32_t per = e->bs / 4;
    if (!blk) { uint64_t span = 1; for (int i = 0; i < level; i++) span *= per; *lb += span; return 0; }
    uint8_t *b = malloc(e->bs);
    if (!e_read(e, blk, b, e->bs)) { free(b); return -1; }
    for (uint32_t i = 0; i < per; i++) if (e_walk_map(e, le32(b + i * 4), level - 1, lb, w)) { free(b); return -1; }
    free(b); return 0;
}

static int e_data(efs_t *e, einode_t *in, wctx_t *w) {
    const uint8_t *ib = in->raw + 40;
    if (in->flags & 0x80000) return e_walk_ext(e, ib, 60, w, 0);
    uint64_t lb = 0;
    for (int i = 0; i < 12; i++) if (e_walk_map(e, le32(ib + i * 4), 0, &lb, w)) return -1;
    for (int l = 1; l <= 3; l++) if (e_walk_map(e, le32(ib + (11 + l) * 4), l, &lb, w)) return -1;
    return 0;
}

static void esc_ctx(FILE *f, const char *s) {
    for (; *s; s++) { if (strchr(".+*?()[]{}^$|\\", *s)) fputc('\\', f); fputc(*s, f); }
}

static void e_xattr_scan(const uint8_t *first, const uint8_t *base, const uint8_t *end,
                         char *ctx, size_t cn, uint64_t *caps, int *hascap) {
    const uint8_t *p = first;
    while (p + 16 <= end && le32(p) != 0) {
        uint8_t nl = p[0], idx = p[1]; uint16_t vo = le16(p + 2); uint32_t vs = le32(p + 8);
        if (p + 16 + nl > end) break;
        if (idx == 6 && base + vo + vs <= end) {
            if (nl == 7 && !memcmp(p + 16, "selinux", 7)) {
                size_t n = vs < cn - 1 ? vs : cn - 1; memcpy(ctx, base + vo, n); ctx[n] = 0;
                while (n && ctx[n - 1] == 0) n--; ctx[n] = 0;
            } else if (nl == 10 && !memcmp(p + 16, "capability", 10) && vs >= 12) {
                const uint8_t *v = base + vo; *caps = le32(v + 4);
                if (vs >= 20) *caps |= (uint64_t)le32(v + 12) << 32;
                *hascap = 1;
            }
        }
        p += (16 + nl + 3) & ~3u;
    }
}

static void e_xattrs(efs_t *e, einode_t *in, char *ctx, size_t cn, uint64_t *caps, int *hascap) {
    ctx[0] = 0; *caps = 0; *hascap = 0;
    if (e->isz > 128) {
        uint16_t ex = le16(in->raw + 128); size_t off = 128 + ex;
        if (off + 4 < e->isz && off + 4 <= sizeof in->raw && le32(in->raw + off) == 0xEA020000u) {
            size_t lim = e->isz < sizeof in->raw ? e->isz : sizeof in->raw;
            e_xattr_scan(in->raw + off + 4, in->raw + off + 4, in->raw + lim, ctx, cn, caps, hascap);
        }
    }
    if (in->acl && !ctx[0]) {
        uint8_t *b = malloc(e->bs);
        if (e_read(e, in->acl, b, e->bs) && le32(b) == 0xEA020000u) e_xattr_scan(b + 32, b, b + e->bs, ctx, cn, caps, hascap);
        free(b);
    }
}

static void emit(efs_t *e, const char *rel, einode_t *in, int is_dir) {
    char ctx[256]; uint64_t caps; int hc;
    e_xattrs(e, in, ctx, sizeof ctx, &caps, &hc);
    fprintf(e->cfg, "%s/%s%s %u %u %04o", e->pname, rel, is_dir && !*rel ? "" : "", in->uid, in->gid, in->mode & 07777);
    if (hc) fprintf(e->cfg, " capabilities=0x%llX", (unsigned long long)caps);
    fputc('\n', e->cfg);
    if (ctx[0]) { fputc('/', e->ctx); fputs(e->pname, e->ctx); fputc('/', e->ctx); esc_ctx(e->ctx, rel); fprintf(e->ctx, " %s\n", ctx); }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int e_dir(efs_t *e, uint32_t ino, const char *rel, int depth);

static int e_node(efs_t *e, uint32_t ino, const char *rel, int depth) {
    einode_t in; if (e_inode(e, ino, &in)) return -1;
    char out[8192]; snprintf(out, sizeof out, "%s/%s", e->outroot, rel);
    uint16_t fmt = in.mode & 0xF000;
    if (fmt == 0x4000) {
        if (*rel) { mkdir(out, 0755); emit(e, rel, &in, 1); }
        return e_dir(e, ino, rel, depth + 1);
    }
    for (size_t i = 0; i < e->nseen; i++) if (e->seen_ino[i] == ino) { unlink(out); if (link(e->seen_path[i], out)) { /* fall back to a copy later */ } emit(e, rel, &in, 0); return 0; }
    if (fmt == 0x8000) {
        FILE *f = fopen(out, "wb"); if (!f) return -1;
        wctx_t w = { f, NULL, 0, e };
        int rc = e_data(e, &in, &w);
        if (!rc && ftruncate(fileno(f), (off_t)in.size)) rc = -1;
        fclose(f);
        chmod(out, (in.mode & 0111) ? 0755 : 0644);
        if (rc) return -1;
    } else if (fmt == 0xA000) {
        char tgt[4096]; size_t n = in.size < sizeof tgt - 1 ? in.size : sizeof tgt - 1;
        if (in.size < 60 && !(in.flags & 0x80000)) memcpy(tgt, in.raw + 40, n);
        else { wctx_t w = { NULL, (uint8_t *)tgt, n, e }; if (e_data(e, &in, &w)) return -1; }
        tgt[n] = 0; unlink(out); if (symlink(tgt, out)) return -1;
    } /* devices, fifos, sockets: config entry only */
    emit(e, rel, &in, 0);
    if (fmt == 0x8000 && e->nseen == e->capseen) {
        e->capseen = e->capseen ? e->capseen * 2 : 1024;
        e->seen_ino = realloc(e->seen_ino, e->capseen * sizeof *e->seen_ino);
        e->seen_path = realloc(e->seen_path, e->capseen * sizeof *e->seen_path);
    }
    if (fmt == 0x8000 && le16(in.raw + 26) > 1) { e->seen_ino[e->nseen] = ino; e->seen_path[e->nseen++] = strdup(out); }
    e->nfiles++;
    return 0;
}

static int e_dir(efs_t *e, uint32_t ino, const char *rel, int depth) {
    if (depth > 64) return -1;
    einode_t in; if (e_inode(e, ino, &in)) return -1;
    uint8_t *d = calloc(1, in.size ? in.size : 1);
    wctx_t w = { NULL, d, in.size, e };
    if (e_data(e, &in, &w)) { free(d); return -1; }
    typedef struct { char *name; uint32_t ino; } de_t; de_t *v = NULL; size_t nv = 0, cv = 0;
    for (uint64_t o = 0; o + 8 <= in.size; ) {
        uint32_t ci = le32(d + o); uint16_t rl = le16(d + o + 4); uint8_t nl = d[o + 6];
        if (rl < 8 || o + rl > in.size) break;
        if (ci && nl && !(nl == 1 && d[o + 8] == '.') && !(nl == 2 && d[o + 8] == '.' && d[o + 9] == '.')) {
            if (nv == cv) { cv = cv ? cv * 2 : 64; v = realloc(v, cv * sizeof *v); }
            v[nv].name = strndup((char *)d + o + 8, nl); v[nv].ino = ci; nv++;
        }
        o += rl;
    }
    free(d);
    char **names = malloc((nv ? nv : 1) * sizeof *names);
    for (size_t i = 0; i < nv; i++) names[i] = v[i].name;
    qsort(names, nv, sizeof *names, cmp_str);
    int rc = 0;
    for (size_t i = 0; i < nv && !rc; i++) {
        uint32_t ci = 0; for (size_t j = 0; j < nv; j++) if (v[j].name == names[i]) { ci = v[j].ino; break; }
        char child[8192]; snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", names[i]);
        if (e_node(e, ci, child, depth)) { fprintf(stderr, "> Warning: failed to extract /%s\n", child); }
    }
    for (size_t i = 0; i < nv; i++) free(v[i].name);
    free(v); free(names);
    return rc;
}

static int extract_ext(const char *img, const char *dro, const char *pname) {
    img_t im; if (img_open(&im, img)) return -1;
    uint8_t sb[1024];
    if (!img_read(&im, 1024, sb, 1024) || le16(sb + 56) != 0xEF53) { img_close(&im); return -1; }
    efs_t e; memset(&e, 0, sizeof e);
    e.im = &im; e.bs = 1024u << le32(sb + 24); e.ipg = le32(sb + 40);
    e.isz = le16(sb + 88) ? le16(sb + 88) : 128;
    uint32_t inc = le32(sb + 96);
    e.dsz = (inc & 0x80) ? le16(sb + 254) : 32; if (e.dsz < 32) e.dsz = 32;
    e.nblocks = le32(sb + 4) | ((inc & 0x80) ? (uint64_t)le32(sb + 0x150) << 32 : 0);
    uint32_t bpg = le32(sb + 32); e.ngroups = (uint32_t)((e.nblocks + bpg - 1) / bpg);
    if (inc & 0x10000000) fprintf(stderr, "> Warning: inline_data feature is not supported\n");
    e.gdt = malloc((size_t)e.ngroups * e.dsz);
    if (!img_read(&im, (uint64_t)(e.bs == 1024 ? 2 : 1) * e.bs, e.gdt, (size_t)e.ngroups * e.dsz)) { img_close(&im); return -1; }
    snprintf(e.pname, sizeof e.pname, "%s", pname);
    snprintf(e.outroot, sizeof e.outroot, "%s/%s", dro, pname);
    char cdir[4200], p1[4400], p2[4400], p3[4400];
    snprintf(cdir, sizeof cdir, "%s/config", dro); mkdir_p(cdir); mkdir_p(e.outroot);
    snprintf(p1, sizeof p1, "%s/%s_fs_config", cdir, pname);
    snprintf(p2, sizeof p2, "%s/%s_file_contexts", cdir, pname);
    snprintf(p3, sizeof p3, "%s/%s_fs_options", cdir, pname);
    e.cfg = fopen(p1, "wb"); e.ctx = fopen(p2, "wb");
    if (!e.cfg || !e.ctx) { img_close(&im); return -1; }
    einode_t root; if (e_inode(&e, 2, &root)) { img_close(&im); return -1; }
    char rc_[256]; uint64_t cp; int hc;
    e_xattrs(&e, &root, rc_, sizeof rc_, &cp, &hc);
    fprintf(e.cfg, "/ %u %u %04o\n%s/ %u %u %04o\n", root.uid, root.gid, root.mode & 07777, pname, root.uid, root.gid, root.mode & 07777);
    if (rc_[0]) fprintf(e.ctx, "/ %s\n/%s %s\n/%s/ %s\n", rc_, pname, rc_, pname, rc_);
    int rc = e_dir(&e, 2, "", 0);
    fclose(e.cfg); fclose(e.ctx);
    FILE *o = fopen(p3, "wb");
    if (o) {
        char uu[40]; const uint8_t *u = sb + 104;
        snprintf(uu, sizeof uu, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
        fprintf(o, "fs_type=ext4\nblock_size=%u\nblocks=%llu\ninode_size=%u\ninodes=%u\nuuid=%s\nmount_point=/%s\n",
                e.bs, (unsigned long long)e.nblocks, e.isz, le32(sb), uu, pname);
        fclose(o);
    }
    {   /* <name>_info: original image size in bytes (used by the "Original size" repack mode) */
        char p4[4400]; snprintf(p4, sizeof p4, "%s/%s_info", cdir, pname);
        FILE *fi = fopen(p4, "wb");
        if (fi) { fprintf(fi, "%llu\n", (unsigned long long)(e.nblocks * e.bs)); fclose(fi); }
    }
    for (size_t i = 0; i < e.nseen; i++) free(e.seen_path[i]);
    free(e.seen_ino); free(e.seen_path); free(e.gdt); img_close(&im);
    return rc;
}

/* ---------- .new.dat (+ transfer list) -> raw img ---------- */

static int dat_to_img(const char *dat, char *outimg, size_t on) {
    char base[4096]; snprintf(base, sizeof base, "%s", dat);
    char *p = strstr(base, ".new.dat"); if (!p) return -1; *p = 0;
    char tl[4200]; snprintf(tl, sizeof tl, "%s.transfer.list", base);
    FILE *t = fopen(tl, "r"); if (!t) { fprintf(stderr, "> Error: %s not found\n", tl); return -1; }
    FILE *d = fopen(dat, "rb"); if (!d) { fclose(t); return -1; }
    snprintf(outimg, on, "%s.img", base);
    FILE *o = fopen(outimg, "wb"); if (!o) { fclose(t); fclose(d); return -1; }
    char line[8192]; int ver = 0; unsigned long long total = 0; int rc = 0;
    if (!fgets(line, sizeof line, t)) rc = -1; else ver = atoi(line);
    if (!rc && (!fgets(line, sizeof line, t))) rc = -1; else if (!rc) total = strtoull(line, NULL, 10);
    if (ver >= 2) { if (!fgets(line, sizeof line, t) || !fgets(line, sizeof line, t)) rc = -1; }
    uint8_t *buf = malloc(4096);
    while (!rc && fgets(line, sizeof line, t)) {
        char cmd[32]; char *rest = strchr(line, ' '); if (!rest) continue;
        size_t cl = (size_t)(rest - line); if (cl >= sizeof cmd) continue;
        memcpy(cmd, line, cl); cmd[cl] = 0;
        if (strcmp(cmd, "new") && strcmp(cmd, "zero") && strcmp(cmd, "erase")) {
            fprintf(stderr, "> Error: unsupported transfer command '%s' (incremental OTA)\n", cmd); rc = -1; break;
        }
        char *s = rest + 1; long cnt = strtol(s, &s, 10);
        for (long i = 0; i + 1 < cnt && !rc; i += 2) {
            s++; unsigned long long a = strtoull(s, &s, 10); s++; unsigned long long b = strtoull(s, &s, 10);
            if (!strcmp(cmd, "erase")) continue;
            fseeko(o, (off_t)(a * 4096), SEEK_SET);
            for (unsigned long long blk = a; blk < b; blk++) {
                if (!strcmp(cmd, "new")) { if (fread(buf, 1, 4096, d) != 4096) { rc = -1; break; } }
                else memset(buf, 0, 4096);
                if (fwrite(buf, 1, 4096, o) != 4096) { rc = -1; break; }
            }
        }
    }
    free(buf);
    if (!rc && ftruncate(fileno(o), (off_t)(total * 4096))) rc = -1;
    fclose(o); fclose(d); fclose(t);
    if (rc) remove(outimg);
    return rc;
}

/* ---------- extract command ---------- */

static int extract_one(const char *path, int auto_unpack, int delete_src);

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"); if (!a) return -1;
    FILE *b = fopen(dst, "wb"); if (!b) { fclose(a); return -1; }
    char *buf = malloc(1 << 20); size_t n; int rc = 0;
    while ((n = fread(buf, 1, 1 << 20, a)) > 0) if (fwrite(buf, 1, n, b) != n) { rc = -1; break; }
    free(buf); fclose(a);
    if (fclose(b)) rc = -1;
    if (rc) remove(dst);
    return rc;
}

static const char *fs_label(const char *t) {
    return !strcmp(t, "ext") ? "ext4" : t;
}

static int extract_raw_by_type(const char *img, const char *name, const char *pname, const char *dro) {
    const char *t = detect_type(img);
    char dest[4400];
    (void)name;
    if (!strcmp(t, "ext")) return extract_ext(img, dro, pname);
    if (!strcmp(t, "erofs")) {
        /* extract.erofs derives the partition name from the file name: give it a clean one */
        char lnk[4400]; snprintf(lnk, sizeof lnk, "%s/dna_%d_%s.img", tmp_dir(), (int)getpid(), pname);
        char dd[4400]; snprintf(dd, sizeof dd, "%s/dna_%d", tmp_dir(), (int)getpid()); mkdir_p(dd);
        snprintf(lnk, sizeof lnk, "%s/%s.img", dd, pname);
        char abs[4400];
        if (img[0] == '/') snprintf(abs, sizeof abs, "%s", img);
        else { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) return -1; snprintf(abs, sizeof abs, "%s/%s", cwd, img); }
        unlink(lnk); if (symlink(abs, lnk)) return -1;
        char *a[] = { "extract.erofs", "-i", lnk, "-x", "-f", "-s", "-o", (char *)dro, NULL };
        int rc = run_quiet(a);
        unlink(lnk); rmdir(dd);
        return rc;
    }
    if (!strcmp(t, "f2fs")) {
        snprintf(dest, sizeof dest, "%s/%s", dro, pname); mkdir_p(dest);
        char *a[] = { "extract.f2fs", (char *)img, dest, NULL };
        return run(a);
    }
    if (!strcmp(t, "boot") || !strcmp(t, "vendor_boot")) {
        snprintf(dest, sizeof dest, "%s/%s", dro, pname); mkdir_p(dest);
        char abs[4400];
        if (img[0] == '/') snprintf(abs, sizeof abs, "%s", img);
        else { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) return -1; snprintf(abs, sizeof abs, "%s/%s", cwd, img); }
        char cwd0[2048]; if (!getcwd(cwd0, sizeof cwd0)) return -1;
        if (chdir(dest)) return -1;
        char *a[] = { "magiskboot", "unpack", "-h", abs, NULL };
        int rc = run(a);
        if (chdir(cwd0)) return -1;
        if (!rc) {   /* keep the original next to the unpacked parts: repack needs it */
            char keep[4700]; snprintf(keep, sizeof keep, "%s/%s.img", dest, pname);
            if (copy_file(abs, keep)) fprintf(stderr, "> Warning: cannot save original image to %s\n", keep);
            else printf("> Original saved: %s\n", keep);
        }
        return rc;
    }
    fprintf(stderr, "> Unsupported image type '%s': %s\n", t, name);
    return EXTRACT_UNSUPPORTED;
}

static int extract_one(const char *path, int auto_unpack, int delete_src) {
    const char *dro = getenv("DNA_DRO");
    if (!dro || !*dro) { fprintf(stderr, "> Error: project is not selected (DNA_DRO is empty)\n"); return 1; }
    char full[4400]; snprintf(full, sizeof full, "%s", path);
    if (access(full, R_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), path);
    if (access(full, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", path); return 1; }
    char name[512], pname[512];
    base_name(full, name, sizeof name); part_name(full, pname, sizeof pname);
    size_t fl = strlen(full);
    int is_img = fl > 4 && !strcmp(full + fl - 4, ".img");
    int rc;

    if (fl > 3 && !strcmp(full + fl - 3, ".br")) {                       /* x.new.dat.br -> x.new.dat -> x.img */
        char dat[4400]; snprintf(dat, sizeof dat, "%s", full); dat[fl - 3] = 0;
        printf("> Decompressing brotli: %s\n", full); fflush(stdout);
        char *a[] = { "brotli", "-d", "-f", full, "-o", dat, NULL };
        if ((rc = run(a))) return rc;
        char img[4400]; rc = dat_to_img(dat, img, sizeof img); remove(dat);
        if (!rc) printf("> Done: %s\n", img);
        return rc;
    }
    if (strstr(full, ".new.dat")) {
        char img[4400]; printf("> Converting dat to img: %s\n", full);
        rc = dat_to_img(full, img, sizeof img);
        if (!rc) printf("> Done: %s\n", img);
        return rc;
    }

    char *la[6]; int n = 0;
    la[n++] = (char *)full;
    if (auto_unpack) { la[n++] = "--auto"; la[n++] = "1"; }
    if (delete_src) { la[n++] = "--delete"; la[n++] = "1"; }
    la[n] = NULL;
    const char *t = detect_type(full);
    if (is_img) {
        const char *fn = strrchr(full, '/'); fn = fn ? fn + 1 : full;
        printf("Unpacking: %s <%s>\n", fn, fs_label(t)); fflush(stdout);
    }
    if (!strcmp(t, "super")) rc = cmd_lpunpack(n, la);
    else if (!strcmp(t, "sparse")) {
        img_t pi; lp_t pl; int is_super = !img_open(&pi, full) && !lp_load(&pi, &pl); img_close(&pi);
        if (is_super) rc = cmd_lpunpack(n, la);
        else {
            char raw[4400]; snprintf(raw, sizeof raw, "%s/dna_%d_%s.raw", tmp_dir(), (int)getpid(), name);
            printf("> Converting sparse to raw: %s\n", name); fflush(stdout);
            if (sparse_to_raw(full, raw)) { fprintf(stderr, "> Error: sparse conversion failed\n"); rc = 1; }
            else { rc = extract_raw_by_type(raw, name, pname, dro); remove(raw); }
        }
    } else rc = extract_raw_by_type(full, name, pname, dro);
    if (!rc && is_img) printf("%s unpacked successfully\n", pname);
    return rc;
}

static int cmd_extract(int argc, char **argv) {
    int del = 0, autox = 0, any = 0, fail = 0;
    char *files[256]; int nf = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (!strcmp(argv[i], "--auto")) autox = flag_val(argc, argv, &i);
        else if (argv[i][0] == '-') continue;
        else {
            for (char *tok = strtok(argv[i], ", \t\n"); tok && nf < 256; tok = strtok(NULL, ", \t\n")) files[nf++] = tok;
        }
    }
    if (!nf) { fprintf(stderr, "usage: dna extract <files...> [--delete 0|1] [--auto 0|1]\n"); return 2; }
    for (int i = 0; i < nf; i++) {
        any = 1;
        char full[4400]; snprintf(full, sizeof full, "%s", files[i]);
        if (access(full, F_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), files[i]);
        size_t nl = strlen(files[i]);
        int is_img = nl > 4 && !strcmp(files[i] + nl - 4, ".img");
        if (!is_img) { printf("> Unpacking: %s\n", files[i]); fflush(stdout); }
        int rc = extract_one(full, autox, del);
        if (rc == EXTRACT_UNSUPPORTED) {
            fprintf(stderr, "> Warning: unsupported image type: %s\n", files[i]);
            if (del) delete_path(full);
            continue;
        }
        if (rc) { fprintf(stderr, "> Failed: %s\n", files[i]); fail = 1; continue; }
        if (!is_img) printf("> Done: %s\n", files[i]);
        if (del) delete_path(full);
    }
    return any && !fail ? 0 : 1;
}


/* ---------- native raw -> sparse writer ---------- */

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

#define SPARSE_BS 4096u
#define SPARSE_DATA_CHUNK_BLOCKS 16384u
#define SPARSE_ZERO_CHUNK_BLOCKS 0x7FFFFFFu

typedef struct {
    FILE *o;
    uint64_t run_start;
    uint32_t run_len;
    int run_type;
    uint8_t *data;
    uint32_t data_len;
    uint32_t chunks;
    int err;
} spw_t;

static int spw_write(spw_t *w, const void *p, size_t n) {
    if (w->err) return -1;
    if (n && fwrite(p, 1, n, w->o) != n) { w->err = 1; return -1; }
    return 0;
}

static int spw_data_chunk(spw_t *w, uint32_t blocks) {
    uint8_t ch[12] = {0xC1, 0xCA};
    uint64_t total = 12ull + (uint64_t)blocks * SPARSE_BS;
    if (total > UINT32_MAX) { w->err = 1; return -1; }
    uint32_t t = (uint32_t)total;
    memcpy(ch + 4, &blocks, 4); memcpy(ch + 8, &t, 4);
    if (spw_write(w, ch, 12)) return -1;
    if (spw_write(w, w->data, (size_t)blocks * SPARSE_BS)) return -1;
    w->chunks++;
    return 0;
}

static int spw_zero_chunk(spw_t *w, uint32_t blocks) {
    uint8_t ch[12] = {0xC3, 0xCA};
    uint32_t t = 12;
    memcpy(ch + 4, &blocks, 4); memcpy(ch + 8, &t, 4);
    if (spw_write(w, ch, 12)) return -1;
    w->chunks++;
    return 0;
}

static int spw_flush(spw_t *w) {
    if (!w->run_len) { w->run_type = -1; return 0; }
    if (w->run_type == 1) {
        uint32_t left = w->run_len;
        uint32_t off = 0;
        while (left) {
            uint32_t n = left > SPARSE_DATA_CHUNK_BLOCKS ? SPARSE_DATA_CHUNK_BLOCKS : left;
            if (spw_data_chunk(w, n)) return -1;
            off += n; left -= n;
        }
        (void)off;
    } else {
        uint32_t left = w->run_len;
        while (left) {
            uint32_t n = left > SPARSE_ZERO_CHUNK_BLOCKS ? SPARSE_ZERO_CHUNK_BLOCKS : left;
            if (spw_zero_chunk(w, n)) return -1;
            left -= n;
        }
    }
    w->run_len = 0;
    w->data_len = 0;
    w->run_type = -1;
    return 0;
}

static int spw_add(spw_t *w, int zero, const uint8_t *blk) {
    int t = zero ? 0 : 1;
    if (w->run_type != t) {
        if (spw_flush(w)) return -1;
        w->run_type = t;
    }
    if (zero) {
        if (w->run_len == SPARSE_ZERO_CHUNK_BLOCKS && spw_flush(w)) return -1;
        w->run_start += w->run_len;
        w->run_len++;
    } else {
        if (w->run_len == SPARSE_DATA_CHUNK_BLOCKS && spw_flush(w)) return -1;
        if (w->run_type != 1) w->run_type = 1;
        memcpy(w->data + (size_t)w->data_len * SPARSE_BS, blk, SPARSE_BS);
        w->data_len++;
        w->run_len++;
    }
    return 0;
}

static int read_at_full(int fd, uint8_t *buf, size_t n, uint64_t off, uint64_t size) {
    size_t done = 0;
    if (off >= size) { memset(buf, 0, n); return 0; }
    uint64_t avail = size - off;
    if (avail < n) {
        memset(buf + avail, 0, n - (size_t)avail);
        n = (size_t)avail;
    }
    while (done < n) {
        ssize_t r = pread(fd, buf + done, n - done, (off_t)(off + done));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { memset(buf + done, 0, n - done); break; }
        done += (size_t)r;
    }
    return 0;
}

static int sparse_feed_data(spw_t *w, int fd, uint64_t start_blk, uint64_t nblk, uint64_t size, uint8_t *buf, const uint8_t *zero) {
    const uint64_t per = (4u << 20) / SPARSE_BS;
    while (nblk) {
        uint64_t n = nblk > per ? per : nblk;
        uint64_t off = start_blk * SPARSE_BS;
        if (read_at_full(fd, buf, (size_t)n * SPARSE_BS, off, size)) return -1;
        for (uint64_t i = 0; i < n; i++) {
            const uint8_t *blk = buf + i * SPARSE_BS;
            int z = !memcmp(blk, zero, SPARSE_BS);
            if (spw_add(w, z, blk)) return -1;
        }
        start_blk += n; nblk -= n;
    }
    return 0;
}

static int raw_to_sparse(const char *src, const char *dst) {
    int fd = open(src, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return -1; }
    uint64_t size = (uint64_t)st.st_size;
    uint64_t nblk = (size + SPARSE_BS - 1) / SPARSE_BS;
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    FILE *o = fopen(dst, "wb");
    if (!o) { close(fd); return -1; }
    setvbuf(o, NULL, _IOFBF, 1u << 20);
    uint8_t hdr[28] = {0};
    int rc = fwrite(hdr, 1, 28, o) == 28 ? 0 : -1;
    uint8_t *buf = malloc(4u << 20), *zero = calloc(1, SPARSE_BS);
    uint8_t *run = malloc((size_t)SPARSE_DATA_CHUNK_BLOCKS * SPARSE_BS);
    spw_t w; memset(&w, 0, sizeof w);
    w.o = o; w.data = run; w.run_type = -1;
    if (!buf || !zero || !run) rc = -1;
    uint64_t next = 0;
    int seek_ok = 1;
    while (!rc && next < nblk) {
        if (seek_ok) {
            off_t d = lseek(fd, (off_t)(next * SPARSE_BS), SEEK_DATA);
            if (d < 0) {
                if (errno == ENXIO) {
                    if (spw_flush(&w)) { rc = -1; break; }
                    uint64_t rem = nblk - next;
                    while (rem && !rc) {
                        uint32_t n = rem > SPARSE_ZERO_CHUNK_BLOCKS ? SPARSE_ZERO_CHUNK_BLOCKS : (uint32_t)rem;
                        rc = spw_zero_chunk(&w, n);
                        rem -= n;
                    }
                    next = nblk;
                    break;
                }
                seek_ok = 0;
            } else {
                uint64_t db = (uint64_t)d / SPARSE_BS;
                if (db > next) {
                    if (spw_flush(&w)) { rc = -1; break; }
                    uint64_t z = db - next;
                    while (z && !rc) {
                        uint32_t n = z > SPARSE_ZERO_CHUNK_BLOCKS ? SPARSE_ZERO_CHUNK_BLOCKS : (uint32_t)z;
                        rc = spw_zero_chunk(&w, n);
                        z -= n;
                    }
                    next = db;
                    if (next >= nblk) break;
                }
                off_t h = lseek(fd, (off_t)(next * SPARSE_BS), SEEK_HOLE);
                uint64_t hb = h < 0 ? nblk : ((uint64_t)h + SPARSE_BS - 1) / SPARSE_BS;
                if (hb > nblk) hb = nblk;
                if (hb <= next) hb = nblk;
                rc = sparse_feed_data(&w, fd, next, hb - next, size, buf, zero);
                next = hb;
                continue;
            }
        }
        rc = sparse_feed_data(&w, fd, next, nblk - next, size, buf, zero);
        next = nblk;
    }
    if (!rc) rc = spw_flush(&w);
    if (!rc) {
        uint32_t magic = 0xED26FF3A, tb = (uint32_t)nblk, bsz = SPARSE_BS, zero32 = 0;
        uint16_t maj = 1, min = 0, fh = 28, ch = 12;
        rewind(o);
        fwrite(&magic, 4, 1, o); fwrite(&maj, 2, 1, o); fwrite(&min, 2, 1, o); fwrite(&fh, 2, 1, o);
        fwrite(&ch, 2, 1, o); fwrite(&bsz, 4, 1, o); fwrite(&tb, 4, 1, o); fwrite(&w.chunks, 4, 1, o); fwrite(&zero32, 4, 1, o);
    }
    free(buf); free(zero); free(run); close(fd);
    if (fclose(o)) rc = -1;
    if (rc) remove(dst);
    return rc;
}

static int blk_zero(const uint8_t *p) {                  /* all 4096 bytes are zero */
    const uint64_t *q = (const uint64_t *)(const void *)p;
    for (int i = 0; i < 512; i++) if (q[i]) return 0;
    return 1;
}

/* ---------- img -> .new.dat / .transfer.list / .patch.dat (+ brotli) ---------- */

typedef struct { uint64_t *v; size_t n, cap; } ranges_t;       /* flat [start, end) pairs */

static void rg_add(ranges_t *r, uint64_t blk) {
    if (r->n && r->v[r->n - 1] == blk) { r->v[r->n - 1] = blk + 1; return; }   /* extend the last range */
    if (r->n + 2 > r->cap) { r->cap = r->cap ? r->cap * 2 : 64; r->v = realloc(r->v, r->cap * sizeof *r->v); }
    r->v[r->n++] = blk; r->v[r->n++] = blk + 1;
}

static void rg_print(FILE *f, const char *cmd, const ranges_t *r) {
    fprintf(f, "%s %zu", cmd, r->n);
    for (size_t i = 0; i < r->n; i++) fprintf(f, ",%llu", (unsigned long long)r->v[i]);
    fputc('\n', f);
}

/* Layout follows the original: block 0 ("clobbered") is always sent first, then the zero blocks,
 * then the remaining data; new.dat holds block 0 followed by the other data blocks in order. */
static int img_to_dat(const char *img, const char *outdir, const char *name, int br_level, int make_br) {
    img_t im; if (img_open(&im, img)) { fprintf(stderr, "> Error: cannot read %s\n", img); return -1; }
    char datp[4600], tlp[4600], pdp[4600];
    snprintf(datp, sizeof datp, "%s/%s.new.dat", outdir, name); snprintf(tlp, sizeof tlp, "%s/%s.transfer.list", outdir, name);
    snprintf(pdp, sizeof pdp, "%s/%s.patch.dat", outdir, name);
    FILE *d = fopen(datp, "wb"); if (!d) { img_close(&im); return -1; }
    uint64_t nblk = (im.size + 4095) / 4096; ranges_t zero = {0}, data = {0};
    uint8_t *buf = malloc(1 << 20); int rc = 0;
    for (uint64_t b = 0; b < nblk && !rc; ) {
        uint64_t nb = nblk - b; if (nb > 256) nb = 256;
        memset(buf, 0, nb * 4096);
        uint64_t off = b * 4096, have = im.size > off ? im.size - off : 0; if (have > nb * 4096) have = nb * 4096;
        if (have && !img_read(&im, off, buf, have)) { rc = -1; break; }
        for (uint64_t i = 0; i < nb; i++) {
            uint64_t blk = b + i; const uint8_t *p = buf + i * 4096;
            if (blk == 0) { if (fwrite(p, 1, 4096, d) != 4096) rc = -1; continue; }
            if (blk_zero(p)) rg_add(&zero, blk);
            else { rg_add(&data, blk); if (fwrite(p, 1, 4096, d) != 4096) rc = -1; }
        }
        b += nb;
    }
    free(buf); img_close(&im);
    if (fclose(d)) rc = -1;
    if (!rc) {
        FILE *t = fopen(tlp, "w"), *pd = fopen(pdp, "wb");
        if (!t || !pd) rc = -1;
        else {
            fprintf(t, "4\n%llu\n0\n0\nnew 2,0,1\n", (unsigned long long)nblk);
            if (zero.n) rg_print(t, "zero", &zero);
            if (data.n) rg_print(t, "new", &data);
        }
        if (t && fclose(t)) rc = -1;
        if (pd) fclose(pd);
    }
    free(zero.v); free(data.v);
    if (!rc && make_br) {
        char br[4700], q[16]; snprintf(br, sizeof br, "%s.br", datp); snprintf(q, sizeof q, "%d", br_level < 0 ? 3 : br_level > 11 ? 11 : br_level);
        printf("> Compressing with brotli (level %s): %s.new.dat\n", q, name); fflush(stdout);
        char *a[] = { "brotli", "-q", q, "-f", "-o", br, datp, NULL };
        rc = run(a);
        if (!rc) remove(datp);
    }
    if (rc) { remove(datp); remove(tlp); remove(pdp); }
    return rc;
}

/* dna convert <img[,img]> --delete 0|1 --type 0(DAT)|1(BR) --br LEVEL */
static int cmd_convert(int argc, char **argv) {
    int del = 0, type = 0, lvl = 3, nn = 0, fail = 0; char *names[256];
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (!strcmp(argv[i], "--type") && i + 1 < argc) type = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--br") && i + 1 < argc) lvl = atoi(argv[++i]);
        else if (argv[i][0] != '-') for (char *t = strtok(argv[i], ", \t\n"); t && nn < 256; t = strtok(NULL, ", \t\n")) names[nn++] = t;
    }
    if (!nn) { fprintf(stderr, "usage: dna convert <img[,img]> [--delete 0|1] [--type 0|1] [--br N]\n"); return 2; }
    const char *pro = getenv("DNA_PRO"); char od[4400];
    snprintf(od, sizeof od, "%s/out", pro && *pro ? pro : ".");
    if (mkdir_p(od)) { fprintf(stderr, "> Error: cannot create %s\n", od); return 1; }
    for (int i = 0; i < nn; i++) {
        char full[4400]; snprintf(full, sizeof full, "%s", names[i]);
        if (access(full, R_OK) && pro && *pro) snprintf(full, sizeof full, "%s/%s", pro, names[i]);
        if (access(full, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", names[i]); fail = 1; continue; }
        char name[512]; base_name(full, name, sizeof name);
        printf("> Converting: %s -> %s\n", names[i], type ? "br" : "dat"); fflush(stdout);
        if (img_to_dat(full, od, name, lvl, type == 1)) { fprintf(stderr, "> Failed: %s\n", names[i]); fail = 1; continue; }
        printf("> Done: %s/%s.%s\n", od, name, type ? "new.dat.br" : "new.dat");
        if (del) { printf("> Deleting: %s\n", names[i]); remove(full); }
    }
    return fail ? 1 : 0;
}

/* ---------- repack ---------- */

#define REPACK_SUFFIX ""   /* built images keep the name they were unpacked with; they go to $DNA_PRO/out/ */

typedef struct { uint64_t blocks, entries; } tstat_t;

static void tree_stat(const char *dir, tstat_t *st, int depth) {
    DIR *d = opendir(dir); if (!d || depth > 64) { if (d) closedir(d); return; }
    struct dirent *de; uint64_t n = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[8192]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat sb; if (lstat(p, &sb)) continue;
        n++; st->entries++;
        if (S_ISDIR(sb.st_mode)) tree_stat(p, st, depth + 1);
        else if (S_ISREG(sb.st_mode)) st->blocks += ((uint64_t)sb.st_size + 4095) / 4096 + (sb.st_size > (64 << 20) ? (uint64_t)sb.st_size / (128u << 20) + 1 : 0);
        else if (S_ISLNK(sb.st_mode) && sb.st_size >= 60) st->blocks += 1;
    }
    closedir(d);
    st->blocks += 1 + n / 60;                                  /* the directory's own blocks */
}

static uint64_t read_u64_file(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    unsigned long long v = 0; if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); return v;
}

static int read_opt(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[512]; size_t kl = strlen(key); int ok = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') { snprintf(out, n, "%s", line + kl + 1); out[strcspn(out, "\r\n")] = 0; ok = 1; break; }
    }
    fclose(f); return ok;
}

typedef struct {
    int rw, autosize, sparse, out_kind, brlvl, erofslvl, shrink, delavb, hal_fallback;
    char mode[16];
    int erofs_workers;
} ropts_t;

static void avb_strip_line(char *line) {
    if (line[0] == '#' || (!strchr(line, ' ') && !strchr(line, '\t'))) return;
    char *f[6]; int nf = 0; char *p = line;
    while (*p && nf < 5) { while (*p == ' ' || *p == '\t') p++; if (!*p || *p == '\n') break; f[nf++] = p; while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++; }
    if (nf < 5) return;
    char *fl = f[4]; char *end = fl; while (*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
    char flags[1024]; size_t fl_len = (size_t)(end - fl); if (fl_len >= sizeof flags) return;
    memcpy(flags, fl, fl_len); flags[fl_len] = 0;
    char res[1024] = ""; char *save = NULL;
    for (char *t = strtok_r(flags, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strncmp(t, "avb", 3) || !strncmp(t, "verify", 6)) continue;
        if (*res) strcat(res, ","); strcat(res, t);
    }
    if (!*res) strcpy(res, "defaults");
    char tail[4096]; snprintf(tail, sizeof tail, "%s", end);
    snprintf(fl, 1024 + strlen(tail) + 2, "%s%s", res, tail);
}

static void delavb_file(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char *out = malloc(1 << 20); size_t on = 0; char line[4096]; int changed = 0;
    while (fgets(line, sizeof line, f) && on < (1 << 20) - 8192) {
        char orig[4096]; snprintf(orig, sizeof orig, "%s", line);
        char buf[8192]; snprintf(buf, sizeof buf, "%s", line); avb_strip_line(buf);
        if (strcmp(buf, orig)) changed = 1;
        on += (size_t)snprintf(out + on, (1 << 20) - on, "%s", buf);
    }
    fclose(f);
    if (changed) { FILE *w = fopen(path, "w"); if (w) { fwrite(out, 1, on, w); fclose(w); VPRINTF("> AVB removed from %s\n", path); } }
    free(out);
}

static void delavb_tree(const char *dir, int depth) {
    DIR *d = opendir(dir); if (!d || depth > 6) { if (d) closedir(d); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char p[8192]; snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat sb; if (lstat(p, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) delavb_tree(p, depth + 1);
        else if (S_ISREG(sb.st_mode) && !strncmp(de->d_name, "fstab", 5)) delavb_file(p);
    }
    closedir(d);
}

static int finish_output(const char *raw, const char *final, const ropts_t *o) {
    int rc;
    if (o->out_kind) {                                   /* 1 = DAT, 2 = BR: the image itself is only an intermediate */
        char dir[4400], name[512]; snprintf(dir, sizeof dir, "%s", final);
        char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, ".");
        base_name(final, name, sizeof name);
        VPRINTF("> Converting to %s\n", o->out_kind == 2 ? "dat + brotli" : "dat"); fflush(stdout);
        rc = img_to_dat(raw, dir, name, o->brlvl > 0 ? o->brlvl : 3, o->out_kind == 2);
        remove(raw);
        if (!rc) { part_done_line(); printf("> Done: %s/%s.%s\n", dir, name, o->out_kind == 2 ? "new.dat.br" : "new.dat"); }
        return rc;
    }
    if (!access(final, F_OK)) VPRINTF("> Replacing existing: %s\n", final);
    if (o->sparse && !o->out_kind) { VPRINTF("> Converting to sparse\n"); fflush(stdout); rc = raw_to_sparse(raw, final); remove(raw); }
    else rc = rename(raw, final);
    if (!rc) part_done_line();
    return rc;
}

static int repack_ext4(const char *name, const char *dro, const char *pro, const ropts_t *o) {
    char tree[4400], cfgd[4400], fsc[4600], fcx[4600], fso[4600], inf[4600], final[4600], raw[4700];
    snprintf(tree, sizeof tree, "%s/%s", dro, name); snprintf(cfgd, sizeof cfgd, "%s/config", dro);
    snprintf(fsc, sizeof fsc, "%s/%s_fs_config", cfgd, name); snprintf(fcx, sizeof fcx, "%s/%s_file_contexts", cfgd, name);
    snprintf(fso, sizeof fso, "%s/%s_fs_options", cfgd, name); snprintf(inf, sizeof inf, "%s/%s_info", cfgd, name);
    snprintf(final, sizeof final, "%s/out/%s" REPACK_SUFFIX ".img", pro, name);
    snprintf(raw, sizeof raw, "%s.tmp", final);
    if (access(fsc, R_OK)) { fprintf(stderr, "> Error: %s not found\n", fsc); return 1; }
    if (access(fcx, R_OK)) { VWARN("> Warning: %s not found, SELinux labels will be missing\n", fcx); }

    tstat_t st = {0, 0}; tree_stat(tree, &st, 0);
    uint64_t inodes = st.entries + st.entries / 20 + 64;
    uint64_t est = (st.blocks + inodes * 256 / 4096) * 103 / 100 + 160;      /* content + inode tables + fs overhead */
    uint64_t blocks;
    if (o->autosize) blocks = est * 105 / 100 + 2048;                       /* ~8 MiB free space */
    else {
        uint64_t sz = read_u64_file(inf);
        if (sz < 4096) { VPRINTF("> Original size unknown (%s missing), using Auto\n", inf); blocks = est * 105 / 100 + 2048; }
        else blocks = sz / 4096;
    }
    char uuid[64] = ""; read_opt(fso, "uuid", uuid, sizeof uuid);
    int rc = 1;
    for (int attempt = 0; attempt < 8; attempt++) {
        VPRINTF("> Building <ext4> (%s): %s, %llu blocks (%llu MiB)\n", o->rw ? "RW" : "RO", name,
               (unsigned long long)blocks, (unsigned long long)(blocks * 4 / 1024)); fflush(stdout);
        remove(raw);
        FILE *z = fopen(raw, "wb"); if (!z) { fprintf(stderr, "> Error: cannot create %s\n", raw); return 1; }
        if (ftruncate(fileno(z), (off_t)(blocks * 4096))) { fclose(z); fprintf(stderr, "> Error: not enough space\n"); return 1; }
        fclose(z);
        char nb[32], ni[32], lbl[64], mp[300], blk[32];
        snprintf(ni, sizeof ni, "%llu", (unsigned long long)inodes); snprintf(blk, sizeof blk, "%llu", (unsigned long long)blocks);
        snprintf(lbl, sizeof lbl, "%.15s", name); snprintf(mp, sizeof mp, "/%s", name); (void)nb;
        char *mk[24]; int k = 0;
        mk[k++] = "mke2fs"; mk[k++] = "-q"; mk[k++] = "-F"; mk[k++] = "-t"; mk[k++] = "ext4";
        mk[k++] = "-b"; mk[k++] = "4096"; mk[k++] = "-I"; mk[k++] = "256"; mk[k++] = "-m"; mk[k++] = "0";
        mk[k++] = "-N"; mk[k++] = ni; mk[k++] = "-L"; mk[k++] = lbl; mk[k++] = "-M"; mk[k++] = mp;
        mk[k++] = "-O"; mk[k++] = "^has_journal,^resize_inode,^flex_bg,^metadata_csum,^64bit,uninit_bg,extent,huge_file,dir_nlink,extra_isize,dir_index,ext_attr,filetype,sparse_super,large_file";
        if (uuid[0]) { mk[k++] = "-U"; mk[k++] = uuid; }
        mk[k++] = raw; mk[k++] = blk; mk[k] = NULL;
        if (run(mk)) { fprintf(stderr, "> Error: mke2fs failed%s\n", o->autosize ? "" : " (original size is probably too small, use Auto size)"); remove(raw); return 1; }

        char *fd[24]; k = 0;
        fd[k++] = "e2fsdroid"; fd[k++] = "-e"; fd[k++] = "-T"; fd[k++] = "1230768000";
        fd[k++] = "-C"; fd[k++] = fsc;
        if (!access(fcx, R_OK)) { fd[k++] = "-S"; fd[k++] = fcx; }
        fd[k++] = "-a"; fd[k++] = mp;
        if (!o->rw) fd[k++] = "-s";
        fd[k++] = "-f"; fd[k++] = tree; fd[k++] = raw; fd[k] = NULL;
        rc = run(fd);
        if (!rc) break;
        if (!o->autosize) { fprintf(stderr, "> Error: content does not fit into the original size, use Auto size\n"); remove(raw); return 1; }
        blocks = blocks * 112 / 100 + 512;
        VWARN("> Image too small, retrying with a larger size\n");
    }
    if (rc) { fprintf(stderr, "> Error: e2fsdroid failed\n"); remove(raw); return 1; }
    if (o->shrink) {
        VPRINTF("> Shrinking free space\n"); fflush(stdout);
        char *ck[] = { "e2fsck", "-fy", raw, NULL }; run(ck);
        char *rs[] = { "resize2fs", "-f", "-M", raw, NULL };
        if (run(rs)) VWARN("> Warning: shrink failed, keeping full size\n");
    }
    if (finish_output(raw, final, o)) { fprintf(stderr, "> Error: cannot write %s\n", final); return 1; }
    if (!o->out_kind) printf("> Done: %s\n", final);
    return 0;
}

typedef struct {
    FILE *out;
    char **known;
    size_t nknown;
    const char *tree;
    const char *name;
} fsc_ctx_t;

static int fsc_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int fsc_known(fsc_ctx_t *c, const char *path) {
    return c->nknown && bsearch(&path, c->known, c->nknown, sizeof *c->known, fsc_cmp);
}

static void fsc_add(fsc_ctx_t *c, const char *rel, const struct stat *st) {
    char key[8192];
    snprintf(key, sizeof key, "%s/%s", c->name, rel);
    if (fsc_known(c, key)) return;
    unsigned mode = st->st_mode & 07777;
    if (S_ISLNK(st->st_mode)) mode = 0644;
    else if (!mode) mode = S_ISDIR(st->st_mode) ? 0755 : 0644;
    unsigned uid = st->st_uid < 10000 ? (unsigned)st->st_uid : 0;
    unsigned gid = st->st_gid < 10000 ? (unsigned)st->st_gid : 0;
    fprintf(c->out, "%s %u %u %04o\n", key, uid, gid, mode);
}

static void fsc_walk(fsc_ctx_t *c, const char *rel, int depth) {
    if (depth > 64) return;
    char dir[8192];
    snprintf(dir, sizeof dir, "%s/%s", c->tree, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[8192], full[8192];
        snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        snprintf(full, sizeof full, "%s/%s", c->tree, child);
        struct stat st;
        if (lstat(full, &st)) continue;
        fsc_add(c, child, &st);
        if (S_ISDIR(st.st_mode)) fsc_walk(c, child, depth + 1);
    }
    closedir(d);
}

static void fs_config_complete(const char *tree, const char *name, const char *fsc) {
    FILE *f = fopen(fsc, "r");
    if (!f) return;
    char **known = NULL;
    size_t n = 0, cap = 0;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
        if (e == p) continue;
        *e = 0;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 1024;
            char **np = realloc(known, nc * sizeof *np);
            if (!np) break;
            known = np; cap = nc;
        }
        known[n] = strdup(p);
        if (!known[n]) break;
        n++;
    }
    fclose(f);
    if (n > 1) qsort(known, n, sizeof *known, fsc_cmp);
    f = fopen(fsc, "a");
    if (f) {
        fsc_ctx_t c = { f, known, n, tree, name };
        struct stat st;
        if (!stat(tree, &st)) fsc_add(&c, "", &st);
        fsc_walk(&c, "", 0);
        fclose(f);
    }
    for (size_t i = 0; i < n; i++) free(known[i]);
    free(known);
}


typedef struct { char *path, *ctx, *parent; } fc_ent_t;

static int fc_meta(char c) { return strchr(".+*?()[]{}|^$", c) != NULL; }

static int fc_exact(const char *pat, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; pat[i]; i++) {
        if (pat[i] == '\\') {
            if (!pat[i + 1] || j + 1 >= n) return 0;
            out[j++] = pat[++i];
        } else {
            if (fc_meta(pat[i]) || j + 1 >= n) return 0;
            out[j++] = pat[i];
        }
    }
    out[j] = 0;
    return 1;
}

static void fc_norm(char *s) {
    size_t l = strlen(s);
    while (l > 1 && s[l - 1] == '/') s[--l] = 0;
}

static void fc_dir(const char *path, char *out, size_t n) {
    const char *s = strrchr(path, '/');
    if (!s) { snprintf(out, n, "."); return; }
    if (s == path) { snprintf(out, n, "/"); return; }
    size_t l = (size_t)(s - path);
    if (l >= n) l = n - 1;
    memcpy(out, path, l); out[l] = 0;
}

static int fc_under(const char *lit, const char *name, const char **rel) {
    char prefix[512];
    snprintf(prefix, sizeof prefix, "/%s", name);
    size_t n = strlen(prefix);
    if (strncmp(lit, prefix, n)) return 0;
    if (!lit[n]) { *rel = lit + n; return 1; }
    if (lit[n] != '/') return 0;
    *rel = lit + n + 1;
    return 1;
}

static int fc_parent_ok(const char *tree, const char *rel) {
    if (!*rel) return 1;
    char parent[8192], full[8192];
    const char *s = strrchr(rel, '/');
    if (!s) parent[0] = 0;
    else {
        size_t l = (size_t)(s - rel);
        if (l >= sizeof parent) return 0;
        memcpy(parent, rel, l); parent[l] = 0;
    }
    if (*parent) snprintf(full, sizeof full, "%s/%s", tree, parent);
    else snprintf(full, sizeof full, "%s", tree);
    struct stat st;
    return !stat(full, &st) && S_ISDIR(st.st_mode);
}

static int fc_implicit(const char *rel) {
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    return !strcmp(base, "lost+found");
}

static int fc_stale(const char *lit, const char *tree, const char *name) {
    const char *rel;
    if (!fc_under(lit, name, &rel) || fc_implicit(rel) || !fc_parent_ok(tree, rel)) return 0;
    char full[8192];
    if (*rel) snprintf(full, sizeof full, "%s/%s", tree, rel);
    else snprintf(full, sizeof full, "%s", tree);
    struct stat st;
    return lstat(full, &st) != 0;
}


static size_t fs_config_prune(const char *tree, const char *name, const char *fsc) {
    FILE *f = fopen(fsc, "r");
    if (!f) return 0;
    char tmp[4600];
    snprintf(tmp, sizeof tmp, "%s.tmp", fsc);
    FILE *o = fopen(tmp, "w");
    if (!o) { fclose(f); return 0; }
    char line[8192];
    size_t removed = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
        int drop = 0;
        if (e != p) {
            char key[8192], abs[8192];
            size_t l = (size_t)(e - p);
            if (l < sizeof key) {
                memcpy(key, p, l); key[l] = 0;
                snprintf(abs, sizeof abs, "/%s", key);
                const char *rel;
                if (fc_under(abs, name, &rel) && !fc_implicit(rel)) {
                    char full[8192];
                    if (*rel) snprintf(full, sizeof full, "%s/%s", tree, rel);
                    else snprintf(full, sizeof full, "%s", tree);
                    struct stat st;
                    if (lstat(full, &st)) drop = 1;
                }
            }
        }
        if (drop) removed++;
        else fputs(line, o);
    }
    fclose(f);
    fclose(o);
    if (removed) {
        if (rename(tmp, fsc)) { VWARN("> Warning: cannot update %s\n", fsc); remove(tmp); return 0; }
    } else remove(tmp);
    return removed;
}

static int fc_parse_line(const char *line, char *pat, size_t pn, char *ctx, size_t cn) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == '\r' || *p == '\n') return 0;
    const char *e = p;
    while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
    size_t pl = (size_t)(e - p);
    if (!pl || pl >= pn) return 0;
    memcpy(pat, p, pl); pat[pl] = 0;
    while (*e == ' ' || *e == '\t') e++;
    const char *c = e;
    while (*c && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') c++;
    size_t cl = (size_t)(c - e);
    if (!cl || cl >= cn) return 0;
    memcpy(ctx, e, cl); ctx[cl] = 0;
    return 1;
}

static int fc_ent_cmp(const void *a, const void *b) {
    const fc_ent_t *x = a, *y = b;
    return strcmp(x->path, y->path);
}

static int fc_exists(const fc_ent_t *e, size_t n, const char *path) {
    fc_ent_t key = { (char *)path, NULL, NULL };
    return n && bsearch(&key, e, n, sizeof *e, fc_ent_cmp) != NULL;
}

static int fc_majority(const fc_ent_t *e, size_t n, const char *parent, char *out, size_t on) {
    int best = 0, second = 0;
    const char *bc = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(e[i].parent, parent)) continue;
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(e[j].parent, parent) && !strcmp(e[j].ctx, e[i].ctx)) { seen = 1; break; }
        if (seen) continue;
        int cnt = 0;
        for (size_t j = i; j < n; j++)
            if (!strcmp(e[j].parent, parent) && !strcmp(e[j].ctx, e[i].ctx)) cnt++;
        if (cnt > best) { second = best; best = cnt; bc = e[i].ctx; }
        else if (cnt > second) second = cnt;
    }
    if (best >= 2 && best > second) { snprintf(out, on, "%s", bc); return 1; }
    return 0;
}

static const char *fc_default(const char *name) {
    if (!strcmp(name, "vendor") || !strcmp(name, "odm")) return "u:object_r:vendor_file:s0";
    return "u:object_r:system_file:s0";
}

static int fc_bin_hw(const char *dir) {
    size_t n = strlen(dir);
    return n >= 7 && !strcmp(dir + n - 7, "/bin/hw");
}

static void fc_strip_suffix(char *s, const char *suf) {
    size_t l = strlen(s), sl = strlen(suf);
    if (l > sl && !strcmp(s + l - sl, suf)) s[l - sl] = 0;
}

static int fc_cil_has(const char *dro, const char *type);

static int fc_hal_add(char names[][256], int *n, const char *s) {
    if (!*s || *n >= 16) return 0;
    for (int i = 0; i < *n; i++) if (!strcmp(names[i], s)) return 0;
    snprintf(names[(*n)++], 256, "%s", s);
    return 1;
}

static int fc_hal_names(const char *base, char names[][256]) {
    char b[512];
    snprintf(b, sizeof b, "%s", base);
    fc_strip_suffix(b, ".so");
    fc_strip_suffix(b, "-service");
    fc_strip_suffix(b, ".service");
    char *at = strchr(b, '@'); if (at) *at = 0;
    static const char *pfx[] = {
        "android.hardware.", "android.hidl.", "vendor.qti.hardware.",
        "vendor.qti.", "vendor.", "android.", "hardware."
    };
    int again = 1;
    while (again) {
        again = 0;
        for (size_t i = 0; i < sizeof pfx / sizeof pfx[0]; i++) {
            size_t l = strlen(pfx[i]);
            if (!strncmp(b, pfx[i], l)) { memmove(b, b + l, strlen(b + l) + 1); again = 1; break; }
        }
    }
    char *hw = strstr(b, ".hardware.");
    if (hw) memmove(b, hw + 10, strlen(hw + 10) + 1);

    char comp[8][128];
    int nc = 0;
    const char *p = b;
    while (*p && nc < 8) {
        while (*p == '.' || *p == '-' || *p == '@') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '.' && *e != '-' && *e != '@') e++;
        size_t j = 0;
        for (; p < e && j + 1 < sizeof comp[0]; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || c == '_') comp[nc][j++] = (char)tolower(c);
        }
        comp[nc][j] = 0;
        if (j) nc++;
        p = e;
    }
    if (!nc) return 0;

    char full[256], alias[256];
    size_t j = 0;
    for (int i = 0; i < nc; i++) {
        if (i) full[j++] = '_';
        size_t l = strlen(comp[i]);
        if (j + l >= sizeof full) break;
        memcpy(full + j, comp[i], l); j += l;
    }
    full[j] = 0;
    snprintf(alias, sizeof alias, "%s", full);
    if (!strcmp(comp[0], "display")) {
        size_t l = strlen("graphics");
        if (l + strlen(alias) - strlen("display") < sizeof alias) {
            memmove(alias + l, alias + strlen("display"), strlen(alias + strlen("display")) + 1);
            memcpy(alias, "graphics", l);
        }
    }

    int n = 0;
    fc_hal_add(names, &n, full);
    if (strcmp(alias, full)) fc_hal_add(names, &n, alias);
    fc_hal_add(names, &n, comp[0]);
    fc_hal_add(names, &n, comp[nc - 1]);
    for (int i = 0; i < nc; i++) fc_hal_add(names, &n, comp[i]);
    return n;
}

static int fc_hal_pick(const char *dro, const char *base, char *out, size_t on) {
    char names[16][256];
    int n = fc_hal_names(base, names);
    if (!n) return -1;
    for (int i = 0; i < n; i++) {
        char cand[300];
        snprintf(cand, sizeof cand, "hal_%s_default_exec", names[i]);
        if (fc_cil_has(dro, cand)) { snprintf(out, on, "%s", cand); return 1; }
    }
    return 0;
}

static char **g_cil_files;
static size_t g_cil_n, g_cil_cap;
static char *g_cil_root;

static void fc_cil_reset(void) {
    for (size_t i = 0; i < g_cil_n; i++) free(g_cil_files[i]);
    free(g_cil_files); g_cil_files = NULL;
    g_cil_n = g_cil_cap = 0;
    free(g_cil_root); g_cil_root = NULL;
}

static void fc_cil_collect(const char *dir, int depth) {
    if (depth > 64) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { fc_cil_collect(full, depth + 1); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        size_t l = strlen(full);
        if (l <= 4 || strcmp(full + l - 4, ".cil")) continue;
        if (g_cil_n == g_cil_cap) {
            size_t nc = g_cil_cap ? g_cil_cap * 2 : 16;
            char **np = realloc(g_cil_files, nc * sizeof *np);
            if (!np) break;
            g_cil_files = np; g_cil_cap = nc;
        }
        g_cil_files[g_cil_n] = strdup(full);
        if (!g_cil_files[g_cil_n]) break;
        g_cil_n++;
    }
    closedir(d);
}

static int fc_token(const char *buf, const char *tok) {
    size_t n = strlen(tok);
    const char *p = buf;
    while ((p = strstr(p, tok))) {
        int left = (p == buf) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        int right = !(isalnum((unsigned char)p[n]) || p[n] == '_');
        if (left && right) return 1;
        p += n;
    }
    return 0;
}

static int fc_cil_file(const char *path, const char *type) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (128L << 20)) { fclose(f); return 0; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    int r = fc_token(buf, type);
    free(buf);
    return r;
}

static int fc_cil_has(const char *dro, const char *type) {
    if (!g_cil_root || strcmp(g_cil_root, dro)) {
        fc_cil_reset();
        g_cil_root = strdup(dro);
        if (!g_cil_root) return 0;
        fc_cil_collect(dro, 0);
    }
    for (size_t i = 0; i < g_cil_n; i++) if (fc_cil_file(g_cil_files[i], type)) return 1;
    return 0;
}

typedef struct {
    const char *tree, *name, *dro;
    fc_ent_t *ents; size_t nents;
    char **adds; size_t nadd, cap;
    int hal_fallback;
} fcx_ctx_t;

static void fc_escape(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (; *in && j + 2 < n; in++) {
        if (fc_meta(*in) || *in == '\\') out[j++] = '\\';
        out[j++] = *in;
    }
    out[j] = 0;
}

static void fc_add_line(fcx_ctx_t *c, const char *path, const char *ctx) {
    if (c->nadd == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 64;
        char **np = realloc(c->adds, nc * sizeof *np);
        if (!np) return;
        c->adds = np; c->cap = nc;
    }
    char esc[16384];
    fc_escape(path, esc, sizeof esc);
    size_t l = strlen(esc) + strlen(ctx) + 3;
    char *s = malloc(l);
    if (!s) return;
    snprintf(s, l, "%s %s\n", esc, ctx);
    c->adds[c->nadd++] = s;
}

static void fc_add_path(fcx_ctx_t *c, const char *rel, const struct stat *st) {
    char abs[8192], parent[8192], tmp[512], type[256];
    if (*rel) snprintf(abs, sizeof abs, "/%s/%s", c->name, rel);
    else snprintf(abs, sizeof abs, "/%s", c->name);
    if (fc_exists(c->ents, c->nents, abs)) return;
    fc_dir(abs, parent, sizeof parent);
    const char *ctx;
    if (!S_ISDIR(st->st_mode) && fc_bin_hw(parent)) {
        const char *base = strrchr(abs, '/');
        base = base ? base + 1 : abs;
        int hp = fc_hal_pick(c->dro, base, type, sizeof type);
        if (hp == 1) {
            snprintf(tmp, sizeof tmp, "u:object_r:%s:s0", type);
            ctx = tmp;
        } else if (c->hal_fallback) {
            ctx = "u:object_r:hal_allocator_default_exec:s0";
            if (hp == 0) VWARN("> Warning: HAL type for %s not found in .cil, using hal_allocator_default_exec for %s\n", base, abs);
        } else {
            ctx = fc_default(c->name);
            if (hp == 0) VWARN("> Warning: HAL type for %s not found in .cil, using %s for %s\n", base, ctx, abs);
        }
    } else if (fc_majority(c->ents, c->nents, parent, tmp, sizeof tmp)) ctx = tmp;
    else ctx = fc_default(c->name);
    fc_add_line(c, abs, ctx);
}

static void fc_walk(fcx_ctx_t *c, const char *rel, int depth) {
    if (depth > 64) return;
    char dir[8192];
    snprintf(dir, sizeof dir, "%s/%s", c->tree, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[8192], full[8192];
        snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
        snprintf(full, sizeof full, "%s/%s", c->tree, child);
        struct stat st;
        if (lstat(full, &st)) continue;
        fc_add_path(c, child, &st);
        if (S_ISDIR(st.st_mode)) fc_walk(c, child, depth + 1);
    }
    closedir(d);
}

static void file_contexts_sync(const char *dro, const char *name, const char *tree, const char *fcx, int hal_fallback) {
    FILE *f = fopen(fcx, "r");
    if (!f) return;
    char **lines = NULL;
    size_t nl = 0, lc = 0;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        if (nl == lc) {
            size_t nc = lc ? lc * 2 : 1024;
            char **np = realloc(lines, nc * sizeof *np);
            if (!np) break;
            lines = np; lc = nc;
        }
        lines[nl] = strdup(line);
        if (!lines[nl]) break;
        nl++;
    }
    fclose(f);

    unsigned char *keep = malloc(nl ? nl : 1);
    if (!keep) {
        for (size_t i = 0; i < nl; i++) free(lines[i]);
        free(lines); return;
    }
    fc_ent_t *ents = NULL;
    size_t ne = 0, ec = 0;
    size_t removed = 0;
    for (size_t i = 0; i < nl; i++) {
        keep[i] = 1;
        char pat[8192], ctx[512], lit[8192];
        if (!fc_parse_line(lines[i], pat, sizeof pat, ctx, sizeof ctx)) continue;
        if (!fc_exact(pat, lit, sizeof lit)) continue;
        fc_norm(lit);
        if (fc_stale(lit, tree, name)) { keep[i] = 0; removed++; continue; }
        if (ne == ec) {
            size_t nc = ec ? ec * 2 : 1024;
            fc_ent_t *np = realloc(ents, nc * sizeof *np);
            if (!np) break;
            ents = np; ec = nc;
        }
        char parent[8192];
        fc_dir(lit, parent, sizeof parent);
        ents[ne].path = strdup(lit);
        ents[ne].ctx = strdup(ctx);
        ents[ne].parent = strdup(parent);
        if (!ents[ne].path || !ents[ne].ctx || !ents[ne].parent) {
            free(ents[ne].path); free(ents[ne].ctx); free(ents[ne].parent);
            break;
        }
        ne++;
    }

    if (ne > 1) qsort(ents, ne, sizeof *ents, fc_ent_cmp);
    char tmp[4600];
    snprintf(tmp, sizeof tmp, "%s.tmp", fcx);
    FILE *out = fopen(tmp, "w");
    if (out) {
        for (size_t i = 0; i < nl; i++) if (keep[i]) fputs(lines[i], out);
        fcx_ctx_t c = { tree, name, dro, ents, ne, NULL, 0, 0, hal_fallback };
        struct stat st;
        if (!lstat(tree, &st)) fc_add_path(&c, "", &st);
        fc_walk(&c, "", 0);
        for (size_t i = 0; i < c.nadd; i++) fputs(c.adds[i], out);
        fclose(out);
        if (removed || c.nadd) {
            if (rename(tmp, fcx)) { VWARN("> Warning: cannot update %s\n", fcx); remove(tmp); }
            else VPRINTF("> Contexts: %zu added, %zu removed\n", c.nadd, removed);
        } else remove(tmp);
        for (size_t i = 0; i < c.nadd; i++) free(c.adds[i]);
        free(c.adds);
    }

    for (size_t i = 0; i < ne; i++) { free(ents[i].path); free(ents[i].ctx); free(ents[i].parent); }
    free(ents);
    for (size_t i = 0; i < nl; i++) free(lines[i]);
    free(lines);
    free(keep);
}

static int repack_erofs(const char *name, const char *dro, const char *pro, const ropts_t *o) {
    char tree[4400], fsc[4600], fcx[4600], fso[4600], final[4600], raw[4700], mp[300], comp[64], uuid[64] = "", ub[80], fcb[4700], fsb[4700];
    snprintf(tree, sizeof tree, "%s/%s", dro, name);
    snprintf(fsc, sizeof fsc, "%s/config/%s_fs_config", dro, name); snprintf(fcx, sizeof fcx, "%s/config/%s_file_contexts", dro, name);
    snprintf(fso, sizeof fso, "%s/config/%s_fs_options", dro, name);
    snprintf(final, sizeof final, "%s/out/%s" REPACK_SUFFIX ".img", pro, name); snprintf(raw, sizeof raw, "%s.tmp", final);
    snprintf(mp, sizeof mp, "--mount-point=/%s", name);
    read_opt(fso, "uuid", uuid, sizeof uuid);
    int lvl = o->erofslvl;
    if (!strcmp(o->mode, "lz4")) snprintf(comp, sizeof comp, "-zlz4");
    else if (!strcmp(o->mode, "lzma")) snprintf(comp, sizeof comp, "-zlzma,%d", lvl > 0 ? lvl : 6);
    else snprintf(comp, sizeof comp, "-zlz4hc,%d", lvl > 0 ? lvl : 9);
    char *a[24]; int k = 0;
    a[k++] = "mkfs.erofs"; a[k++] = comp; a[k++] = "-T"; a[k++] = "0";
    char wb[32];
    if (o->erofs_workers > 0) { snprintf(wb, sizeof wb, "--workers=%d", o->erofs_workers); a[k++] = wb; }
    if (uuid[0]) { snprintf(ub, sizeof ub, "%s", uuid); a[k++] = "-U"; a[k++] = ub; }
    a[k++] = mp;
    snprintf(fsb, sizeof fsb, "--fs-config-file=%s", fsc); if (!access(fsc, R_OK)) a[k++] = fsb;
    snprintf(fcb, sizeof fcb, "--file-contexts=%s", fcx); if (!access(fcx, R_OK)) a[k++] = fcb;
    a[k++] = raw; a[k++] = tree; a[k] = NULL;
    if (o->erofs_workers > 0) VPRINTF("> Building <erofs> (%s): %s, %d workers\n", comp, name, o->erofs_workers);
    else VPRINTF("> Building <erofs> (%s): %s\n", comp, name);
    fflush(stdout);
    remove(raw);
    if (run_quiet(a)) { fprintf(stderr, "> Error: mkfs.erofs failed\n"); remove(raw); return 1; }
    if (finish_output(raw, final, o)) return 1;
    if (!o->out_kind) printf("> Done: %s\n", final);
    return 0;
}

static int repack_f2fs(const char *name, const char *dro, const char *pro, const ropts_t *o) {
    char tree[4400], fsc[4600], fcx[4600], fso[4600], inf[4600], final[4600], raw[4700], lbl[64], uuid[64] = "", mp[300];
    snprintf(tree, sizeof tree, "%s/%s", dro, name);
    snprintf(fsc, sizeof fsc, "%s/config/%s_fs_config", dro, name);
    snprintf(fcx, sizeof fcx, "%s/config/%s_file_contexts", dro, name);
    snprintf(fso, sizeof fso, "%s/config/%s_fs_options", dro, name);
    snprintf(inf, sizeof inf, "%s/config/%s_info", dro, name);
    snprintf(final, sizeof final, "%s/out/%s.img", pro, name);
    snprintf(raw, sizeof raw, "%s.tmp", final);
    snprintf(lbl, sizeof lbl, "%.15s", name);
    snprintf(mp, sizeof mp, "/%s", name);
    read_opt(fso, "uuid", uuid, sizeof uuid);

    uint64_t bytes = 0;
    if (!o->autosize) {
        bytes = read_u64_file(inf);
        if (bytes < (64u << 20)) bytes = 0;
    }
    if (!bytes) {
        tstat_t st = {0, 0}; tree_stat(tree, &st, 0);
        bytes = st.blocks * 4096;
        bytes = bytes * 115 / 100 + (64u << 20);
    }
    bytes = (bytes + 4095) & ~4095ull;
    if (bytes < (64u << 20)) bytes = 64u << 20;

    VPRINTF("> Building <f2fs>: %s, %llu MiB\n", name, (unsigned long long)(bytes >> 20)); fflush(stdout);
    remove(raw);
    FILE *z = fopen(raw, "wb");
    if (!z) { fprintf(stderr, "> Error: cannot create %s\n", raw); return 1; }
    if (ftruncate(fileno(z), (off_t)bytes)) { fclose(z); fprintf(stderr, "> Error: not enough space\n"); remove(raw); return 1; }
    fclose(z);

    char *mk[16]; int k = 0;
    mk[k++] = "mkfs.f2fs"; mk[k++] = "-f"; mk[k++] = "-l"; mk[k++] = lbl;
    if (uuid[0]) { mk[k++] = "-U"; mk[k++] = uuid; }
    mk[k++] = raw; mk[k] = NULL;
    if (run(mk)) { fprintf(stderr, "> Error: mkfs.f2fs failed\n"); remove(raw); return 1; }

    char *sl[12]; k = 0;
    sl[k++] = "sload_f2fs"; sl[k++] = "-f"; sl[k++] = tree;
    sl[k++] = "-t"; sl[k++] = mp;
    if (!access(fsc, R_OK)) { sl[k++] = "-C"; sl[k++] = fsc; }
    if (!access(fcx, R_OK)) { sl[k++] = "-s"; sl[k++] = fcx; }
    sl[k++] = raw; sl[k] = NULL;
    if (run(sl)) { fprintf(stderr, "> Error: sload_f2fs failed\n"); remove(raw); return 1; }
    if (finish_output(raw, final, o)) { fprintf(stderr, "> Error: cannot write %s\n", final); return 1; }
    if (!o->out_kind) printf("> Done: %s\n", final);
    return 0;
}

static int repack_boot(const char *name, const char *dro, const char *pro) {
    char dir[4400], orig[4400], final[4600], cwd[2048];
    snprintf(dir, sizeof dir, "%s/%s", dro, name); snprintf(orig, sizeof orig, "%s/%s.img", dir, name);
    snprintf(final, sizeof final, "%s/out/%s" REPACK_SUFFIX ".img", pro, name);
    if (access(orig, R_OK)) snprintf(orig, sizeof orig, "%s/%s.img", pro, name);        /* fallback: old projects */
    if (access(orig, R_OK)) { fprintf(stderr, "> Error: original %s/%s.img is required to repack boot images\n", dir, name); return 1; }
    if (!getcwd(cwd, sizeof cwd) || chdir(dir)) return 1;
    VPRINTF("> Repacking boot image: %s\n", name); fflush(stdout);
    char tmp[4700]; snprintf(tmp, sizeof tmp, "%s.tmp", final);
    char *a[] = { "magiskboot", "repack", orig, tmp, NULL };
    int rc = run(a);
    if (chdir(cwd)) return 1;
    if (!rc) {
        if (!access(final, F_OK)) VPRINTF("> Replacing existing: %s\n", final);
        if (rename(tmp, final)) { fprintf(stderr, "> Error: cannot write %s\n", final); return 1; }
        part_done_line();
        printf("> Done: %s\n", final);
    } else remove(tmp);
    return rc;
}

typedef enum { PERF_SAVER = 0, PERF_BALANCED, PERF_FAST, PERF_TURBO, PERF_MANUAL } perf_class_t;
typedef enum { STORAGE_UNKNOWN = 0, STORAGE_EMMC, STORAGE_UFS, STORAGE_NVME, STORAGE_OTHER } storage_kind_t;

typedef struct {
    uint64_t mem_total_kb, mem_avail_kb, swap_total_kb, swap_free_kb;
    int ncpu_online, ncpu_conf, big_cores, little_cores, cpu_budget;
    storage_kind_t storage;
    int temp_battery_c, temp_skin_c, temp_soc_c;
    int psi_mem_avg10, psi_cpu_avg10, psi_io_avg10;
    uint64_t free_tmp_bytes, free_pro_bytes;
    perf_class_t cls;
    int part_jobs, total_cpu_threads, erofs_workers, brotli_jobs, zstd_threads, nice_level, reserve_mb;
} perf_t;

static uint64_t read_u64_path(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

static int read_text_path(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    out[0] = 0;
    if (!fgets(out, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return 1;
}

static int lower_has(const char *s, const char *needle) {
    char a[128], b[128];
    size_t i;
    for (i = 0; s[i] && i + 1 < sizeof a; i++) a[i] = (char)tolower((unsigned char)s[i]);
    a[i] = 0;
    for (i = 0; needle[i] && i + 1 < sizeof b; i++) b[i] = (char)tolower((unsigned char)needle[i]);
    b[i] = 0;
    return strstr(a, b) != NULL;
}

static int norm_temp(long v) {
    if (v > 1000) v /= 1000;
    else if (v > 150) v /= 10;
    if (v < -20 || v > 150) return 0;
    return (int)v;
}

static int dir_has_prefix(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    size_t pl = strlen(prefix);
    while ((de = readdir(d))) {
        if (!strncmp(de->d_name, prefix, pl)) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int dir_has_substr(const char *dir, const char *needle) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d))) {
        if (strstr(de->d_name, needle)) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int storage_detect(void) {
    char p[4096];
    if (!access(syspath(p, sizeof p, "/sys/block/mmcblk0"), F_OK)) return STORAGE_EMMC;
    if (!access(syspath(p, sizeof p, "/sys/block/nvme0n1"), F_OK)) return STORAGE_NVME;
    int sda = !access(syspath(p, sizeof p, "/sys/block/sda"), F_OK);
    char cls[4096], plat[4096];
    syspath(cls, sizeof cls, "/sys/class");
    syspath(plat, sizeof plat, "/sys/devices/platform");
    if (sda && (dir_has_prefix(cls, "ufs") || dir_has_substr(plat, "ufs"))) return STORAGE_UFS;
    if (!access(syspath(p, sizeof p, "/sys/block"), F_OK) && dir_has_prefix(syspath(p, sizeof p, "/sys/block"), "s")) return STORAGE_OTHER;
    if (!access(syspath(p, sizeof p, "/sys/block/mmcblk"), F_OK)) return STORAGE_OTHER;
    DIR *d = opendir(syspath(p, sizeof p, "/sys/block"));
    if (!d) return STORAGE_UNKNOWN;
    struct dirent *de;
    int any = 0;
    while ((de = readdir(d))) if (de->d_name[0] != '.') { any = 1; break; }
    closedir(d);
    return any ? STORAGE_OTHER : STORAGE_UNKNOWN;
}

static void thermal_scan(int *battery, int *skin, int *soc) {
    char dir[4096], p[4096], type[128];
    syspath(dir, sizeof dir, "/sys/class/thermal");
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strncmp(de->d_name, "thermal_zone", 12)) continue;
            snprintf(p, sizeof p, "%s/%s/type", dir, de->d_name);
            if (!read_text_path(p, type, sizeof type)) continue;
            snprintf(p, sizeof p, "%s/%s/temp", dir, de->d_name);
            int t = norm_temp((long)read_u64_path(p));
            if (!t) continue;
            if (lower_has(type, "battery") || lower_has(type, "bms")) { if (t > *battery) *battery = t; }
            else if (lower_has(type, "skin") || lower_has(type, "quiet_therm") || lower_has(type, "xo_therm") || lower_has(type, "case")) { if (t > *skin) *skin = t; }
            else if (lower_has(type, "cpu") || lower_has(type, "gpu") || lower_has(type, "soc") || lower_has(type, "cpuss") || lower_has(type, "mtktscpu")) { if (t > *soc) *soc = t; }
        }
        closedir(d);
    }
    if (!*battery) {
        syspath(p, sizeof p, "/sys/class/power_supply/battery/temp");
        int t = norm_temp((long)read_u64_path(p));
        if (t) *battery = t;
    }
}

static int psi_read(const char *rel) {
    char p[4096], line[512];
    FILE *f = fopen(syspath(p, sizeof p, rel), "r");
    if (!f) return 0;
    int v = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "some", 4)) continue;
        char *a = strstr(line, "avg10=");
        if (a) { double d = strtod(a + 6, NULL); if (d > 0) v = (int)d; }
        break;
    }
    fclose(f);
    return v;
}

static uint64_t free_bytes_path(const char *path) {
    struct statvfs sv;
    if (!path || !*path || statvfs(path, &sv)) return 0;
    return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
}

static void perf_detect(perf_t *p) {
    memset(p, 0, sizeof *p);
    p->mem_total_kb = meminfo_kb("MemTotal");
    p->mem_avail_kb = meminfo_kb("MemAvailable");
    if (!p->mem_avail_kb) p->mem_avail_kb = meminfo_kb("MemFree") + meminfo_kb("Buffers") + meminfo_kb("Cached");
    p->swap_total_kb = meminfo_kb("SwapTotal");
    p->swap_free_kb = meminfo_kb("SwapFree");

    long on = sysconf(_SC_NPROCESSORS_ONLN), conf = sysconf(_SC_NPROCESSORS_CONF);
    p->ncpu_online = on > 0 ? (int)on : 1;
    p->ncpu_conf = conf > 0 ? (int)conf : p->ncpu_online;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0 && n < p->ncpu_online) p->ncpu_online = n;
    }
    uint64_t caps[256] = {0};
    int have[256] = {0}, maxc = 0;
    int limit = p->ncpu_conf > 256 ? 256 : p->ncpu_conf;
    for (int i = 0; i < limit; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/sys/devices/system/cpu/cpu%d/cpu_capacity", g_sysroot, i);
        uint64_t v = read_u64_path(path);
        if (!v) {
            snprintf(path, sizeof path, "%s/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", g_sysroot, i);
            v = read_u64_path(path);
        }
        if (v) { caps[i] = v; have[i] = 1; if ((int)v > maxc) maxc = (int)v; }
    }
    if (!maxc) {
        p->big_cores = p->ncpu_online;
    } else {
        for (int i = 0; i < limit; i++) if (have[i]) {
            if ((int)caps[i] * 4 >= maxc * 3) p->big_cores++;
            else p->little_cores++;
        }
    }
    p->cpu_budget = p->big_cores + p->little_cores / 2;
    if (p->cpu_budget > p->ncpu_online - 1) p->cpu_budget = p->ncpu_online - 1;
    if (p->cpu_budget < 1) p->cpu_budget = 1;
    if (p->cpu_budget > 12) p->cpu_budget = 12;

    p->storage = (storage_kind_t)storage_detect();
    thermal_scan(&p->temp_battery_c, &p->temp_skin_c, &p->temp_soc_c);
    p->psi_mem_avg10 = psi_read("/proc/pressure/memory");
    p->psi_cpu_avg10 = psi_read("/proc/pressure/cpu");
    p->psi_io_avg10 = psi_read("/proc/pressure/io");

    const char *tmp = getenv("DNA_TMP");
    if (!tmp || !*tmp) tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/data/local/tmp";
    p->free_tmp_bytes = free_bytes_path(tmp);
    const char *pro = getenv("DNA_PRO");
    if (pro && *pro) {
        char out[4096];
        snprintf(out, sizeof out, "%s/out", pro);
        p->free_pro_bytes = free_bytes_path(out);
    }
}

static void thermal_thresholds(const char *env, int dw, int dh, int dc, int *w, int *h, int *c) {
    *w = dw; *h = dh; *c = dc;
    const char *s = getenv(env);
    if (!s || !*s) return;
    int a, b, d;
    char tail;
    if (sscanf(s, "%d,%d,%d%c", &a, &b, &d, &tail) != 3) return;
    if (!(a < b && b < d) || a < -50 || d > 200) return;
    *w = a; *h = b; *c = d;
}

static int perf_thermal_level(const perf_t *p) {
    int bw, bh, bc, sw, sh, sc, ow, oh, oc;
    thermal_thresholds("DNA_THERMAL_BATTERY", 40, 43, 46, &bw, &bh, &bc);
    thermal_thresholds("DNA_THERMAL_SKIN", 42, 45, 48, &sw, &sh, &sc);
    thermal_thresholds("DNA_THERMAL_SOC", 80, 90, 98, &ow, &oh, &oc);
    if (g_thermal_limit_hot > 0) {
        int h = g_thermal_limit_hot;
        bw = sw = ow = h - 5; bh = sh = oh = h; bc = sc = oc = h + 8;
    }
    int lvl = 0;
    int vals[3] = { p->temp_battery_c, p->temp_skin_c, p->temp_soc_c };
    int th[3][3] = { { bw, bh, bc }, { sw, sh, sc }, { ow, oh, oc } };
    for (int i = 0; i < 3; i++) {
        if (!vals[i]) continue;
        int x = 0;
        if (vals[i] >= th[i][2]) x = 3;
        else if (vals[i] >= th[i][1]) x = 2;
        else if (vals[i] >= th[i][0]) x = 1;
        if (x > lvl) lvl = x;
    }
    return lvl;
}


typedef struct { int n_ext4, n_erofs, n_f2fs, n_boot; int out_kind; } perf_load_t;
typedef struct {
    int has_profile; perf_class_t profile;
    int jobs, erofs_workers, brotli_jobs, zstd_threads, nice_level, reserve_mb;
    int has_jobs, has_erofs_workers, has_brotli_jobs, has_zstd_threads, has_nice, has_reserve;
} perf_ovr_t;

static int env_int(const char *name, int lo, int hi, int *out) {
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    char *e = NULL;
    long v = strtol(s, &e, 10);
    if (!e || *e || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

static void perf_ovr_from_env(perf_ovr_t *o) {
    memset(o, 0, sizeof *o);
    const char *pf = getenv("DNA_PERF");
    if (pf && *pf && strcmp(pf, "auto") && strcmp(pf, "AUTO")) {
        if (!strcasecmp(pf, "saver")) { o->has_profile = 1; o->profile = PERF_SAVER; }
        else if (!strcasecmp(pf, "balanced")) { o->has_profile = 1; o->profile = PERF_BALANCED; }
        else if (!strcasecmp(pf, "fast")) { o->has_profile = 1; o->profile = PERF_FAST; }
        else if (!strcasecmp(pf, "turbo")) { o->has_profile = 1; o->profile = PERF_TURBO; }
    }
    o->has_jobs = env_int("DNA_JOBS", 1, 32, &o->jobs);
    o->has_erofs_workers = env_int("DNA_EROFS_WORKERS", 1, 32, &o->erofs_workers);
    o->has_brotli_jobs = env_int("DNA_BROTLI_JOBS", 1, 32, &o->brotli_jobs);
    o->has_zstd_threads = env_int("DNA_ZSTD_THREADS", 1, 32, &o->zstd_threads);
    o->has_nice = env_int("DNA_NICE", -20, 19, &o->nice_level);
    o->has_reserve = env_int("DNA_MEM_RESERVE_MB", 0, 65536, &o->reserve_mb);
}

static perf_class_t perf_select_class(const perf_t *p) {
    int level = perf_thermal_level(p);
    uint64_t mib = 1024ULL;
    int avail_small = p->mem_avail_kb && p->mem_avail_kb < 512 * mib;
    if (p->mem_total_kb < 3 * 1024 * mib || avail_small || p->ncpu_online <= 2 || level >= 2) return PERF_SAVER;
    avail_small = p->mem_avail_kb && p->mem_avail_kb < 1536 * mib;
    if (p->mem_total_kb < 6 * 1024 * mib || p->ncpu_online <= 4 || avail_small || level >= 1) return PERF_BALANCED;
    avail_small = p->mem_avail_kb && p->mem_avail_kb < 2560 * mib;
    if (p->mem_total_kb < 8 * 1024 * mib || p->ncpu_online < 8 || avail_small) return PERF_FAST;
    if (p->mem_avail_kb >= 3 * 1024 * mib && level == 0) return PERF_TURBO;
    return PERF_FAST;
}

static void perf_make_plan(perf_t *p, const perf_load_t *ld, const perf_ovr_t *ov) {
    perf_class_t cls = ov->has_profile ? ov->profile : perf_select_class(p);
    int pj, tcpu, nice, reserve, br, zstd, emax;
    switch (cls) {
    case PERF_SAVER:    pj = 1; tcpu = p->cpu_budget < 2 ? p->cpu_budget : 2; nice = 10; reserve = 768;  br = 1; zstd = p->cpu_budget < 2 ? p->cpu_budget : 2; emax = 2; break;
    case PERF_FAST:     pj = 3; tcpu = p->cpu_budget < 8 ? p->cpu_budget : 8; nice = 0;  reserve = 1024; br = 3; zstd = p->ncpu_online - 1; emax = 8; break;
    case PERF_TURBO:    pj = 4; tcpu = p->cpu_budget < 12 ? p->cpu_budget : 12; nice = 0; reserve = 1024; br = 4; zstd = p->ncpu_online - 1; emax = 12; break;
    case PERF_BALANCED:
    case PERF_MANUAL:
    default:            pj = 2; tcpu = p->cpu_budget < 4 ? p->cpu_budget : 4; nice = 5; reserve = 768; br = 2; zstd = p->ncpu_online < 4 ? p->ncpu_online : 4; emax = 4; break;
    }
    if (zstd < 1) zstd = 1;
    if (cls == PERF_FAST || cls == PERF_TURBO) {
        zstd = p->ncpu_online - 1;
        if (zstd < 1) zstd = 1;
        int cap = cls == PERF_FAST ? 6 : 8;
        if (zstd > cap) zstd = cap;
    }
    int cpu_b = p->cpu_budget < tcpu ? p->cpu_budget : tcpu;
    if (cpu_b < 1) cpu_b = 1;
    p->cls = cls;
    p->total_cpu_threads = tcpu;
    p->nice_level = nice;
    p->reserve_mb = reserve;
    int n = ld->n_ext4 + ld->n_erofs + ld->n_f2fs;
    if (n == 0) {
        p->part_jobs = 1; p->erofs_workers = 1; p->brotli_jobs = 1; p->zstd_threads = zstd;
    } else {
        int storage_cap = p->storage == STORAGE_EMMC ? 2 : (p->storage == STORAGE_UFS ? 4 : 32);
        int part = pj < n ? pj : n;
        if (part > storage_cap) part = storage_cap;
        int erofs;
        if (n == 1 && ld->n_erofs == 1) { part = 1; erofs = cpu_b; }
        else {
            int half = cpu_b / 2; if (half < 1) half = 1;
            if (part > half) part = half;
            erofs = cpu_b / part; if (erofs < 1) erofs = 1;
        }
        if (erofs > emax) erofs = emax;
        uint64_t total_mb = p->mem_total_kb / 1024, avail_mb = p->mem_avail_kb / 1024;
        uint64_t cap_total = total_mb * 55 / 100;
        uint64_t budget;
        if (p->mem_avail_kb) {
            if (avail_mb > (uint64_t)reserve) budget = avail_mb - (uint64_t)reserve; else budget = 0;
            if (budget > cap_total) budget = cap_total;
        } else budget = cap_total;
        int per_job = 0;
        if (ld->n_ext4) per_job = 384;
        if (ld->n_f2fs && per_job < 384) per_job = 384;
        if (ld->n_erofs) { int e = 96 * erofs; if (e > per_job) per_job = e; }
        if (per_job < 1) per_job = 1;
        int mb_erofs = (int)(budget / 96); if (mb_erofs < 1) mb_erofs = 1;
        if (erofs > mb_erofs) erofs = mb_erofs;
        int mb_jobs = (int)(budget / (uint64_t)per_job); if (mb_jobs < 1) mb_jobs = 1;
        if (part > mb_jobs) part = mb_jobs;
        if (ld->n_erofs == n && part > 1) {
            int e = cpu_b / part; if (e < 1) e = 1;
            if (erofs > e) erofs = e;
        }
        if (erofs < 1) erofs = 1;
        if (part < 1) part = 1;
        p->part_jobs = part;
        p->erofs_workers = erofs;
        int mb_buf = (int)(budget / 96); if (mb_buf < 1) mb_buf = 1;
        int brb = br < n ? br : n;
        if (brb > mb_buf) brb = mb_buf;
        if (brb < 1) brb = 1;
        p->brotli_jobs = brb;
        p->zstd_threads = zstd;
    }
    if (ov->has_jobs) p->part_jobs = ov->jobs;
    if (ov->has_erofs_workers) p->erofs_workers = ov->erofs_workers;
    if (ov->has_brotli_jobs) p->brotli_jobs = ov->brotli_jobs;
    if (ov->has_zstd_threads) p->zstd_threads = ov->zstd_threads;
    if (ov->has_nice) p->nice_level = ov->nice_level;
    if (ov->has_reserve) p->reserve_mb = ov->reserve_mb;
    int maxn = n > 0 ? n : 1;
    if (p->part_jobs > maxn) p->part_jobs = maxn;
    if (p->part_jobs < 1) p->part_jobs = 1;
    if (p->erofs_workers < 1) p->erofs_workers = 1;
    if (p->brotli_jobs < 1) p->brotli_jobs = 1;
    if (p->zstd_threads < 1) p->zstd_threads = 1;
}


static const char *profile_name(perf_class_t c);
static const char *storage_name(storage_kind_t s);

/* dna repack <dirs,...> --read 1|0 --auto 1|0 --format 0|1 --type 0|1|2 --br N --erofs N --mode lz4hc|lz4|lzma
 *            --compress 0|1 --delavb 0|1 --hal-fallback 0|1 --convert 0|1|2   (convert: 0 ext4, 1 erofs, 2 f2fs) */
static int cmd_repack(int argc, char **argv) {
    ropts_t o = { 1, 1, 0, 0, 0, 0, 0, 0, 0, "lz4hc", 0 }; int conv = 0;
    char *names[128]; int nn = 0;
    perf_ovr_t ov; perf_ovr_from_env(&ov);
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; int hasv = i + 1 < argc;
        if (!strcmp(a, "--read") && hasv) o.rw = atoi(argv[++i]);
        else if (!strcmp(a, "--auto") && hasv) o.autosize = atoi(argv[++i]);
        else if (!strcmp(a, "--format") && hasv) o.sparse = atoi(argv[++i]);
        else if (!strcmp(a, "--type") && hasv) o.out_kind = atoi(argv[++i]);
        else if (!strcmp(a, "--br") && hasv) o.brlvl = atoi(argv[++i]);
        else if (!strcmp(a, "--erofs") && hasv) o.erofslvl = atoi(argv[++i]);
        else if (!strcmp(a, "--mode") && hasv) snprintf(o.mode, sizeof o.mode, "%s", argv[++i]);
        else if (!strcmp(a, "--compress")) o.shrink = flag_val(argc, argv, &i);
        else if (!strcmp(a, "--delavb")) o.delavb = flag_val(argc, argv, &i);
        else if (!strcmp(a, "--hal-fallback")) o.hal_fallback = flag_val(argc, argv, &i);
        else if (!strcmp(a, "--convert") && hasv) conv = atoi(argv[++i]);
        else if (!strcmp(a, "--profile") && hasv) {
            const char *v = argv[++i];
            if (!strcmp(v, "auto")) ov.has_profile = 0;
            else if (!strcmp(v, "saver")) { ov.has_profile = 1; ov.profile = PERF_SAVER; }
            else if (!strcmp(v, "balanced")) { ov.has_profile = 1; ov.profile = PERF_BALANCED; }
            else if (!strcmp(v, "fast")) { ov.has_profile = 1; ov.profile = PERF_FAST; }
            else if (!strcmp(v, "turbo")) { ov.has_profile = 1; ov.profile = PERF_TURBO; }
            else { fprintf(stderr, "> Error: unknown profile '%s'\n", v); return 2; }
        }
        else if (!strcmp(a, "--jobs") && hasv) { int v = atoi(argv[++i]); if (v >= 1 && v <= 32) { ov.jobs = v; ov.has_jobs = 1; } }
        else if (!strcmp(a, "--erofs-workers") && hasv) { int v = atoi(argv[++i]); if (v >= 1 && v <= 32) { ov.erofs_workers = v; ov.has_erofs_workers = 1; } }
        else if (!strcmp(a, "--brotli-jobs") && hasv) { int v = atoi(argv[++i]); if (v >= 1 && v <= 32) { ov.brotli_jobs = v; ov.has_brotli_jobs = 1; } }
        else if (!strcmp(a, "--nice") && hasv) { int v = atoi(argv[++i]); if (v >= -20 && v <= 19) { ov.nice_level = v; ov.has_nice = 1; } }
        else if (!strcmp(a, "--mem-reserve-mb") && hasv) { int v = atoi(argv[++i]); if (v >= 0 && v <= 65536) { ov.reserve_mb = v; ov.has_reserve = 1; } }
        else if (!strcmp(a, "--thermal-limit") && hasv) { int v = atoi(argv[++i]); if (v >= 20 && v <= 120) g_thermal_limit_hot = v; }
        else if (a[0] != '-') for (char *t = strtok(argv[i], ", \t\n"); t && nn < 128; t = strtok(NULL, ", \t\n")) names[nn++] = t;
    }
    const char *dro = getenv("DNA_DRO"), *pro = getenv("DNA_PRO");
    if (!dro || !*dro || !pro || !*pro) { fprintf(stderr, "> Error: project is not selected\n"); return 1; }
    if (!nn) { fprintf(stderr, "usage: dna repack <dir[,dir]> [--read 0|1] [--auto 0|1] [--format 0|1] [--type 0|1|2] [--br N] [--erofs N] [--mode lz4hc|lz4|lzma] [--compress 0|1] [--delavb 0|1] [--hal-fallback 0|1] [--convert 0|1|2] [--profile auto|saver|balanced|fast|turbo] [--jobs N] [--erofs-workers N] [--brotli-jobs N] [--nice N] [--mem-reserve-mb N] [--thermal-limit C]\n"); return 2; }
    { char od[4400]; snprintf(od, sizeof od, "%s/out", pro);
      if (mkdir_p(od)) { fprintf(stderr, "> Error: cannot create %s\n", od); return 1; } }
    perf_load_t ld = { 0, 0, 0, 0, o.out_kind };
    for (int i = 0; i < nn; i++) {
        char *n = names[i]; size_t l = strlen(n); while (l && n[l - 1] == '/') n[--l] = 0;
        if (!strcmp(n, "config") || !*n) continue;
        char tree[4400], hdr[4600]; snprintf(tree, sizeof tree, "%s/%s", dro, n); snprintf(hdr, sizeof hdr, "%s/header", tree);
        struct stat sb; if (stat(tree, &sb) || !S_ISDIR(sb.st_mode)) continue;
        if (!access(hdr, R_OK)) ld.n_boot++;
        else if (conv == 1) ld.n_erofs++;
        else if (conv == 2) ld.n_f2fs++;
        else ld.n_ext4++;
    }
    perf_t perf; perf_detect(&perf);
    perf_make_plan(&perf, &ld, &ov);
    int tmax = perf.temp_battery_c;
    if (perf.temp_skin_c > tmax) tmax = perf.temp_skin_c;
    if (perf.temp_soc_c > tmax) tmax = perf.temp_soc_c;
    printf("> Performance: %s (RAM %.1f/%.1f GiB, free %.1f GiB, %d cores, %s, ",
           profile_name(perf.cls), (double)perf.mem_avail_kb / 1048576.0, (double)perf.mem_total_kb / 1048576.0,
           (double)perf.free_pro_bytes / 1073741824.0, perf.ncpu_online, storage_name(perf.storage));
    if (tmax) printf("%d°C)\n", tmax); else printf("n/a)\n");
    printf("> Plan: %d jobs, erofs workers %d/job, nice %d, reserve %.1f GiB\n",
           perf.part_jobs, perf.erofs_workers, perf.nice_level, (double)perf.reserve_mb / 1024.0);
    if (ld.n_erofs > 0) {
        perf_load_t seq = ld;
        seq.n_erofs = 1;
        perf_t one = perf;
        perf_make_plan(&one, &seq, &ov);
        o.erofs_workers = one.erofs_workers;
    }
    g_repack_quiet = !g_debug;
    int fail = 0;
    for (int i = 0; i < nn; i++) {
        char *n = names[i]; size_t l = strlen(n); while (l && n[l - 1] == '/') n[--l] = 0;
        if (!strcmp(n, "config") || !*n) continue;
        char tree[4400], hdr[4600]; snprintf(tree, sizeof tree, "%s/%s", dro, n); snprintf(hdr, sizeof hdr, "%s/header", tree);
        struct stat sb; if (stat(tree, &sb) || !S_ISDIR(sb.st_mode)) { fprintf(stderr, "> Error: %s not found\n", tree); fail = 1; continue; }
        int is_boot = !access(hdr, R_OK);
        const char *fmt = is_boot ? "boot" : (conv == 1 ? "erofs" : (conv == 2 ? "f2fs" : "ext4"));
        printf("> Packing: %s <%s>\n", n, fmt); fflush(stdout);
        part_start();
        int rc;
        if (is_boot) rc = repack_boot(n, dro, pro);
        else {
            if (o.delavb) delavb_tree(tree, 0);
            char cfgd[4400], fsc[4600], fcx[4600];
            snprintf(cfgd, sizeof cfgd, "%s/config", dro);
            snprintf(fsc, sizeof fsc, "%s/%s_fs_config", cfgd, n);
            snprintf(fcx, sizeof fcx, "%s/%s_file_contexts", cfgd, n);
            if (!access(fsc, R_OK)) {
                size_t stale = fs_config_prune(tree, n, fsc);
                fs_config_complete(tree, n, fsc);
                if (stale) VPRINTF("> fs_config: %zu stale entries removed\n", stale);
            }
            if (!access(fcx, R_OK)) file_contexts_sync(dro, n, tree, fcx, o.hal_fallback);
            if (conv == 1) rc = repack_erofs(n, dro, pro, &o);
            else if (conv == 2) rc = repack_f2fs(n, dro, pro, &o);
            else rc = repack_ext4(n, dro, pro, &o);
        }
        if (rc) { fprintf(stderr, "> Failed: %s\n", n); fail = 1; }
    }
    return fail ? 1 : 0;
}

/* ---------- SHA-256 (liblp checksums) ---------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; } sha_t;

static const uint32_t SHA_K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha_t *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256(const void *data, size_t len, uint8_t out[32]) {
    sha_t c = {{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}, len, {0}};
    const uint8_t *p = data; size_t rest = len;
    while (rest >= 64) { sha_block(&c, p); p += 64; rest -= 64; }
    memset(c.buf, 0, 64); memcpy(c.buf, p, rest); c.buf[rest] = 0x80;
    if (rest >= 56) { sha_block(&c, c.buf); memset(c.buf, 0, 64); }
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) c.buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_block(&c, c.buf);
    for (int i = 0; i < 8; i++) { out[i*4] = c.h[i] >> 24; out[i*4+1] = c.h[i] >> 16; out[i*4+2] = c.h[i] >> 8; out[i*4+3] = (uint8_t)c.h[i]; }
}

/* ---------- output sink: raw (holes) or sparse, written strictly in order ---------- */

#define SINK_MAX_RAW_BLOCKS 1048575u

typedef struct {
    FILE *o; int sparse; uint64_t blocks; uint32_t nchunks; int err;
    int run; uint32_t fillv, rblocks; off_t hdr;          /* open run: 1 = raw data, 2 = fill */
} sink_t;

static void sk_chunk(sink_t *k, uint16_t type, uint32_t blocks, uint32_t payload, int with_fill, uint32_t fill) {
    uint8_t ch[12] = {0}; ch[0] = type & 0xFF; ch[1] = type >> 8;
    uint32_t tot = 12 + payload + (with_fill ? 4 : 0); memcpy(ch + 4, &blocks, 4); memcpy(ch + 8, &tot, 4);
    if (fwrite(ch, 1, 12, k->o) != 12) k->err = 1;
    if (with_fill && fwrite(&fill, 4, 1, k->o) != 1) k->err = 1;
    k->nchunks++;
}

/* finish the open run: patch its chunk header with the final block count */
static void sk_close(sink_t *k) {
    if (!k->run) return;
    if (k->sparse) {
        uint32_t tot = 12 + (k->run == 1 ? k->rblocks * 4096u : 4u);
        off_t end = ftello(k->o);
        if (fseeko(k->o, k->hdr + 4, SEEK_SET) || fwrite(&k->rblocks, 4, 1, k->o) != 1 || fwrite(&tot, 4, 1, k->o) != 1 || fseeko(k->o, end, SEEK_SET)) k->err = 1;
    }
    k->run = 0;
}

static void sk_skip(sink_t *k, uint64_t len, uint16_t sparse_type) {   /* gap or explicit zeros */
    sk_close(k);
    uint64_t nb = len / 4096; k->blocks += nb;
    if (!nb) return;
    if (k->sparse) { while (nb) { uint32_t n = nb > 0x7FFFFFF ? 0x7FFFFFF : (uint32_t)nb; sk_chunk(k, sparse_type, n, 0, sparse_type == 0xCAC2, 0); nb -= n; } }
    else if (fseeko(k->o, (off_t)len, SEEK_CUR)) k->err = 1;
}
static void sk_gap(sink_t *k, uint64_t len)  { sk_skip(k, len, 0xCAC3); }   /* untouched on flash */
static void sk_zero(sink_t *k, uint64_t len) { sk_skip(k, len, 0xCAC2); }   /* explicit zeros */

static void sk_raw(sink_t *k, const uint8_t *buf, uint64_t len) {            /* metadata: written as is */
    sk_close(k);
    uint64_t nb = len / 4096; k->blocks += nb;
    if (!nb) return;
    if (k->sparse) sk_chunk(k, 0xCAC1, (uint32_t)nb, (uint32_t)len, 0, 0);
    if (fwrite(buf, 1, len, k->o) != len) k->err = 1;
}

/* One partition block. Like libsparse: a block made of one repeated 32-bit value becomes a FILL chunk
 * (merged with neighbours of the same value), anything else is raw data (merged too). */
static void sk_block(sink_t *k, const uint8_t *p) {
    const uint32_t *w = (const uint32_t *)(const void *)p;
    int uniform = 1; for (int i = 1; i < 1024; i++) if (w[i] != w[0]) { uniform = 0; break; }
    k->blocks++;
    if (uniform) {
        uint32_t v = w[0];
        if (k->run == 2 && k->fillv == v) { k->rblocks++; }
        else { sk_close(k); k->run = 2; k->fillv = v; k->rblocks = 1; k->hdr = ftello(k->o); if (k->sparse) sk_chunk(k, 0xCAC2, 1, 0, 1, v); }
        if (!k->sparse) { if (v == 0) { if (fseeko(k->o, 4096, SEEK_CUR)) k->err = 1; } else if (fwrite(p, 1, 4096, k->o) != 4096) k->err = 1; }
    } else {
        if (k->run == 1 && k->rblocks >= SINK_MAX_RAW_BLOCKS) sk_close(k);
        if (k->run == 1) k->rblocks++;
        else { sk_close(k); k->run = 1; k->rblocks = 1; k->hdr = ftello(k->o); if (k->sparse) sk_chunk(k, 0xCAC1, 1, 4096, 0, 0); }
        if (fwrite(p, 1, 4096, k->o) != 4096) k->err = 1;
    }
}

static void sk_data(sink_t *k, const uint8_t *buf, uint64_t len) {
    for (uint64_t i = 0; i + 4096 <= len && !k->err; i += 4096) sk_block(k, buf + i);
}

static int sk_finish(sink_t *k, uint64_t total) {
    sk_close(k);
    uint64_t want = total / 4096;
    if (k->blocks < want) sk_gap(k, (want - k->blocks) * 4096);
    if (k->sparse) {
        uint32_t magic = 0xED26FF3A, bsz = 4096, tb = (uint32_t)want, zero32 = 0;
        uint16_t maj = 1, min = 0, fh = 28, ch = 12;
        if (fseeko(k->o, 0, SEEK_SET)) return -1;
        fwrite(&magic, 4, 1, k->o); fwrite(&maj, 2, 1, k->o); fwrite(&min, 2, 1, k->o); fwrite(&fh, 2, 1, k->o);
        fwrite(&ch, 2, 1, k->o); fwrite(&bsz, 4, 1, k->o); fwrite(&tb, 4, 1, k->o); fwrite(&k->nchunks, 4, 1, k->o); fwrite(&zero32, 4, 1, k->o);
    } else if (ftruncate(fileno(k->o), (off_t)total)) k->err = 1;
    return k->err ? -1 : 0;
}

/* ---------- lpmake: build a super image (liblp format) natively ---------- */

#define LP_ALIGN (1024ull * 1024ull)
#define LP_META_MAX 65536u

typedef struct { char name[40]; char path[4400]; uint64_t size; int group; uint64_t start; } lpp_t;
typedef struct { char base[40]; char slot; char path[4400]; uint64_t size; } lpi_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static int lp_not_dynamic(const char *b) {
    static const char *skip[] = { "boot", "vendor_boot", "init_boot", "dtbo", "recovery", "super", "vbmeta", "userdata", "cache", "misc", NULL };
    for (int i = 0; skip[i]; i++) if (!strcmp(b, skip[i])) return 1;
    return !strncmp(b, "vbmeta", 6);
}

static int cmd_lpmake(int argc, char **argv) {
    const char *type = "VAB", *group = "qti_dynamic_partitions"; int sparse = 0; double sz_in = 8.5;
    lpi_t *im = calloc(256, sizeof *im); int nim = 0;
    for (int i = 0; i < argc; i++) {
        int hv = i + 1 < argc;
        if (!strcmp(argv[i], "--type") && hv) type = argv[++i];
        else if (!strcmp(argv[i], "--format") && hv) sparse = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--super_size") && hv) sz_in = atof(argv[++i]);
        else if (!strcmp(argv[i], "--super_group") && hv) group = argv[++i];
        else if (argv[i][0] != '-') {
            for (char *t = strtok(argv[i], ", \t\n"); t && nim < 256; t = strtok(NULL, ", \t\n")) {
                lpi_t *x = &im[nim];
                snprintf(x->path, sizeof x->path, "%s", t);
                if (access(x->path, R_OK) && getenv("DNA_PRO")) snprintf(x->path, sizeof x->path, "%s/%s", getenv("DNA_PRO"), t);
                if (access(x->path, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", t); free(im); return 1; }
                base_name(t, x->base, sizeof x->base);
                size_t l = strlen(x->base); x->slot = 0;
                if (l > 2 && x->base[l-2] == '_' && (x->base[l-1] == 'a' || x->base[l-1] == 'b')) { x->slot = x->base[l-1]; x->base[l-2] = 0; }
                if (lp_not_dynamic(x->base)) { fprintf(stderr, "> Warning: %s is not a dynamic partition, skipped\n", t); continue; }
                img_t pi; if (img_open(&pi, x->path)) { fprintf(stderr, "> Error: cannot read %s\n", t); img_close(&pi); free(im); return 1; }
                x->size = align_up(pi.size, 4096); img_close(&pi);
                nim++;
            }
        }
    }
    int ab = strcmp(type, "A");
    if (strcmp(type, "A") && strcmp(type, "AB") && strcmp(type, "VAB")) { fprintf(stderr, "> Error: --type must be A, AB or VAB\n"); free(im); return 2; }
    if (!nim) { fprintf(stderr, "> Error: no images to pack\nusage: dna lpmake --type A|AB|VAB --format 0|1 --super_size GiB --super_group NAME img[,img]\n"); free(im); return 2; }
    uint64_t super = (uint64_t)(sz_in < 4096 ? sz_in * 1073741824.0 : sz_in) & ~4095ull;
    uint32_t slots = !strcmp(type, "A") ? 2 : 3;      /* matches the original dna: A-only 2, AB and virtual AB 3 */

    lpp_t *pp = calloc(520, sizeof *pp); int np = 0;
    if (!ab) {
        for (int i = 0; i < nim; i++) {
            for (int j = 0; j < np; j++) if (!strcmp(pp[j].name, im[i].base)) { fprintf(stderr, "> Error: duplicate partition %s (use one slot for A-only)\n", im[i].base); free(im); free(pp); return 1; }
            snprintf(pp[np].name, sizeof pp[np].name, "%s", im[i].base); snprintf(pp[np].path, sizeof pp[np].path, "%s", im[i].path);
            pp[np].size = im[i].size; pp[np].group = 1; np++;
        }
    } else {
        char seen[256][40]; int ns = 0;
        for (int i = 0; i < nim; i++) {
            int dup = 0; for (int j = 0; j < ns; j++) if (!strcmp(seen[j], im[i].base)) dup = 1;
            if (dup) continue;
            snprintf(seen[ns++], 40, "%s", im[i].base);
            lpi_t *sa = NULL, *sb = NULL;
            for (int j = 0; j < nim; j++) if (!strcmp(im[j].base, im[i].base)) { if (im[j].slot == 'b') sb = &im[j]; else if (!sa) sa = &im[j]; }
            lpp_t *a = &pp[np++], *b = &pp[np++];
            snprintf(a->name, sizeof a->name, "%s_a", im[i].base); snprintf(b->name, sizeof b->name, "%s_b", im[i].base);
            a->group = 1; b->group = 2;
            if (sa) { snprintf(a->path, sizeof a->path, "%s", sa->path); a->size = sa->size; }
            if (sb) { snprintf(b->path, sizeof b->path, "%s", sb->path); b->size = sb->size; }
        }
    }
    for (int i = 0; i < np; i++) if (strlen(pp[i].name) > 35) { fprintf(stderr, "> Error: partition name too long: %s\n", pp[i].name); free(im); free(pp); return 1; }

    uint64_t reserved = 12288 + 2ull * slots * LP_META_MAX, first_data = align_up(reserved, LP_ALIGN), cursor = first_data;
    int ne = 0;
    for (int i = 0; i < np; i++) if (pp[i].size) { pp[i].start = align_up(cursor, LP_ALIGN); cursor = pp[i].start + pp[i].size; ne++; }
    if (cursor > super) {
        fprintf(stderr, "> Error: partitions need %llu MiB but super is %llu MiB. Increase the super size.\n",
                (unsigned long long)(cursor >> 20), (unsigned long long)(super >> 20));
        free(im); free(pp); return 1;
    }

    int ng = ab ? 3 : 2;
    uint32_t hsz = !strcmp(type, "VAB") ? 256 : 128;
    uint32_t tsz = (uint32_t)(np * 52 + ne * 24 + ng * 48 + 64);
    uint8_t *tb = calloc(1, tsz), *p = tb;
    int ei = 0;
    for (int i = 0; i < np; i++, p += 52) {
        memcpy(p, pp[i].name, strlen(pp[i].name));
        put32(p + 36, 0); put32(p + 40, ei); put32(p + 44, pp[i].size ? 1 : 0); put32(p + 48, pp[i].group);
        if (pp[i].size) ei++;
    }
    for (int i = 0; i < np; i++) if (pp[i].size) { put64(p, pp[i].size / 512); put32(p + 8, 0); put64(p + 12, pp[i].start / 512); put32(p + 20, 0); p += 24; }
    memcpy(p, "default", 7); p += 48;
    if (!ab) { memcpy(p, group, strlen(group) < 35 ? strlen(group) : 35); put64(p + 40, super); p += 48; }
    else for (int g = 0; g < 2; g++) {
        char gn[64]; snprintf(gn, sizeof gn, "%s_%c", group, g ? 'b' : 'a');
        memcpy(p, gn, strlen(gn) < 35 ? strlen(gn) : 35); put64(p + 40, super); p += 48;
    }
    put64(p, first_data / 512); put32(p + 8, (uint32_t)LP_ALIGN); put32(p + 12, 0); put64(p + 16, super); memcpy(p + 24, "super", 5); put32(p + 60, 0);

    uint8_t hd[256] = {0};
    put32(hd, 0x414C5030); put16(hd + 4, 10); put16(hd + 6, hsz == 256 ? 2 : 0); put32(hd + 8, hsz);
    put32(hd + 44, tsz); sha256(tb, tsz, hd + 48);
    put32(hd + 80, 0); put32(hd + 84, np); put32(hd + 88, 52);
    put32(hd + 92, np * 52); put32(hd + 96, ne); put32(hd + 100, 24);
    put32(hd + 104, np * 52 + ne * 24); put32(hd + 108, ng); put32(hd + 112, 48);
    put32(hd + 116, np * 52 + ne * 24 + ng * 48); put32(hd + 120, 1); put32(hd + 124, 64);
    if (hsz == 256) put32(hd + 128, 1);                       /* LP_HEADER_FLAG_VIRTUAL_AB_DEVICE */
    sha256(hd, hsz, hd + 12);

    uint8_t *md = calloc(1, reserved), geo[52] = {0};
    put32(geo, 0x616c4467); put32(geo + 4, 52); put32(geo + 40, LP_META_MAX); put32(geo + 44, slots); put32(geo + 48, 4096);
    { uint8_t tmp[52]; memcpy(tmp, geo, 52); memset(tmp + 8, 0, 32); sha256(tmp, 52, geo + 8); }
    memcpy(md + 4096, geo, 52); memcpy(md + 8192, geo, 52);
    for (uint32_t s = 0; s < slots * 2; s++) { memcpy(md + 12288 + (uint64_t)s * LP_META_MAX, hd, hsz); memcpy(md + 12288 + (uint64_t)s * LP_META_MAX + hsz, tb, tsz); }

    char od[4400], final[4600], tmpf[4700];
    const char *pro = getenv("DNA_PRO");
    snprintf(od, sizeof od, "%s/out", pro && *pro ? pro : ".");
    if (mkdir_p(od)) { fprintf(stderr, "> Error: cannot create %s\n", od); return 1; }
    snprintf(final, sizeof final, "%s/super.img", od); snprintf(tmpf, sizeof tmpf, "%s.tmp", final);
    sink_t k; memset(&k, 0, sizeof k); k.o = fopen(tmpf, "wb"); k.sparse = sparse;
    if (!k.o) { fprintf(stderr, "> Error: cannot write %s\n", tmpf); return 1; }
    if (sparse) { uint8_t z[28] = {0}; if (fwrite(z, 1, 28, k.o) != 28) k.err = 1; }
    printf("> Building super (%s, %s): %llu MiB, %d partitions\n", type, sparse ? "sparse" : "raw", (unsigned long long)(super >> 20), np);
    sk_zero(&k, 4096);
    sk_raw(&k, md + 4096, reserved - 4096);
    uint64_t cur = reserved;
    uint8_t *buf = malloc(1 << 20);
    for (int i = 0; i < np && !k.err; i++) {
        if (!pp[i].size) continue;
        sk_gap(&k, pp[i].start - cur);
        printf("> Adding: %s (%llu KiB)\n", pp[i].name, (unsigned long long)(pp[i].size >> 10)); fflush(stdout);
        img_t pi; if (img_open(&pi, pp[i].path)) { k.err = 1; break; }
        for (uint64_t off = 0; off < pp[i].size && !k.err; off += 1 << 20) {
            uint64_t n = pp[i].size - off; if (n > (1 << 20)) n = 1 << 20;
            memset(buf, 0, n);
            uint64_t have = off < pi.size ? pi.size - off : 0; if (have > n) have = n;
            if (have && !img_read(&pi, off, buf, have)) { k.err = 1; break; }
            sk_data(&k, buf, n);
        }
        img_close(&pi);
        cur = pp[i].start + pp[i].size;
    }
    free(buf);
    int rc = k.err ? -1 : sk_finish(&k, super);
    if (fclose(k.o)) rc = -1;
    free(im); free(pp); free(tb); free(md);
    if (rc) { fprintf(stderr, "> Error: failed to write super image (disk full?)\n"); remove(tmpf); return 1; }
    if (!access(final, F_OK)) printf("> Replacing existing: %s\n", final);
    if (rename(tmpf, final)) { fprintf(stderr, "> Error: cannot write %s\n", final); return 1; }
    printf("> Done: %s\n", final);
    return 0;
}

/* ---------- payload.bin dumper ---------- */

/* update_engine payload: CrAU header + DeltaArchiveManifest (protobuf). */

typedef struct { uint64_t start, blocks; } pext_t;
typedef struct {
    int type;
    uint64_t data_off, data_len, dst_len;
    pext_t *ext; uint32_t next, extcap;
} pop_t;
typedef struct {
    char *name;
    pop_t *ops; uint32_t nops, opcap;
} ppart_t;
typedef struct {
    uint64_t block_size, data_start, file_size;
    char path[4400];
    ppart_t *parts; uint32_t nparts, partcap;
} pay_t;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static int pb_varint(const uint8_t **pp, const uint8_t *end, uint64_t *v) {
    const uint8_t *p = *pp;
    uint64_t r = 0; int shift = 0;
    while (p < end && shift < 64) {
        uint8_t b = *p++;
        r |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *pp = p; *v = r; return 0; }
        shift += 7;
    }
    return -1;
}

static int pb_skip(const uint8_t **pp, const uint8_t *end, uint32_t wire) {
    uint64_t n;
    const uint8_t *p = *pp;
    if (wire == 0) { if (pb_varint(&p, end, &n)) return -1; }
    else if (wire == 1) { if ((size_t)(end - p) < 8) return -1; p += 8; }
    else if (wire == 2) {
        if (pb_varint(&p, end, &n) || n > (uint64_t)(end - p)) return -1;
        p += n;
    } else if (wire == 5) { if ((size_t)(end - p) < 4) return -1; p += 4; }
    else return -1;
    *pp = p;
    return 0;
}

static int pb_field(const uint8_t **pp, const uint8_t *end, uint64_t *field, uint32_t *wire) {
    uint64_t key;
    if (pb_varint(pp, end, &key)) return -1;
    *field = key >> 3; *wire = (uint32_t)(key & 7);
    return *field ? 0 : -1;
}

static int pext_add(pop_t *op, uint64_t start, uint64_t blocks) {
    if (op->next == op->extcap) {
        uint32_t nc = op->extcap ? op->extcap * 2 : 4;
        pext_t *n = realloc(op->ext, (size_t)nc * sizeof *n);
        if (!n) return -1;
        op->ext = n; op->extcap = nc;
    }
    op->ext[op->next].start = start;
    op->ext[op->next].blocks = blocks;
    op->next++;
    return 0;
}

static int pay_parse_extent(const uint8_t *p, const uint8_t *end, pop_t *op) {
    uint64_t start = 0, blocks = 0;
    while (p < end) {
        uint64_t f; uint32_t w;
        if (pb_field(&p, end, &f, &w)) return -1;
        if (f == 1 && w == 0) { if (pb_varint(&p, end, &start)) return -1; }
        else if (f == 2 && w == 0) { if (pb_varint(&p, end, &blocks)) return -1; }
        else if (pb_skip(&p, end, w)) return -1;
    }
    return pext_add(op, start, blocks);
}

static int pay_parse_op(const uint8_t *p, const uint8_t *end, pop_t *op) {
    op->type = -1;
    while (p < end) {
        uint64_t f; uint32_t w;
        if (pb_field(&p, end, &f, &w)) return -1;
        if (f == 1 && w == 0) { uint64_t v; if (pb_varint(&p, end, &v)) return -1; op->type = (int)v; }
        else if (f == 2 && w == 0) { if (pb_varint(&p, end, &op->data_off)) return -1; }
        else if (f == 3 && w == 0) { if (pb_varint(&p, end, &op->data_len)) return -1; }
        else if (f == 6 && w == 2) {
            uint64_t n; if (pb_varint(&p, end, &n) || n > (uint64_t)(end - p)) return -1;
            if (pay_parse_extent(p, p + n, op)) return -1;
            p += n;
        }
        else if (f == 7 && w == 0) { if (pb_varint(&p, end, &op->dst_len)) return -1; }
        else if (pb_skip(&p, end, w)) return -1;
    }
    return op->type >= 0 ? 0 : -1;
}

static int pay_parse_part(const uint8_t *p, const uint8_t *end, pay_t *py) {
    ppart_t part; memset(&part, 0, sizeof part);
    while (p < end) {
        uint64_t f; uint32_t w;
        if (pb_field(&p, end, &f, &w)) goto bad;
        if (f == 1 && w == 2) {
            uint64_t n; if (pb_varint(&p, end, &n) || n > (uint64_t)(end - p) || n > 4095) goto bad;
            free(part.name); part.name = malloc((size_t)n + 1);
            if (!part.name) goto bad;
            memcpy(part.name, p, (size_t)n); part.name[n] = 0; p += n;
        } else if (f == 8 && w == 2) {
            uint64_t n; if (pb_varint(&p, end, &n) || n > (uint64_t)(end - p)) goto bad;
            if (part.nops == part.opcap) {
                uint32_t nc = part.opcap ? part.opcap * 2 : 8;
                pop_t *no = realloc(part.ops, (size_t)nc * sizeof *no);
                if (!no) goto bad;
                part.ops = no; part.opcap = nc;
            }
            memset(&part.ops[part.nops], 0, sizeof part.ops[part.nops]);
            if (pay_parse_op(p, p + n, &part.ops[part.nops])) goto bad;
            part.nops++; p += n;
        } else if (pb_skip(&p, end, w)) goto bad;
    }
    if (!part.name || !*part.name) goto bad;
    if (strchr(part.name, '/') || !strcmp(part.name, ".") || !strcmp(part.name, "..")) goto bad;
    if (py->nparts == py->partcap) {
        uint32_t nc = py->partcap ? py->partcap * 2 : 8;
        ppart_t *np = realloc(py->parts, (size_t)nc * sizeof *np);
        if (!np) goto bad;
        py->parts = np; py->partcap = nc;
    }
    py->parts[py->nparts++] = part;
    return 0;
bad:
    free(part.name);
    for (uint32_t i = 0; i < part.nops; i++) free(part.ops[i].ext);
    free(part.ops);
    return -1;
}

static int pay_parse_manifest(pay_t *py, const uint8_t *p, size_t len) {
    const uint8_t *end = p + len;
    py->block_size = 4096;
    while (p < end) {
        uint64_t f; uint32_t w;
        if (pb_field(&p, end, &f, &w)) return -1;
        if (f == 3 && w == 0) { if (pb_varint(&p, end, &py->block_size)) return -1; }
        else if (f == 13 && w == 2) {
            uint64_t n; if (pb_varint(&p, end, &n) || n > (uint64_t)(end - p)) return -1;
            if (pay_parse_part(p, p + n, py)) return -1;
            p += n;
        } else if (pb_skip(&p, end, w)) return -1;
    }
    return py->nparts && py->block_size ? 0 : -1;
}

static void pay_free(pay_t *py) {
    for (uint32_t i = 0; i < py->nparts; i++) {
        for (uint32_t j = 0; j < py->parts[i].nops; j++) free(py->parts[i].ops[j].ext);
        free(py->parts[i].ops); free(py->parts[i].name);
    }
    free(py->parts); memset(py, 0, sizeof *py);
}

static int pay_load(pay_t *py, const char *path) {
    memset(py, 0, sizeof *py);
    snprintf(py->path, sizeof py->path, "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseeko(f, 0, SEEK_END)) { fclose(f); return -1; }
    off_t sz = ftello(f); py->file_size = (uint64_t)sz;
    uint8_t h[24];
    if (sz < 20 || !rd(f, 0, h, 20) || memcmp(h, "CrAU", 4)) { fclose(f); return -1; }
    uint64_t ver = be64(h + 4), msize = be64(h + 12), sig = 0;
    size_t hdr = 20;
    if (ver >= 2) {
        if (sz < 24 || !rd(f, 20, h + 20, 4)) { fclose(f); return -1; }
        sig = be32(h + 20); hdr = 24;
    }
    if (ver < 1 || ver > 2 || msize > (uint64_t)sz - hdr ||
        sig > (uint64_t)sz - hdr - msize || msize > (256u << 20)) { fclose(f); return -1; }
    uint8_t *man = malloc((size_t)msize + 1);
    if (!man) { fclose(f); return -1; }
    if (!rd(f, (long)hdr, man, (size_t)msize)) { free(man); fclose(f); return -1; }
    int ok = pay_parse_manifest(py, man, (size_t)msize);
    free(man); fclose(f);
    if (ok) { pay_free(py); return -1; }
    py->data_start = hdr + msize + sig;
    return 0;
}

static uint64_t ext_bytes(const pext_t *e, uint32_t n, uint64_t bs) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) total += e[i].blocks * bs;
    return total;
}

static uint64_t meminfo_kb(const char *key) {
    char path[4096];
    FILE *f = fopen(syspath(path, sizeof path, "/proc/meminfo"), "r");
    if (!f) return 0;
    char line[256];
    size_t kl = strlen(key);
    uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, kl)) {
            const char *p = line + kl;
            while (*p == ':' || *p == ' ' || *p == '\t') p++;
            v = strtoull(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}


static int payload_workers(void) {
    const char *env = getenv("DNA_PAYLOAD_WORKERS");
    if (env && *env) {
        int n = atoi(env);
        if (n < 1) n = 1;
        if (n > 8) n = 8;
        return n;
    }
    uint64_t total = meminfo_kb("MemTotal");
    uint64_t avail = meminfo_kb("MemAvailable");
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int n;
    if (total >= 5632ULL * 1024) n = 8;
    else if (total >= 3072ULL * 1024) n = 4;
    else n = 2;
    if (total && total < 2048ULL * 1024) n = 1;
    if (avail && avail < 256ULL * 1024) n = 1;
    if (ncpu > 0 && n > ncpu) n = (int)ncpu;
    if (n < 1) n = 1;
    return n;
}

static int grow_buf(uint8_t **buf, size_t *cap, size_t need) {
    if (*cap >= need) return 0;
    size_t nc = *cap ? *cap : 1;
    while (nc < need) nc *= 2;
    uint8_t *n = realloc(*buf, nc);
    if (!n) return -1;
    *buf = n; *cap = nc;
    return 0;
}

typedef struct {
    int fd;
    const pext_t *e;
    uint32_t n, i;
    uint64_t pos, bs;
} extw_fd_t;

static int extw_fd_put(extw_fd_t *w, const uint8_t *buf, size_t n) {
    while (n) {
        if (w->i >= w->n) return -1;
        uint64_t cap = w->e[w->i].blocks * w->bs;
        if (w->pos >= cap) { w->i++; w->pos = 0; continue; }
        uint64_t left = cap - w->pos;
        size_t k = n < left ? n : (size_t)left;
        uint64_t off = w->e[w->i].start * w->bs + w->pos;
        size_t done = 0;
        while (done < k) {
            ssize_t wr = pwrite(w->fd, buf + done, k - done, (off_t)(off + done));
            if (wr < 0) { if (errno == EINTR) continue; return -1; }
            if (wr == 0) return -1;
            done += (size_t)wr;
        }
        w->pos += k; buf += k; n -= k;
    }
    return 0;
}

static int extw_fd_done(extw_fd_t *w) {
    while (w->i < w->n && w->pos >= w->e[w->i].blocks * w->bs) { w->i++; w->pos = 0; }
    return w->i >= w->n;
}

static int read_payload_at(int fd, void *buf, size_t len, uint64_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int decompress_external(const char *prog, char *const argv[],
                               const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t out_cap, size_t *out_len) {
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p2c[0], 0); dup2(c2p[1], 1);
        close(p2c[0]); close(p2c[1]); close(c2p[0]); close(c2p[1]);
        execvp(prog, argv);
        _exit(127);
    }
    close(p2c[0]); close(c2p[1]);
    int fl = fcntl(p2c[1], F_GETFL, 0);
    if (fl >= 0) fcntl(p2c[1], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(c2p[0], F_GETFL, 0);
    if (fl >= 0) fcntl(c2p[0], F_SETFL, fl | O_NONBLOCK);

    size_t in_pos = 0, out_pos = 0;
    int in_open = 1, out_open = 1, bad = 0, overflow = 0;
    uint8_t sink[4096];
    while (!bad && (in_open || out_open)) {
        struct pollfd pf[2];
        pf[0].fd = in_open ? p2c[1] : -1;
        pf[0].events = POLLOUT; pf[0].revents = 0;
        pf[1].fd = out_open ? c2p[0] : -1;
        pf[1].events = POLLIN; pf[1].revents = 0;
        int pr = poll(pf, 2, -1);
        if (pr < 0) { if (errno == EINTR) continue; bad = 1; break; }
        if (pf[1].fd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t *dst = out_pos < out_cap ? out + out_pos : sink;
            size_t room = out_pos < out_cap ? out_cap - out_pos : sizeof sink;
            ssize_t n = read(c2p[0], dst, room);
            if (n > 0) {
                if (out_pos < out_cap) out_pos += (size_t)n;
                else overflow = 1;
            } else if (n == 0) {
                out_open = 0;
            } else if (errno != EINTR && errno != EAGAIN) {
                out_open = 0;
            }
        }
        if (!bad && pf[0].fd >= 0 && (pf[0].revents & (POLLOUT | POLLERR | POLLHUP))) {
            if (in_pos >= in_len) {
                close(p2c[1]); in_open = 0;
            } else {
                size_t k = in_len - in_pos;
                if (k > (1u << 20)) k = 1u << 20;
                ssize_t n = write(p2c[1], in + in_pos, k);
                if (n > 0) in_pos += (size_t)n;
                else if (n < 0 && errno != EINTR && errno != EAGAIN) bad = 1;
                if (!bad && in_pos >= in_len) { close(p2c[1]); in_open = 0; }
            }
        }
        if (pf[0].revents & POLLERR) {
            if (in_pos < in_len) bad = 1;
            if (in_open) { close(p2c[1]); in_open = 0; }
        }
        if (pf[1].revents & POLLERR) out_open = 0;
    }
    if (in_open) close(p2c[1]);
    if (out_open) close(c2p[0]);
    if (bad) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1; }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || overflow) return -1;
    *out_len = out_pos;
    return 0;
}

typedef struct {
    int pfd;
    uint8_t *inbuf; size_t incap;
    uint8_t *outbuf; size_t outcap;
    uint8_t *zbuf; size_t zcap;
#ifdef DNA_HAVE_XZ
    struct xz_dec *dec;
#endif
} worker_ctx_t;

static int process_op_fast(pay_t *py, pop_t *op, extw_fd_t *w, worker_ctx_t *c) {
    uint64_t out_bytes = ext_bytes(op->ext, op->next, py->block_size);
    if (out_bytes > SIZE_MAX) return -1;
    size_t out_len = (size_t)out_bytes;

    if (op->type == 6 || op->type == 7) {
        size_t need = out_len > (1u << 20) ? (1u << 20) : (out_len ? out_len : 1);
        if (grow_buf(&c->zbuf, &c->zcap, need)) return -1;
        memset(c->zbuf, 0, need);
        uint64_t done = 0;
        while (done < out_bytes) {
            size_t k = out_bytes - done > (1u << 20) ? (1u << 20) : (size_t)(out_bytes - done);
            if (extw_fd_put(w, c->zbuf, k)) return -1;
            done += k;
        }
        return 0;
    }

    if (op->data_off > py->file_size - py->data_start ||
        op->data_len > py->file_size - py->data_start - op->data_off) return -1;
    if (op->data_len > SIZE_MAX) return -1;
    size_t in_len = (size_t)op->data_len;

    if (op->type == 0) {
        size_t need = in_len > (1u << 20) ? (1u << 20) : (in_len ? in_len : 1);
        if (grow_buf(&c->inbuf, &c->incap, need)) return -1;
        uint64_t done = 0;
        while (done < op->data_len) {
            size_t k = op->data_len - done > (1u << 20) ? (1u << 20) : (size_t)(op->data_len - done);
            if (read_payload_at(c->pfd, c->inbuf, k, py->data_start + op->data_off + done)) return -1;
            if (extw_fd_put(w, c->inbuf, k)) return -1;
            done += k;
        }
        return 0;
    }

    if (grow_buf(&c->inbuf, &c->incap, in_len ? in_len : 1)) return -1;
    if (grow_buf(&c->outbuf, &c->outcap, out_len ? out_len : 1)) return -1;
    if (in_len && read_payload_at(c->pfd, c->inbuf, in_len, py->data_start + op->data_off)) return -1;

    int decoded = 0;
#ifdef DNA_HAVE_XZ
    if (op->type == 8) {
        struct xz_buf b = { c->inbuf, 0, in_len, c->outbuf, 0, out_len ? out_len : 1 };
        enum xz_ret ret = xz_dec_run(c->dec, &b);
        if (ret == XZ_STREAM_END && b.out_pos == out_len) decoded = 1;
    }
#endif
    if (!decoded) {
        const char *prog;
        char *argv[5];
        if (op->type == 8) {
            prog = "busybox";
            argv[0] = (char *)"busybox"; argv[1] = (char *)"xz"; argv[2] = (char *)"-dc"; argv[3] = NULL;
        } else if (op->type == 1) {
            prog = "busybox";
            argv[0] = (char *)"busybox"; argv[1] = (char *)"bzip2"; argv[2] = (char *)"-dc"; argv[3] = NULL;
        } else if (op->type == 11) {
            prog = "brotli";
            argv[0] = (char *)"brotli"; argv[1] = (char *)"-d"; argv[2] = (char *)"-c"; argv[3] = NULL;
        } else if (op->type == 15) {
            prog = "zstd";
            argv[0] = (char *)"zstd"; argv[1] = (char *)"-d"; argv[2] = (char *)"-c"; argv[3] = NULL;
        } else {
            return -1;
        }
        size_t got = 0;
        if (decompress_external(prog, argv, c->inbuf, in_len, c->outbuf, out_len, &got)) return -1;
        if (got != out_len) return -1;
    }
    if (out_len && extw_fd_put(w, c->outbuf, out_len)) return -1;
    return 0;
}

static int worker_run(pay_t *py, ppart_t *p, int pfd, int ofd, int id, int nworkers) {
    worker_ctx_t c;
    memset(&c, 0, sizeof c);
    c.pfd = pfd;
#ifdef DNA_HAVE_XZ
    c.dec = xz_dec_init(XZ_SINGLE, 0);
    if (!c.dec) return -1;
#endif
    int rc = 0;
    for (uint32_t i = (uint32_t)id; i < p->nops && !rc; i += (uint32_t)nworkers) {
        extw_fd_t w = { ofd, p->ops[i].ext, p->ops[i].next, 0, 0, py->block_size };
        rc = process_op_fast(py, &p->ops[i], &w, &c);
        if (!rc && !extw_fd_done(&w)) rc = -1;
    }
    free(c.inbuf); free(c.outbuf); free(c.zbuf);
#ifdef DNA_HAVE_XZ
    xz_dec_end(c.dec);
#endif
    return rc;
}

static int dumper_one(pay_t *py, ppart_t *p, int auto_unpack, int delete_img, int *auto_fail) {
#ifdef DNA_HAVE_XZ
    xz_crc32_init();
#ifdef XZ_USE_CRC64
    xz_crc64_init();
#endif
#endif
    char out[4700];
    char d[4400]; snprintf(d, sizeof d, "%s", py->path);
    char *sl = strrchr(d, '/');
    if (sl) *sl = 0; else strcpy(d, ".");
    snprintf(out, sizeof out, "%s/%s.img", d, p->name);

    for (uint32_t i = 0; i < p->nops; i++) {
        int t = p->ops[i].type;
        if (t != 0 && t != 1 && t != 6 && t != 7 && t != 8 && t != 11 && t != 15) {
            fprintf(stderr, "> Error: %s uses unsupported incremental operation %d\n", p->name, t);
            return 1;
        }
    }

    printf("> Extracting: %s\n", p->name); fflush(stdout);
    int ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) { fprintf(stderr, "> Error: cannot write %s\n", out); return 1; }
    uint64_t max_end = 0;
    for (uint32_t i = 0; i < p->nops; i++) {
        pop_t *op = &p->ops[i];
        for (uint32_t j = 0; j < op->next; j++) {
            uint64_t e = (op->ext[j].start + op->ext[j].blocks) * py->block_size;
            if (e > max_end) max_end = e;
        }
    }
    if (ftruncate(ofd, (off_t)max_end)) {
        close(ofd); remove(out);
        fprintf(stderr, "> Error: cannot size %s\n", out);
        return 1;
    }

    int pfd = open(py->path, O_RDONLY);
    if (pfd < 0) {
        close(ofd); remove(out);
        fprintf(stderr, "> Error: cannot read %s\n", py->path);
        return 1;
    }

    int jobs = payload_workers();
    if (jobs > (int)p->nops) jobs = (int)p->nops;
    if (jobs < 1) jobs = 1;

    int rc = 0;
    if (jobs == 1) {
        rc = worker_run(py, p, pfd, ofd, 0, 1);
    } else {
        pid_t pids[8];
        int npids = 0, fork_fail = 0;
        for (int id = 1; id < jobs; id++) {
            pid_t pid = fork();
            if (pid < 0) { fork_fail = 1; break; }
            if (pid == 0) {
                int cr = worker_run(py, p, pfd, ofd, id, jobs);
                _exit(cr ? 1 : 0);
            }
            pids[npids++] = pid;
        }
        if (fork_fail) {
            for (int i = 0; i < npids; i++) kill(pids[i], SIGKILL);
            for (int i = 0; i < npids; i++) waitpid(pids[i], NULL, 0);
            rc = worker_run(py, p, pfd, ofd, 0, 1);
        } else {
            if (worker_run(py, p, pfd, ofd, 0, jobs)) rc = 1;
            if (rc) for (int i = 0; i < npids; i++) kill(pids[i], SIGKILL);
            for (int i = 0; i < npids; i++) {
                int st = 0;
                if (waitpid(pids[i], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = 1;
            }
        }
    }
    close(pfd);
    if (close(ofd)) rc = 1;
    if (rc) { fprintf(stderr, "> Error: failed to extract %s\n", p->name); remove(out); return 1; }
    printf("> Done: %s\n", out);
    if (auto_unpack) {
        printf("> Auto-unpacking: %s\n", p->name); fflush(stdout);
        int erc = extract_one(out, 1, delete_img);
        if (erc == EXTRACT_UNSUPPORTED) {
            fprintf(stderr, "> Warning: unsupported image type: %s\n", out);
            if (delete_img) delete_path(out);
        } else if (erc) {
            fprintf(stderr, "> Warning: auto-unpack failed: %s\n", out);
            *auto_fail = 1;
        } else if (delete_img) {
            delete_path(out);
        }
    }
    return 0;
}

static int cmd_dumper(int argc, char **argv) {
    const char *parts = NULL, *file = NULL;
    int list = 0, del = 0, autox = 0;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--partition") && i + 1 < argc) parts = argv[++i];
        else if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (!strcmp(argv[i], "--auto")) autox = flag_val(argc, argv, &i);
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: dna dumper [--list] [--partition a,b] [--delete 0|1] [--auto 0|1] payload.bin\n"); return 2; }
    char full[4400]; snprintf(full, sizeof full, "%s", file);
    if (access(full, R_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), file);
    if (access(full, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", file); return 1; }

    pay_t py;
    if (pay_load(&py, full)) { fprintf(stderr, "> Error: %s is not a valid payload.bin\n", file); pay_free(&py); return 1; }
    if (list) {
        for (uint32_t i = 0; i < py.nparts; i++) puts(py.parts[i].name);
        pay_free(&py); return 0;
    }
    {
        int jobs = payload_workers();
        printf("> Payload mode: %s (%d worker%s)\n",
               jobs >= 6 ? "fast" : (jobs >= 3 ? "balanced" : "saver"),
               jobs, jobs == 1 ? "" : "s");
        fflush(stdout);
    }

    int want = parts && *parts, found = 0, fail = 0, auto_fail = 0;
    for (uint32_t i = 0; i < py.nparts; i++) {
        if (want && !in_list(parts, py.parts[i].name)) continue;
        found = 1;
        if (dumper_one(&py, &py.parts[i], autox, del, &auto_fail)) fail = 1;
    }
    if (!found) { fprintf(stderr, "> Error: no matching partitions\n"); fail = 1; }
    pay_free(&py);
    if (!fail && !auto_fail && del) delete_path(full);
    return fail ? 1 : auto_fail;
}

/* ---------- unzip command ---------- */

static int cmd_unzip(int argc, char **argv) {
    const char *file = NULL, *dest = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--delete")) { flag_val(argc, argv, &i); continue; }
        if (argv[i][0] == '-') continue;
        if (!file) file = argv[i];
        else if (!dest) dest = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: dna unzip <file.zip2> <dest_dir>\n"); return 2; }
    char full[4400]; snprintf(full, sizeof full, "%s", file);
    if (access(full, R_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), file);
    if (access(full, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", file); return 1; }
    if (!dest || !*dest) dest = ".";
    if (mkdir_p(dest)) { fprintf(stderr, "> Error: cannot create %s\n", dest); return 1; }

    printf("> Extracting: %s -> %s\n", file, dest); fflush(stdout);
    char *a[] = { "busybox", "unzip", "-o", "-q", full, "-d", (char *)dest, NULL };
    int rc = run(a);
    if (rc) { fprintf(stderr, "> Error: unzip failed (rc=%d)\n", rc); return 1; }
    printf("> Done: %s\n", dest);
    return 0;
}


static const char *profile_name(perf_class_t c) {
    switch (c) {
    case PERF_SAVER: return "saver";
    case PERF_BALANCED: return "balanced";
    case PERF_FAST: return "fast";
    case PERF_TURBO: return "turbo";
    default: return "manual";
    }
}

static const char *storage_name(storage_kind_t s) {
    switch (s) {
    case STORAGE_EMMC: return "eMMC";
    case STORAGE_UFS: return "UFS";
    case STORAGE_NVME: return "NVMe";
    case STORAGE_OTHER: return "other";
    default: return "unknown";
    }
}

static int cmd_perf(int argc, char **argv) {
    int shell = 0, json = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--shell")) shell = 1;
        else if (!strcmp(argv[i], "--json")) json = 1;
        else { fprintf(stderr, "usage: dna perf [--shell|--json]\n"); return 2; }
    }
    perf_t p; perf_ovr_t ov; perf_load_t ld = { 0, 8, 0, 0, 0 };
    perf_detect(&p);
    perf_ovr_from_env(&ov);
    perf_make_plan(&p, &ld, &ov);

    if (shell) {
        printf("DNA_PERF_CLASS=%s\n", profile_name(p.cls));
        printf("DNA_PART_JOBS=%d\n", p.part_jobs);
        printf("DNA_EROFS_WORKERS=%d\n", p.erofs_workers);
        printf("DNA_BROTLI_JOBS=%d\n", p.brotli_jobs);
        printf("DNA_ZSTD_THREADS=%d\n", p.zstd_threads);
        return 0;
    }
    if (json) {
        printf("{\"profile\":\"%s\",\"storage\":\"%s\",\"mem_total_mb\":%llu,\"mem_avail_mb\":%llu,"
               "\"cpu_budget\":%d,\"ncpu\":%d,\"big_cores\":%d,\"little_cores\":%d,"
               "\"temp\":{\"battery\":%d,\"skin\":%d,\"soc\":%d},"
               "\"psi\":{\"mem\":%d,\"cpu\":%d,\"io\":%d},"
               "\"free_tmp_mb\":%llu,\"free_out_mb\":%llu,"
               "\"part_jobs\":%d,\"erofs_workers\":%d,\"brotli_jobs\":%d,\"zstd_threads\":%d,"
               "\"nice\":%d,\"reserve_mb\":%d}\n",
               profile_name(p.cls), storage_name(p.storage),
               (unsigned long long)(p.mem_total_kb / 1024), (unsigned long long)(p.mem_avail_kb / 1024),
               p.cpu_budget, p.ncpu_online, p.big_cores, p.little_cores,
               p.temp_battery_c, p.temp_skin_c, p.temp_soc_c,
               p.psi_mem_avg10, p.psi_cpu_avg10, p.psi_io_avg10,
               (unsigned long long)(p.free_tmp_bytes >> 20), (unsigned long long)(p.free_pro_bytes >> 20),
               p.part_jobs, p.erofs_workers, p.brotli_jobs, p.zstd_threads, p.nice_level, p.reserve_mb);
        return 0;
    }

    printf("> Device: RAM %.1f/%.1f GiB, %d cores (%d big + %d little), %s\n",
           (double)p.mem_avail_kb / 1048576.0, (double)p.mem_total_kb / 1048576.0,
           p.ncpu_online, p.big_cores, p.little_cores, storage_name(p.storage));
    printf("> Thermal: battery ");
    if (p.temp_battery_c) printf("%d°C", p.temp_battery_c); else printf("n/a");
    printf(", skin ");
    if (p.temp_skin_c) printf("%d°C", p.temp_skin_c); else printf("n/a");
    printf(", soc ");
    if (p.temp_soc_c) printf("%d°C", p.temp_soc_c); else printf("n/a");
    printf("\n");
    printf("> PSI memory: %d%%\n", p.psi_mem_avg10);
    printf("> Profile: %s\n", profile_name(p.cls));
    printf("> Plan: part_jobs=%d, erofs_workers=%d, brotli_jobs=%d, nice=%d\n",
           p.part_jobs, p.erofs_workers, p.brotli_jobs, p.nice_level);

    const char *dbg = getenv("DNA_PERF_DEBUG");
    if (dbg && *dbg && strcmp(dbg, "0")) {
        printf("> perf: RAM total=%llu MiB avail=%llu MiB swap=%llu/%llu MiB\n",
               (unsigned long long)(p.mem_total_kb / 1024), (unsigned long long)(p.mem_avail_kb / 1024),
               (unsigned long long)(p.swap_free_kb / 1024), (unsigned long long)(p.swap_total_kb / 1024));
        printf("> perf: CPU online=%d conf=%d big=%d little=%d budget=%d\n",
               p.ncpu_online, p.ncpu_conf, p.big_cores, p.little_cores, p.cpu_budget);
        printf("> perf: storage=%s temps=%d/%d/%d C PSI=%d/%d/%d\n", storage_name(p.storage),
               p.temp_battery_c, p.temp_skin_c, p.temp_soc_c,
               p.psi_mem_avg10, p.psi_cpu_avg10, p.psi_io_avg10);
        printf("> perf: free tmp=%llu MiB out=%llu MiB, reserve=%d MiB\n",
               (unsigned long long)(p.free_tmp_bytes >> 20),
               (unsigned long long)(p.free_pro_bytes >> 20), p.reserve_mb);
    }
    return 0;
}

/* ---------- dispatcher ---------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "dna %s\nusage: dna <gettype|dumper|extract|lpunpack|lpmake|repack|convert|unzip|perf> ...\n", VERSION);
        return 2;
    }
    const char *dbg = getenv("DNA_DEBUG");
    g_debug = dbg && *dbg && strcmp(dbg, "0");
    const char *c = argv[1];
    if (!strcmp(c, "gettype"))  return cmd_gettype(argc - 2, argv + 2);
    if (!strcmp(c, "lpunpack")) return cmd_lpunpack(argc - 2, argv + 2);
    if (!strcmp(c, "extract"))  return cmd_extract(argc - 2, argv + 2);
    if (!strcmp(c, "repack"))   return cmd_repack(argc - 2, argv + 2);
    if (!strcmp(c, "lpmake"))   return cmd_lpmake(argc - 2, argv + 2);
    if (!strcmp(c, "convert"))  return cmd_convert(argc - 2, argv + 2);
    if (!strcmp(c, "unzip"))    return cmd_unzip(argc - 2, argv + 2);
    if (!strcmp(c, "dumper"))   return cmd_dumper(argc - 2, argv + 2);
    if (!strcmp(c, "perf"))     return cmd_perf(argc - 2, argv + 2);
    fprintf(stderr, "dna: unknown command '%s'\n", c);
    return 2;
}
