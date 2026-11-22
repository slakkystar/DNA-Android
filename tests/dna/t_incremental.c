#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"
#include <utime.h>

static void script(const char *dir, const char *name, const char *body) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    test_write(path, body);
    CHECK(chmod(path, 0755) == 0);
}

static int lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0, c, last = '\n';
    while ((c = fgetc(f)) != EOF) { if (c == '\n') n++; last = c; }
    if (last != '\n') n++;
    fclose(f);
    return n;
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
    char dro[4096], pro[4096], bin[4096], mklog[4096], path[8192];
    snprintf(dro, sizeof dro, "%s", test_path("dro"));
    snprintf(pro, sizeof pro, "%s", test_path("pro"));
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    CHECK(mkdir(dro, 0755) == 0);
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(bin, 0755) == 0);
    CHECK(mkdir(test_path("dro/system"), 0755) == 0);
    CHECK(mkdir(test_path("pro/out"), 0755) == 0);
    test_write(test_path("dro/system/a"), "one\n");
    snprintf(mklog, sizeof mklog, "%s", test_path("mk.log"));
    script(bin, "mkfs.erofs",
        "#!/bin/sh\n"
        "echo \"$@\" >> \"$MKLOG\"\n"
        "for a in \"$@\"; do prev=$last; last=$a; done\n"
        "printf img > \"$prev\"\n"
        "exit 0\n");
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("MKLOG", mklog, 1);
    setenv("DNA_INCREMENTAL", "1", 1);

    ropts_t o;
    memset(&o, 0, sizeof o);
    o.rw = 1;
    o.autosize = 1;
    o.incremental = 1;
    o.erofslvl = 9;
    snprintf(o.mode, sizeof o.mode, "lz4hc");

    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 1);
    const char *cache = test_path("pro/out/.dna-cache/system.json");
    CHECK(has(cache, "\"fp\":\""));
    CHECK(has(cache, "\"out\":\"system.img\""));
    CHECK(has(cache, "\"out_size\":"));
    CHECK(has(cache, "\"out_mtime\":"));

    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 1);

    test_write(test_path("dro/system/a"), "one changed\n");
    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 2);

    o.erofslvl = 8;
    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 3);

    char tool[4096];
    snprintf(tool, sizeof tool, "%s", test_path("bin/mkfs.erofs"));
    struct utimbuf ut;
    ut.actime = time(NULL) + 20;
    ut.modtime = time(NULL) + 20;
    CHECK(utime(tool, &ut) == 0);
    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 4);

    CHECK(unlink(test_path("pro/out/system.img")) == 0);
    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 5);

    test_write(cache, "{broken");
    CHECK(repack_one("system", 1, &o, dro, pro, 0) == 0);
    CHECK(lines(mklog) == 6);

    fprintf(stderr, "t_incremental: ok\n");
    return 0;
}
