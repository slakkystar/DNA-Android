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

#define VERSION "0.2"

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
    if (!strcmp(list, "@a")) { size_t l = strlen(name); return !(l > 2 && !strcmp(name + l - 2, "_b")); }
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

static int cmd_lpunpack(int argc, char **argv) {
    const char *parts = NULL, *file = NULL;
    int list = 0, del = 0, autox = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--partition") && i + 1 < argc) parts = argv[++i];
        else if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (!strcmp(argv[i], "--auto")) autox = flag_val(argc, argv, &i);
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: dna lpunpack [--list] [--partition a,b] [--delete 0|1] [--auto 0|1] super.img\n"); return 2; }

    img_t im; lp_t lp;
    if (img_open(&im, file) || lp_load(&im, &lp)) { fprintf(stderr, "> Error: %s is not a valid super image\n", file); img_close(&im); return 1; }

    if (list) {
        for (size_t i = 0; i < lp.np; i++) if (lp.p[i].sectors) puts(lp.p[i].name);
        img_close(&im); return 0;
    }

    char dir[4096]; snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else strcpy(dir, ".");

    int rc = 0, done = 0;
    uint8_t *buf = malloc(1 << 20);
    for (size_t i = 0; i < lp.np; i++) {
        part_t *p = &lp.p[i];
        if (!p->sectors || !in_list(parts, p->name)) continue;
        char out[4400]; snprintf(out, sizeof out, "%s/%s.img", dir, p->name);
        printf("> Extracting: %s\n", p->name); fflush(stdout);
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
    }
    free(buf); img_close(&im);
    if (!rc && !done) { fprintf(stderr, "> Error: no matching partitions\n"); rc = 1; }
    if (!rc && autox) printf("> Note: --auto unpack of images is not implemented yet\n");
    if (!rc && del) { printf("> Deleting: %s\n", file); remove(file); }
    return rc;
}


/* ---------- generic helpers ---------- */

static int run(char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) { execvp(argv[0], argv); fprintf(stderr, "> Error: cannot run %s: %s\n", argv[0], strerror(errno)); _exit(127); }
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
    printf("> Extracted %llu entries to %s\n", (unsigned long long)e.nfiles, e.outroot);
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

static int extract_one(const char *path, int *deleted_src);

static int extract_raw_by_type(const char *img, const char *name, const char *pname, const char *dro) {
    const char *t = detect_type(img);
    char dest[4400];
    if (!strcmp(t, "ext")) { printf("> ext4 detected: %s\n", name); return extract_ext(img, dro, pname); }
    if (!strcmp(t, "erofs")) {
        printf("> erofs detected: %s\n", name);
        /* extract.erofs derives the partition name from the file name: give it a clean one */
        char lnk[4400]; snprintf(lnk, sizeof lnk, "%s/dna_%d_%s.img", tmp_dir(), (int)getpid(), pname);
        char dd[4400]; snprintf(dd, sizeof dd, "%s/dna_%d", tmp_dir(), (int)getpid()); mkdir_p(dd);
        snprintf(lnk, sizeof lnk, "%s/%s.img", dd, pname);
        char abs[4400];
        if (img[0] == '/') snprintf(abs, sizeof abs, "%s", img);
        else { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) return -1; snprintf(abs, sizeof abs, "%s/%s", cwd, img); }
        unlink(lnk); if (symlink(abs, lnk)) return -1;
        char *a[] = { "extract.erofs", "-i", lnk, "-x", "-f", "-s", "-o", (char *)dro, NULL };
        int rc = run(a);
        unlink(lnk); rmdir(dd);
        return rc;
    }
    if (!strcmp(t, "f2fs")) {
        printf("> f2fs detected: %s\n", name);
        snprintf(dest, sizeof dest, "%s/%s", dro, pname); mkdir_p(dest);
        char *a[] = { "extract.f2fs", (char *)img, dest, NULL };
        return run(a);
    }
    if (!strcmp(t, "boot") || !strcmp(t, "vendor_boot")) {
        printf("> %s detected: %s\n", t, name);
        snprintf(dest, sizeof dest, "%s/%s", dro, pname); mkdir_p(dest);
        char abs[4400];
        if (img[0] == '/') snprintf(abs, sizeof abs, "%s", img);
        else { char cwd[2048]; if (!getcwd(cwd, sizeof cwd)) return -1; snprintf(abs, sizeof abs, "%s/%s", cwd, img); }
        char cwd0[2048]; if (!getcwd(cwd0, sizeof cwd0)) return -1;
        if (chdir(dest)) return -1;
        char *a[] = { "magiskboot", "unpack", "-h", abs, NULL };
        int rc = run(a);
        if (chdir(cwd0)) return -1;
        return rc;
    }
    fprintf(stderr, "> Unsupported image type '%s': %s\n", t, name);
    return 1;
}

static int extract_one(const char *path, int *deleted_src) {
    const char *dro = getenv("DNA_DRO");
    if (!dro || !*dro) { fprintf(stderr, "> Error: project is not selected (DNA_DRO is empty)\n"); return 1; }
    char full[4400]; snprintf(full, sizeof full, "%s", path);
    if (access(full, R_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), path);
    if (access(full, R_OK)) { fprintf(stderr, "> Error: file not found: %s\n", path); return 1; }
    char name[512], pname[512];
    base_name(full, name, sizeof name); part_name(full, pname, sizeof pname);
    size_t fl = strlen(full); *deleted_src = 0;
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

    const char *t = detect_type(full);
    if (!strcmp(t, "super")) return cmd_lpunpack(3, (char *[]){ "--partition", "@a", full, NULL });
    if (!strcmp(t, "sparse")) {
        { img_t pi; lp_t pl; int is_super = !img_open(&pi, full) && !lp_load(&pi, &pl); img_close(&pi);
          if (is_super) return cmd_lpunpack(3, (char *[]){ "--partition", "@a", full, NULL }); }
        char raw[4400]; snprintf(raw, sizeof raw, "%s/dna_%d_%s.raw", tmp_dir(), (int)getpid(), name);
        printf("> Converting sparse to raw: %s\n", name); fflush(stdout);
        if (sparse_to_raw(full, raw)) { fprintf(stderr, "> Error: sparse conversion failed\n"); return 1; }
        rc = extract_raw_by_type(raw, name, pname, dro);
        remove(raw);
        return rc;
    }
    return extract_raw_by_type(full, name, pname, dro);
}

static int cmd_extract(int argc, char **argv) {
    int del = 0, any = 0, fail = 0;
    char *files[256]; int nf = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--delete")) del = flag_val(argc, argv, &i);
        else if (argv[i][0] == '-') continue;
        else {
            for (char *tok = strtok(argv[i], ", \t\n"); tok && nf < 256; tok = strtok(NULL, ", \t\n")) files[nf++] = tok;
        }
    }
    if (!nf) { fprintf(stderr, "usage: dna extract <files...> [--delete 0|1]\n"); return 2; }
    for (int i = 0; i < nf; i++) {
        int d; any = 1;
        printf("> Unpacking: %s\n", files[i]); fflush(stdout);
        int rc = extract_one(files[i], &d);
        if (rc) { fprintf(stderr, "> Failed: %s\n", files[i]); fail = 1; continue; }
        printf("> Done: %s\n", files[i]);
        if (del) {
            char full[4400]; snprintf(full, sizeof full, "%s", files[i]);
            if (access(full, F_OK) && getenv("DNA_PRO")) snprintf(full, sizeof full, "%s/%s", getenv("DNA_PRO"), files[i]);
            printf("> Deleting: %s\n", files[i]); remove(full);
        }
    }
    return any && !fail ? 0 : 1;
}

/* ---------- dispatcher ---------- */

static int not_yet(const char *c) {
    fprintf(stderr, "dna: '%s' is not implemented yet\n", c);
    return 64;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "dna %s\nusage: dna <gettype|dumper|extract|lpunpack|lpmake|repack|convert|unzip> ...\n", VERSION);
        return 2;
    }
    const char *c = argv[1];
    if (!strcmp(c, "gettype"))  return cmd_gettype(argc - 2, argv + 2);
    if (!strcmp(c, "lpunpack")) return cmd_lpunpack(argc - 2, argv + 2);
    if (!strcmp(c, "extract"))  return cmd_extract(argc - 2, argv + 2);
    if (!strcmp(c, "dumper") ||
        !strcmp(c, "lpmake")  || !strcmp(c, "repack")  || !strcmp(c, "convert")  ||
        !strcmp(c, "unzip"))
        return not_yet(c);
    fprintf(stderr, "dna: unknown command '%s'\n", c);
    return 2;
}
