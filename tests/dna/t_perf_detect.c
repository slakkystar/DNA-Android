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

static void fake_cpu(const char *root, int cpu, const char *name, const char *value) {
    char p[4096];
    snprintf(p, sizeof p, "%s/sys/devices/system/cpu/cpu%d/%s", root, cpu, name);
    mkfile_at(p, value);
}

int main(void) {
    char root[4096];
    snprintf(root, sizeof root, "%s/sysroot", test_tmpdir());
    CHECK(mkdir_p(root) == 0);
    g_sysroot = root;

    mkfile_at(test_path("sysroot/proc/meminfo"),
              "MemTotal:       12582912 kB\n"
              "MemAvailable:    8388608 kB\n"
              "SwapTotal:       2097152 kB\n"
              "SwapFree:        1048576 kB\n");
    long conf = sysconf(_SC_NPROCESSORS_CONF);
    if (conf < 1) conf = 1;
    if (conf > 256) conf = 256;
    for (int i = 0; i < (int)conf; i++) fake_cpu(root, i, "cpu_capacity", i < 4 ? "1024\n" : "512\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone0/type"), "battery\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone0/temp"), "43000\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone1/type"), "skin-therm\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone1/temp"), "365\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone2/type"), "soc_cpu\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone2/temp"), "43\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone3/type"), "bad\n");
    mkfile_at(test_path("sysroot/sys/class/thermal/thermal_zone3/temp"), "200\n");
    mkfile_at(test_path("sysroot/proc/pressure/memory"), "some avg10=12.90 avg60=0.00 avg300=0.00 total=0\n");
    mkfile_at(test_path("sysroot/proc/pressure/cpu"), "some avg10=3.50 avg60=0.00 avg300=0.00 total=0\n");
    mkfile_at(test_path("sysroot/proc/pressure/io"), "some avg10=1.10 avg60=0.00 avg300=0.00 total=0\n");
    mkfile_at(test_path("sysroot/sys/block/mmcblk0/queue/rotational"), "0\n");

    perf_t p;
    perf_detect(&p);
    CHECK(p.mem_total_kb == 12582912ULL);
    CHECK(p.mem_avail_kb == 8388608ULL);
    CHECK(p.swap_total_kb == 2097152ULL);
    CHECK(p.swap_free_kb == 1048576ULL);
    int big = conf < 4 ? (int)conf : 4;
    CHECK(p.big_cores == big);
    CHECK(p.little_cores == (int)conf - big);
    CHECK(p.cpu_budget >= 1 && p.cpu_budget <= 12);
    CHECK(p.temp_battery_c == 43);
    CHECK(p.temp_skin_c == 36);
    CHECK(p.temp_soc_c == 43);
    CHECK(p.psi_mem_avg10 == 12);
    CHECK(p.psi_cpu_avg10 == 3);
    CHECK(p.psi_io_avg10 == 1);
    CHECK(p.storage == STORAGE_EMMC);

    CHECK(perf_thermal_level(&p) == 2);
    p.temp_battery_c = 0; p.temp_skin_c = 0; p.temp_soc_c = 80; CHECK(perf_thermal_level(&p) == 1);
    p.temp_soc_c = 90; CHECK(perf_thermal_level(&p) == 2);
    p.temp_soc_c = 98; CHECK(perf_thermal_level(&p) == 3);
    p.temp_soc_c = 0; p.temp_battery_c = 46; p.temp_skin_c = 0; CHECK(perf_thermal_level(&p) == 3);

    setenv("DNA_THERMAL_BATTERY", "30,31,32", 1);
    p.temp_battery_c = 31; p.temp_soc_c = 0; p.temp_skin_c = 0;
    CHECK(perf_thermal_level(&p) == 2);
    unsetenv("DNA_THERMAL_BATTERY");

    char root2[4096];
    snprintf(root2, sizeof root2, "%s/sysroot2", test_tmpdir());
    CHECK(mkdir_p(root2) == 0);
    g_sysroot = root2;
    mkfile_at(test_path("sysroot2/sys/block/sda/queue/rotational"), "0\n");
    mkfile_at(test_path("sysroot2/sys/class/ufs/ufshcd0/power"), "1\n");
    mkfile_at(test_path("sysroot2/proc/meminfo"), "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 50 kB\n");
    for (int i = 0; i < (int)conf; i++) fake_cpu(root2, i, "cpufreq/cpuinfo_max_freq", i < 2 ? "1800000\n" : "1000000\n");
    perf_detect(&p);
    CHECK(p.storage == STORAGE_UFS);
    CHECK(p.mem_avail_kb == 200);
    CHECK(p.big_cores == (conf < 2 ? (int)conf : 2));

    char root3[4096];
    snprintf(root3, sizeof root3, "%s/sysroot3", test_tmpdir());
    CHECK(mkdir_p(root3) == 0);
    g_sysroot = root3;
    perf_detect(&p);
    CHECK(p.mem_total_kb == 0);
    CHECK(p.psi_mem_avg10 == 0);
    CHECK(p.temp_battery_c == 0);
    CHECK(p.big_cores >= 1);

    g_sysroot = "";
    fprintf(stderr, "t_perf_detect: ok\n");
    return 0;
}
