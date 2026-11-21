#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static int decode_sparse(const char *path, const char *out) {
    FILE *f = fopen(path, "rb"), *o = fopen(out, "wb");
    if (!f || !o) { if (f) fclose(f); if (o) fclose(o); return -1; }
    uint8_t h[28];
    if (fread(h, 1, 28, f) != 28 || le32(h) != 0xED26FF3Au) { fclose(f); fclose(o); return -1; }
    uint32_t bs = le32(h + 12), total = le32(h + 16), nch = le32(h + 20);
    uint16_t fh = le16(h + 8), chh = le16(h + 10);
    if (fseek(f, fh, SEEK_SET)) { fclose(f); fclose(o); return -1; }
    uint8_t *buf = malloc(bs);
    size_t bufcap = bs;
    if (!buf) { fclose(f); fclose(o); return -1; }
    uint64_t out_blocks = 0;
    int rc = 0;
    for (uint32_t i = 0; i < nch && !rc; i++) {
        uint8_t c[12];
        if (fread(c, 1, chh, f) != chh) { rc = -1; break; }
        uint16_t type = le16(c);
        uint32_t blocks = le32(c + 4), tot = le32(c + 8);
        uint64_t bytes = (uint64_t)blocks * bs;
        if (type == 0xCAC1) {
            if (bytes > bufcap) {
                uint8_t *nb = realloc(buf, (size_t)bytes);
                if (!nb) { rc = -1; break; }
                buf = nb; bufcap = (size_t)bytes;
            }
            if (fread(buf, 1, bytes, f) != bytes || fwrite(buf, 1, bytes, o) != bytes) rc = -1;
        } else if (type == 0xCAC2) {
            uint8_t fill[4];
            if (fread(fill, 1, 4, f) != 4) { rc = -1; break; }
            for (uint32_t j = 0; j < bs; j += 4) memcpy(buf + j, fill, 4);
            for (uint32_t j = 0; j < blocks && !rc; j++) if (fwrite(buf, 1, bs, o) != bs) rc = -1;
        } else if (type == 0xCAC3) {
            memset(buf, 0, bs);
            for (uint32_t j = 0; j < blocks && !rc; j++) if (fwrite(buf, 1, bs, o) != bs) rc = -1;
        } else if (type == 0xCAC4) {
            if (fseek(f, tot - chh, SEEK_CUR)) rc = -1;
        } else rc = -1;
        if (!rc && type != 0xCAC4 && tot > bytes && fseek(f, tot - chh - (long)bytes, SEEK_CUR)) rc = -1;
        out_blocks += blocks;
    }
    free(buf); fclose(f);
    if (fclose(o)) rc = -1;
    if (!rc && out_blocks != total) rc = -1;
    return rc;
}

static void compare(const char *raw, const char *sparse) {
    char dec[4096];
    snprintf(dec, sizeof dec, "%s.dec", raw);
    if (decode_sparse(sparse, dec) != 0) { fprintf(stderr, "decode failed: %s\n", raw); CHECK(0); }
    FILE *a = fopen(raw, "rb"), *b = fopen(dec, "rb");
    CHECK(a && b);
    CHECK(fseek(a, 0, SEEK_END) == 0 && fseek(b, 0, SEEK_END) == 0);
    long sa = ftell(a), sb = ftell(b);
    CHECK(sb >= sa);
    CHECK(sb <= ((sa + 4095) / 4096) * 4096);
    rewind(a); rewind(b);
    uint8_t ba[65536], bb[65536];
    for (long left = sa; left > 0; ) {
        size_t n = left > (long)sizeof ba ? sizeof ba : (size_t)left;
        CHECK(fread(ba, 1, n, a) == n && fread(bb, 1, n, b) == n);
        CHECK(memcmp(ba, bb, n) == 0);
        left -= (long)n;
    }
    for (long left = sb - sa; left > 0; ) {
        size_t n = left > (long)sizeof bb ? sizeof bb : (size_t)left;
        CHECK(fread(bb, 1, n, b) == n);
        for (size_t i = 0; i < n; i++) CHECK(bb[i] == 0);
        left -= (long)n;
    }
    fclose(a); fclose(b);
}

static void case_file(const char *name, const void *data, size_t len) {
    char raw[4096], sp[4096];
    snprintf(raw, sizeof raw, "%s/%s.raw", test_tmpdir(), name);
    snprintf(sp, sizeof sp, "%s/%s.sparse", test_tmpdir(), name);
    test_write_bytes(raw, data, len);
    CHECK(raw_to_sparse(raw, sp) == 0);
    compare(raw, sp);
}

static void case_holes(void) {
    char raw[4096], sp[4096];
    snprintf(raw, sizeof raw, "%s/holes.raw", test_tmpdir());
    snprintf(sp, sizeof sp, "%s/holes.sparse", test_tmpdir());
    int fd = open(raw, O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 20 * 4096) == 0);
    uint8_t blk[4096];
    memset(blk, 0x5A, sizeof blk);
    CHECK(pwrite(fd, blk, sizeof blk, 1 * 4096) == 4096);
    CHECK(pwrite(fd, blk, sizeof blk, 5 * 4096) == 4096);
    CHECK(pwrite(fd, blk, 4096, 19 * 4096) == 4096);
    CHECK(pwrite(fd, blk, 100, 10 * 4096 + 37) == 100);
    close(fd);
    CHECK(raw_to_sparse(raw, sp) == 0);
    compare(raw, sp);
}

static void case_sparse_zeros(void) {
    char raw[4096], sp[4096];
    snprintf(raw, sizeof raw, "%s/zeros-alloc.raw", test_tmpdir());
    snprintf(sp, sizeof sp, "%s/zeros-alloc.sparse", test_tmpdir());
    int fd = open(raw, O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 16 * 4096) == 0);
    uint8_t blk[4096];
    memset(blk, 0x11, sizeof blk);
    CHECK(pwrite(fd, blk, sizeof blk, 0) == 4096);
    CHECK(pwrite(fd, blk, sizeof blk, 15 * 4096) == 4096);
    close(fd);
    CHECK(raw_to_sparse(raw, sp) == 0);
    compare(raw, sp);
}

int main(void) {
    uint8_t data[4096 * 20];
    memset(data, 0xA5, sizeof data);
    case_file("empty", NULL, 0);
    case_file("short", data, 100);
    case_file("one-block", data, 4096);
    case_file("short-tail", data, 4097);
    uint8_t zeros[4096 * 8] = {0};
    case_file("zeros", zeros, sizeof zeros);
    case_file("data8", data, 4096 * 8);
    uint8_t alt[4096 * 16];
    memset(alt, 0, sizeof alt);
    for (int i = 0; i < 8; i++) memset(alt + (i * 2 + 1) * 4096, 0x33 + i, 4096);
    case_file("alternating", alt, sizeof alt);
    case_holes();
    case_sparse_zeros();
    CHECK(12ull + (uint64_t)SPARSE_DATA_CHUNK_BLOCKS * SPARSE_BS <= UINT32_MAX);
    CHECK(SPARSE_DATA_CHUNK_BLOCKS == 16384);
    fprintf(stderr, "t_sparse: ok\n");
    return 0;
}
