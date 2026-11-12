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

#define VERSION "0.1"

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
    if (!strcmp(c, "dumper") || !strcmp(c, "extract") ||
        !strcmp(c, "lpmake")  || !strcmp(c, "repack")  || !strcmp(c, "convert")  ||
        !strcmp(c, "unzip"))
        return not_yet(c);
    fprintf(stderr, "dna: unknown command '%s'\n", c);
    return 2;
}
