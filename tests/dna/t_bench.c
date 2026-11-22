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

static int contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

int main(void) {
    char tmp[4096], cache[4096], bin[4096], path[8192];
    snprintf(tmp, sizeof tmp, "%s", test_path("tmp"));
    snprintf(cache, sizeof cache, "%s", test_path("cache"));
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    CHECK(mkdir(tmp, 0755) == 0);
    CHECK(mkdir(cache, 0755) == 0);
    CHECK(mkdir(bin, 0755) == 0);
    script(bin, "mkfs.erofs",
        "#!/bin/sh\n"
        "for a in \"$@\"; do case \"$a\" in --workers=*) n=${a#--workers=};; esac; done\n"
        "case \"$n\" in\n"
        "  1) sleep 0.12;;\n"
        "  2) sleep 0.06;;\n"
        "  4) sleep 0.03;;\n"
        "  *) sleep 0.30;;\n"
        "esac\n"
        "exit 0\n");
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("DNA_TMP", tmp, 1);
    setenv("DNA_DIR", cache, 1);

    perf_t p;
    memset(&p, 0, sizeof p);
    p.cpu_budget = 8;
    p.temp_battery_c = 31;

    char bench_dir[4096];
    snprintf(bench_dir, sizeof bench_dir, "%s/dna-bench-%d", tmp, (int)getpid());
    CHECK(perf_bench(&p) == 0);
    CHECK(access(bench_dir, F_OK) != 0);

    char json[4400];
    snprintf(json, sizeof json, "%s/.dna-perf-bench.json", cache);
    CHECK(contains(json, "\"timestamp\":\""));
    CHECK(contains(json, "\"temp_c\":31"));
    CHECK(contains(json, "\"best_erofs_workers\":4"));
    CHECK(contains(json, "\"best_parallel_writers\":"));
    CHECK(contains(json, "\"erofs_workers_tested\":[1,2,4,8]"));
    CHECK(contains(json, "\"erofs_times_ms\":["));
    CHECK(contains(json, "\"write_times_ms\":["));

    char ts[64];
    int e = -1, w = -1;
    CHECK(bench_read_last(cache, ts, sizeof ts, &e, &w) == 1);
    CHECK(e == 4 && w >= 2 && w <= 4 && ts[0]);

    fprintf(stderr, "t_bench: ok\n");
    return 0;
}
