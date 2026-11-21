#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void put_block(int fd, uint64_t blk, unsigned char v) {
    unsigned char b[4096];
    memset(b, v, sizeof b);
    CHECK(pwrite(fd, b, sizeof b, (off_t)(blk * 4096)) == 4096);
}

static char *read_text(const char *path) {
    static char buf[8192];
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void expect_dat(const char *out, const char *name, const char *tl, const char *bytes, size_t nbytes) {
    char p[4600];
    snprintf(p, sizeof p, "%s/%s.transfer.list", out, name);
    CHECK(strcmp(read_text(p), tl) == 0);
    snprintf(p, sizeof p, "%s/%s.new.dat", out, name);
    FILE *f = fopen(p, "rb");
    CHECK(f != NULL);
    char got[16384];
    size_t n = fread(got, 1, sizeof got, f);
    fclose(f);
    CHECK(n == nbytes);
    CHECK(memcmp(got, bytes, nbytes) == 0);
    snprintf(p, sizeof p, "%s/%s.patch.dat", out, name);
    CHECK(strcmp(read_text(p), "") == 0);
}

int main(void) {
    char out[4096], raw[4096];
    snprintf(out, sizeof out, "%s", test_path("out"));
    CHECK(mkdir(out, 0755) == 0);
    snprintf(raw, sizeof raw, "%s/holes.img", test_tmpdir());
    int fd = open(raw, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 8 * 4096) == 0);
    put_block(fd, 0, 0xAA);
    put_block(fd, 1, 0x00);
    put_block(fd, 3, 0xBB);
    CHECK(close(fd) == 0);
    CHECK(img_to_dat(raw, out, "holes", 3, 0) == 0);
    unsigned char want[8192];
    memset(want, 0xAA, 4096);
    memset(want + 4096, 0xBB, 4096);
    expect_dat(out, "holes",
               "4\n8\n0\n0\nnew 2,0,1\nzero 4,1,3,4,8\nnew 2,3,4\n",
               (const char *)want, sizeof want);

    snprintf(raw, sizeof raw, "%s/allhole.img", test_tmpdir());
    fd = open(raw, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 4 * 4096) == 0);
    CHECK(close(fd) == 0);
    CHECK(img_to_dat(raw, out, "allhole", 3, 0) == 0);
    memset(want, 0, sizeof want);
    expect_dat(out, "allhole", "4\n4\n0\n0\nnew 2,0,1\nzero 2,1,4\n",
               (const char *)want, 4096);

    snprintf(raw, sizeof raw, "%s/full.img", test_tmpdir());
    fd = open(raw, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    put_block(fd, 0, 0x11);
    put_block(fd, 1, 0x00);
    put_block(fd, 2, 0x22);
    CHECK(close(fd) == 0);
    CHECK(img_to_dat(raw, out, "full", 3, 0) == 0);
    memset(want, 0x11, 4096);
    memset(want + 4096, 0x22, 4096);
    expect_dat(out, "full", "4\n3\n0\n0\nnew 2,0,1\nzero 2,1,2\nnew 2,2,3\n",
               (const char *)want, 8192);

    fprintf(stderr, "t_dat: ok\n");
    return 0;
}
