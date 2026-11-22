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

static int has(const char *path, const char *s) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, s) != NULL;
}

int main(void) {
    ropts_t o;
    memset(&o, 0, sizeof o);
    snprintf(o.mode, sizeof o.mode, "lz4hc");
    o.auto_speed = 1;
    CHECK(autospeed_apply(&o, PERF_SAVER) == 1 && o.erofslvl == 4 && o.brlvl == 2);
    CHECK(autospeed_apply(&o, PERF_BALANCED) == 1 && o.erofslvl == 6 && o.brlvl == 3);
    CHECK(autospeed_apply(&o, PERF_FAST) == 1 && o.erofslvl == 8 && o.brlvl == 5);
    CHECK(autospeed_apply(&o, PERF_TURBO) == 1 && o.erofslvl == 9 && o.brlvl == 6);
    snprintf(o.mode, sizeof o.mode, "lzma");
    o.erofslvl = 5;
    CHECK(autospeed_apply(&o, PERF_FAST) == 0 && o.erofslvl == 5);
    snprintf(o.mode, sizeof o.mode, "lz4hc");
    o.auto_speed = 0;
    o.erofslvl = 7;
    CHECK(autospeed_apply(&o, PERF_FAST) == 0 && o.erofslvl == 7);

    char dro[4096], pro[4096], bin[4096], log[4096], path[8192];
    snprintf(dro, sizeof dro, "%s", test_path("dro"));
    snprintf(pro, sizeof pro, "%s", test_path("pro"));
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    snprintf(log, sizeof log, "%s", test_path("mk.log"));
    CHECK(mkdir(dro, 0755) == 0);
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(bin, 0755) == 0);
    CHECK(mkdir(test_path("dro/system"), 0755) == 0);
    test_write(test_path("dro/system/a"), "x\n");
    script(bin, "mkfs.erofs",
        "#!/bin/sh\n"
        "echo \"$@\" >> \"$MKLOG\"\n"
        "for a in \"$@\"; do prev=$last; last=$a; done\n"
        "printf img > \"$prev\"\n"
        "exit 0\n");
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("MKLOG", log, 1);
    setenv("DNA_DRO", dro, 1);
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_SPACE_CHECK", "0", 1);

    char *argv[] = { "system", "--profile", "saver", "--auto-speed", "1", "--mode", "lz4hc",
                     "--erofs", "9", "--br", "7", "--convert", "1", "--format", "0", "--type", "0", NULL };
    CHECK(cmd_repack(15, argv) == 0);
    CHECK(has(log, "-zlz4hc,4"));

    unlink(log);
    char *argv2[] = { "system", "--profile", "saver", "--auto-speed", "0", "--mode", "lz4hc",
                      "--erofs", "7", "--convert", "1", "--format", "0", "--type", "0", NULL };
    CHECK(cmd_repack(13, argv2) == 0);
    CHECK(has(log, "-zlz4hc,7"));

    unlink(log);
    char *argv3[] = { "system", "--profile", "fast", "--auto-speed", "1", "--mode", "lzma",
                      "--erofs", "5", "--convert", "1", "--format", "0", "--type", "0", NULL };
    CHECK(cmd_repack(13, argv3) == 0);
    CHECK(has(log, "-zlzma,5"));

    fprintf(stderr, "t_autospeed: ok\n");
    return 0;
}
