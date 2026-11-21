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

static void fake_cpus(const char *root, int n, const char *value) {
    for (int i = 0; i < n; i++) {
        char p[4096];
        snprintf(p, sizeof p, "%s/sys/devices/system/cpu/cpu%d/cpu_capacity", root, i);
        mkfile_at(p, value);
    }
}

static void set_temp(const char *root, int c) {
    char p[4096], v[32];
    snprintf(p, sizeof p, "%s/sys/class/thermal/thermal_zone0/type", root);
    mkfile_at(p, "soc\n");
    snprintf(p, sizeof p, "%s/sys/class/thermal/thermal_zone0/temp", root);
    snprintf(v, sizeof v, "%d000\n", c);
    mkfile_at(p, v);
}

int main(void) {
    CHECK(freopen("/dev/null", "w", stdout) != NULL);
    long conf = sysconf(_SC_NPROCESSORS_CONF);
    if (conf < 1) conf = 1;
    if (conf > 32) conf = 32;
    char root[4096];
    snprintf(root, sizeof root, "%s/pw", test_tmpdir());
    CHECK(mkdir_p(root) == 0);
    g_sysroot = root;
    mkfile_at(test_path("pw/proc/meminfo"), "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n");
    fake_cpus(root, (int)conf, "1024\n");
    set_temp(root, 35);

    unsetenv("DNA_PAYLOAD_WORKERS");
    int n = payload_workers();
    perf_t p; perf_detect(&p);
    CHECK(n >= 1 && n <= 8);
    CHECK(n <= p.cpu_budget);
    CHECK(n == (8 < p.cpu_budget ? 8 : p.cpu_budget));

    setenv("DNA_PAYLOAD_WORKERS", "3", 1);
    CHECK(payload_workers() == 3);
    unsetenv("DNA_PAYLOAD_WORKERS");

    char root2[4096];
    snprintf(root2, sizeof root2, "%s/pw2", test_tmpdir());
    CHECK(mkdir_p(root2) == 0);
    g_sysroot = root2;
    mkfile_at(test_path("pw2/proc/meminfo"), "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n");
    fake_cpus(root2, (int)conf, "1024\n");
    set_temp(root2, 95);
    n = payload_workers();
    CHECK(n >= 1 && n <= 2);

    char root3[4096];
    snprintf(root3, sizeof root3, "%s/pw3", test_tmpdir());
    CHECK(mkdir_p(root3) == 0);
    g_sysroot = root3;
    mkfile_at(test_path("pw3/proc/meminfo"), "MemTotal: 12582912 kB\nMemAvailable: 8388608 kB\n");
    fake_cpus(root3, (int)conf, "1024\n");
    set_temp(root3, 35);
    mkfile_at(test_path("pw3/proc/pressure/memory"), "some avg10=20.00 avg60=0.00 avg300=0.00\n");
    perf_detect(&p);
    int before = (8 < (p.cpu_budget * 2) ? 8 : p.cpu_budget * 2);
    n = payload_workers();
    CHECK(n <= before / 2 + 1);

    char root4[4096];
    snprintf(root4, sizeof root4, "%s/pw4", test_tmpdir());
    CHECK(mkdir_p(root4) == 0);
    g_sysroot = root4;
    mkfile_at(test_path("pw4/proc/meminfo"), "MemTotal: 1572864 kB\nMemAvailable: 1048576 kB\n");
    fake_cpus(root4, (int)conf, "1024\n");
    set_temp(root4, 35);
    CHECK(payload_workers() == 1);

    g_sysroot = "";
    fprintf(stderr, "t_payload_workers: ok\n");
    return 0;
}
