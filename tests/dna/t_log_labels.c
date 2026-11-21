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

int main(void) {
    char pro[4096], dro[4096], tree[4096], cfg[4096], tools[4096];
    snprintf(pro, sizeof pro, "%s", test_path("proj"));
    snprintf(dro, sizeof dro, "%s", pro);
    snprintf(tree, sizeof tree, "%s/system", pro);
    snprintf(cfg, sizeof cfg, "%s/config", pro);
    snprintf(tools, sizeof tools, "%s/tools", test_tmpdir());
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(tree, 0755) == 0);
    CHECK(mkdir(cfg, 0755) == 0);
    CHECK(mkdir(tools, 0755) == 0);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    test_write(test_path("proj/system/file"), "x");
    test_write(test_path("proj/config/system_fs_config"), "/system 0 0 0755\n/system/file 0 0 0644\n");
    script(test_path("tools/mke2fs"),
           "last=; prev=; for a in \"$@\"; do prev=$last; last=$a; done\n"
           "truncate -s $((last * 4096)) \"$prev\"\n");
    script(test_path("tools/e2fsdroid"), "exit 0\n");
    script(test_path("tools/resize2fs"), "exit 0\n");
    script(test_path("tools/e2fsck"), "exit 0\n");

    char path[8192];
    snprintf(path, sizeof path, "%s:%s", tools, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", dro, 1);
    g_debug = 1;

    char capture[4096];
    snprintf(capture, sizeof capture, "%s", test_path("labels.out"));
    CHECK(freopen(capture, "w", stdout) != NULL);
    char *args[] = { "system", "--auto", "1", "--read", "1" };
    int rc = cmd_repack(4, args);
    fflush(stdout);
    CHECK(rc == 0);

    FILE *f = fopen(capture, "r");
    CHECK(f != NULL);
    char text[16384] = "";
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    CHECK(strstr(text, "<ext4>") != NULL);
    CHECK(strstr(text, "(ext4)") == NULL);
    CHECK(strstr(text, "Building <ext4>") != NULL);

    fprintf(stderr, "t_log_labels: ok\n");
    return 0;
}
