#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void tree(const char *pro, const char *name, size_t bytes) {
    char dir[4096], file[4096];
    snprintf(dir, sizeof dir, "%s/%s", pro, name);
    snprintf(file, sizeof file, "%s/blob", dir);
    CHECK(mkdir(dir, 0755) == 0);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)bytes) == 0);
    CHECK(close(fd) == 0);
}

int main(void) {
    char pro[4096];
    snprintf(pro, sizeof pro, "%s", test_path("proj"));
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(test_path("proj/config"), 0755) == 0);
    tree(pro, "small", 1u << 20);
    tree(pro, "large", 16u << 20);
    ropts_t o;
    memset(&o, 0, sizeof o);
    o.autosize = 1;

    uint64_t e1 = 0, e2 = 0, i1 = 0, i2 = 0;
    uint64_t b1 = ext4_size_blocks("small", pro, &o, &e1, &i1);
    uint64_t b2 = ext4_size_blocks("large", pro, &o, &e2, &i2);
    CHECK(b1 > 0 && b2 > b1);
    CHECK(e2 > e1 && i2 >= i1);

    const char *hint = test_path("proj/config/large_dna_size_hint");
    test_write(hint, "ratio_permille=3000\n");
    uint64_t b3 = ext4_size_blocks("large", pro, &o, NULL, NULL);
    CHECK(b3 > b2);

    test_write(hint, "ratio_permille=4000\n");
    CHECK(ext4_size_blocks("large", pro, &o, NULL, NULL) == b2);
    test_write(hint, "ratio_permille=499\n");
    CHECK(ext4_size_blocks("large", pro, &o, NULL, NULL) == b2);
    test_write(hint, "ratio_permille=2000\n");
    CHECK(ext4_size_blocks("large", pro, &o, NULL, NULL) >= b2);

    o.autosize = 0;
    test_write(test_path("proj/config/small_info"), "33554432\n");
    CHECK(ext4_size_blocks("small", pro, &o, NULL, NULL) == 33554432 / 4096);


    char bin[4096], mklog[4096], count[4096];
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    CHECK(mkdir(bin, 0755) == 0);
    snprintf(mklog, sizeof mklog, "%s", test_path("mk.log"));
    snprintf(count, sizeof count, "%s", test_path("mk.count"));
    const char *p1 = test_path("bin/mke2fs");
    FILE *f = fopen(p1, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\n"
          "for a in \"$@\"; do last=$a; done\n"
          "echo \"$last\" >> \"$MKLOG\"\n"
          "exit 0\n", f);
    fclose(f);
    CHECK(chmod(p1, 0755) == 0);
    const char *p2 = test_path("bin/e2fsdroid");
    f = fopen(p2, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\n"
          "n=$(cat \"$MKCOUNT\" 2>/dev/null || echo 0)\n"
          "n=$((n+1)); echo $n > \"$MKCOUNT\"\n"
          "[ $n -eq 1 ] && exit 1\n"
          "exit 0\n", f);
    fclose(f);
    CHECK(chmod(p2, 0755) == 0);
    char path[8192];
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("MKLOG", mklog, 1);
    setenv("MKCOUNT", count, 1);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    tree(pro, "retry", 16u << 20);
    test_write(test_path("proj/config/retry_fs_config"), "");
    unsetenv("DNA_SPACE_CHECK");
    o.autosize = 1;
    o.sparse = 0;
    part_start();
    CHECK(repack_ext4("retry", pro, pro, &o) == 0);
    FILE *lg = fopen(mklog, "r");
    CHECK(lg != NULL);
    unsigned long long a = 0, b = 0;
    CHECK(fscanf(lg, "%llu %llu", &a, &b) == 2);
    fclose(lg);
    CHECK(b == a * 125 / 100 + 512);

    fprintf(stderr, "t_ext4_size: ok\n");
    return 0;
}
