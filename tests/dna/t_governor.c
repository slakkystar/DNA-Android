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

static void root(const char *name, char **out, const char *mem, const char *soc, const char *psi) {
    static char paths[8][4096];
    static int i = 0;
    char *r = paths[i++];
    snprintf(r, 4096, "%s/%s", test_tmpdir(), name);
    CHECK(mkdir_p(r) == 0);
    char p[4096];
    snprintf(p, sizeof p, "%s/proc/meminfo", r);
    mkfile_at(p, mem);
    if (soc) {
        snprintf(p, sizeof p, "%s/sys/class/thermal/thermal_zone0/type", r);
        mkfile_at(p, "soc\n");
        snprintf(p, sizeof p, "%s/sys/class/thermal/thermal_zone0/temp", r);
        mkfile_at(p, soc);
    }
    if (psi) {
        snprintf(p, sizeof p, "%s/proc/pressure/memory", r);
        mkfile_at(p, psi);
    }
    *out = r;
}

int main(void) {
    g_gate_mem_timeout = 1;
    g_gate_temp_timeout = 1;
    char *r;
    perf_t p;
    const char *reason = "";
    p.reserve_mb = 768;

    root("ok", &r, "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n", "35000\n", "some avg10=1.00\n");
    g_sysroot = r;
    CHECK(perf_gate(&p, 0, &reason) == GATE_OK);
    CHECK(perf_wait_gate(&p, 0) == 0);

    root("low", &r, "MemTotal: 12582912 kB\nMemAvailable: 100000 kB\n", "35000\n", NULL);
    g_sysroot = r;
    CHECK(perf_gate(&p, 0, &reason) == GATE_WAIT);
    CHECK(strcmp(reason, "low memory") == 0);
    CHECK(perf_wait_gate(&p, 0) == 1);

    root("psi", &r, "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n", "35000\n", "some avg10=20.00\n");
    g_sysroot = r;
    CHECK(perf_gate(&p, 0, &reason) == GATE_OK);
    CHECK(perf_gate(&p, 1, &reason) == GATE_WAIT);
    CHECK(strcmp(reason, "memory pressure") == 0);
    CHECK(perf_wait_gate(&p, 0) == 0);
    CHECK(perf_wait_gate(&p, 1) == 2);

    root("hot", &r, "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n", "95000\n", NULL);
    g_sysroot = r;
    CHECK(perf_gate(&p, 0, &reason) == GATE_WAIT);
    CHECK(strcmp(reason, "device is hot") == 0);
    CHECK(perf_wait_gate(&p, 0) == 1);
    CHECK(perf_wait_gate(&p, 1) == 2);

    g_sysroot = "";
    g_gate_mem_timeout = 300;
    g_gate_temp_timeout = 600;
    fprintf(stderr, "t_governor: ok\n");
    return 0;
}
