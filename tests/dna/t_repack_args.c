#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"
#include <sys/stat.h>

static void script(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\n", f);
    fputs(body, f);
    fclose(f);
    CHECK(chmod(path, 0755) == 0);
}

static int count_text(const char *s, const char *needle) {
    int n = 0;
    size_t l = strlen(needle);
    while ((s = strstr(s, needle))) { n++; s += l; }
    return n;
}

int main(void) {
    char pro[4096], cfg[4096], tree[4096], tools[4096], args[4096];
    snprintf(pro, sizeof pro, "%s", test_path("proj"));
    snprintf(cfg, sizeof cfg, "%s/config", pro);
    snprintf(tree, sizeof tree, "%s/system", pro);
    snprintf(tools, sizeof tools, "%s/tools", test_tmpdir());
    snprintf(args, sizeof args, "%s", test_path("erofs.args"));
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(cfg, 0755) == 0);
    CHECK(mkdir(tree, 0755) == 0);
    CHECK(mkdir(tools, 0755) == 0);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    test_write(test_path("proj/system/file"), "x");
    test_write(test_path("proj/config/system_fs_config"), "/system 0 0 0755\n/system/file 0 0 0644\n");
    test_write(test_path("proj/config/system_file_contexts"), "/system(/.*)? u:object_r:system_file:s0\n");
    script(test_path("tools/mkfs.erofs"),
           "printf '%s\\n' \"$@\" > \"$DNA_ARGS\"\n"
           "last=; prev=; for a in \"$@\"; do prev=$last; last=$a; done\n"
           "truncate -s 4096 \"$prev\"\n");
    char path[8192];
    snprintf(path, sizeof path, "%s:%s", tools, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", pro, 1);
    setenv("DNA_ARGS", args, 1);
    g_debug = 1;
    CHECK(freopen("/dev/null", "w", stdout) != NULL);

    char *a[] = { "system", "--convert", "1", "--auto", "1", "--erofs-workers", "3" };
    CHECK(cmd_repack(7, a) == 0);
    FILE *f = fopen(args, "r");
    CHECK(f != NULL);
    char text[16384];
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(count_text(text, "--workers=3\n") == 1);
    CHECK(strstr(text, "-T\n0\n") != NULL);
    CHECK(strstr(text, "--mount-point=/system\n") != NULL);
    CHECK(strstr(text, "--fs-config-file=") != NULL);
    CHECK(strstr(text, "--file-contexts=") != NULL);
    CHECK(strstr(text, "-zlz4hc,9\n") != NULL);

    char *bad[] = { "system", "--profile", "bogus" };
    CHECK(cmd_repack(3, bad) == 2);

    fprintf(stderr, "t_repack_args: ok\n");
    return 0;
}
