#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void script(const char *dir, const char *name, const char *body) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    test_write(path, body);
    CHECK(chmod(path, 0755) == 0);
}

static int same_file(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    if (!x || !y) { if (x) fclose(x); if (y) fclose(y); return 0; }
    int ok = 1, c1, c2;
    do { c1 = fgetc(x); c2 = fgetc(y); if (c1 != c2) ok = 0; } while (ok && c1 != EOF);
    fclose(x); fclose(y);
    return ok;
}

int main(void) {
    static uint8_t data[8192];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 37 + 11);
    char img[4096], out[4096], bin[4096], path[8192];
    snprintf(img, sizeof img, "%s", test_path("sys.img"));
    snprintf(out, sizeof out, "%s", test_path("out"));
    CHECK(mkdir(out, 0755) == 0);
    test_write_bytes(img, data, sizeof data);
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    CHECK(mkdir(bin, 0755) == 0);
    script(bin, "brotli", "#!/bin/sh\nexec cat\n");
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);

    CHECK(img_to_dat(img, out, "sys", 3, 1) == 0);
    CHECK(access(test_path("out/sys.new.dat"), F_OK) != 0);
    CHECK(same_file(img, test_path("out/sys.new.dat.br")));
    CHECK(access(test_path("out/sys.transfer.list"), F_OK) == 0);

    unlink(test_path("out/sys.new.dat.br"));
    CHECK(img_to_dat(img, out, "sys", 3, 0) == 0);
    CHECK(access(test_path("out/sys.new.dat.br"), F_OK) != 0);
    CHECK(same_file(img, test_path("out/sys.new.dat")));

    fprintf(stderr, "t_brotli_pipe: ok\n");
    return 0;
}
