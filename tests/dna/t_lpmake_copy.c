#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void fill_file(const char *path, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    for (size_t i = 0; i < n; i++) {
        unsigned char v = (unsigned char)(i * 31 + (i >> 9) + 7);
        CHECK(fwrite(&v, 1, 1, f) == 1);
    }
    CHECK(fclose(f) == 0);
}

int main(void) {
    const size_t n = (4u << 20) + 12345;
    char src[4096], a[4096], b[4096];
    snprintf(src, sizeof src, "%s", test_path("src.bin"));
    snprintf(a, sizeof a, "%s", test_path("a.bin"));
    snprintf(b, sizeof b, "%s", test_path("b.bin"));
    fill_file(src, n);
    int in = open(src, O_RDONLY);
    CHECK(in >= 0);

    int out = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(out >= 0);
#ifdef DNA_TEST
    g_no_cfr = 0;
#endif
    CHECK(copy_range_fd(in, out, 0, n) == 0);
    CHECK(close(out) == 0);

    out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(out >= 0);
#ifdef DNA_TEST
    g_no_cfr = 1;
#endif
    CHECK(copy_range_fd(in, out, 0, n) == 0);
    CHECK(close(out) == 0);
    CHECK(close(in) == 0);

    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    CHECK(fa && fb);
    int same = 1;
    for (;;) {
        unsigned char x[65536], y[65536];
        size_t na = fread(x, 1, sizeof x, fa), nb = fread(y, 1, sizeof y, fb);
        if (na != nb || memcmp(x, y, na)) { same = 0; break; }
        if (!na) break;
    }
    fclose(fa); fclose(fb);
    CHECK(same);
#ifdef DNA_TEST
    g_no_cfr = 0;
#endif

    fprintf(stderr, "t_lpmake_copy: ok\n");
    return 0;
}
