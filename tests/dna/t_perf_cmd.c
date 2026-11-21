#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void mkfile_at(const char *path, const char *text) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *s = strrchr(dir, '/');
    CHECK(s != NULL);
    *s = 0;
    CHECK(mkdir_p(dir) == 0);
    test_write(path, text);
}

static void capture(char *out, size_t n, int argc, char **argv) {
    const char *path = test_path("perf.out");
    CHECK(freopen(path, "w", stdout) != NULL);
    CHECK(cmd_perf(argc, argv) == 0);
    fflush(stdout);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    CHECK(freopen("/dev/null", "w", stdout) != NULL);
}

int main(void) {
    char root[4096];
    snprintf(root, sizeof root, "%s/sysroot", test_tmpdir());
    CHECK(mkdir_p(root) == 0);
    g_sysroot = root;
    mkfile_at(test_path("sysroot/proc/meminfo"),
              "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone0/type"), "battery\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone0/temp"), "37000\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone1/type"), "skin\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone1/temp"), "39000\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone2/type"), "soc\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone2/temp"), "50000\n");
    mkfile_at(test_path("sysroot/proc/pressure/memory"), "some avg10=0.40 avg60=0.00 avg300=0.00\n");
    mkfile_at(test_path("sysroot/sys/block/sda/queue/rotational"), "0\n");
    mkfile_at(test_path("sysroot/sys/class/ufs/ufshcd0/x"), "1\n");

    char out[16384];
    char *normal[] = { NULL };
    capture(out, sizeof out, 0, normal);
    CHECK(strstr(out, "> Device: RAM 8.0/12.0 GiB") != NULL);
    CHECK(strstr(out, "UFS") != NULL);
    CHECK(strstr(out, "> Thermal: battery 37°C, skin 39°C, soc 50°C") != NULL);
    CHECK(strstr(out, "> PSI memory: 0%") != NULL);
    CHECK(strstr(out, "> Profile: ") != NULL);
    CHECK(strstr(out, "> Plan: part_jobs=") != NULL);

    setenv("DNA_PERF_DEBUG", "1", 1);
    capture(out, sizeof out, 0, normal);
    CHECK(strstr(out, "> perf: RAM total=") != NULL);
    unsetenv("DNA_PERF_DEBUG");

    char *shell[] = { "--shell" };
    capture(out, sizeof out, 1, shell);
    CHECK(strstr(out, "DNA_PERF_CLASS=") != NULL);
    CHECK(strstr(out, "DNA_PART_JOBS=") != NULL);
    CHECK(strstr(out, "DNA_EROFS_WORKERS=") != NULL);
    CHECK(strstr(out, "DNA_BROTLI_JOBS=") != NULL);
    CHECK(strstr(out, "DNA_ZSTD_THREADS=") != NULL);
    CHECK(strstr(out, ">") == NULL);

    char *json[] = { "--json" };
    capture(out, sizeof out, 1, json);
    CHECK(out[0] == '{');
    CHECK(strstr(out, "\"profile\":\"") != NULL);
    CHECK(strstr(out, "\"mem_total_mb\":12288") != NULL);
    CHECK(strstr(out, "\"temp\":{\"battery\":37,\"skin\":39,\"soc\":50}") != NULL);
    CHECK(strstr(out, "\"part_jobs\":") != NULL);
    CHECK(strstr(out, "\"reserve_mb\":") != NULL);

    g_sysroot = "";
    fprintf(stderr, "t_perf_cmd: ok\n");
    return 0;
}
