#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void base(perf_t *p) {
    memset(p, 0, sizeof *p);
    p->mem_total_kb = 12ULL * 1024 * 1024;
    p->mem_avail_kb = 6ULL * 1024 * 1024;
    p->ncpu_online = 8;
    p->ncpu_conf = 8;
    p->big_cores = 4;
    p->little_cores = 4;
    p->cpu_budget = 7;
    p->storage = STORAGE_UFS;
}

int main(void) {
    perf_t p;
    base(&p);
    CHECK(perf_select_class(&p) == PERF_TURBO);

    p.mem_total_kb = 2ULL * 1024 * 1024;
    CHECK(perf_select_class(&p) == PERF_SAVER);
    p.mem_total_kb = 5ULL * 1024 * 1024;
    CHECK(perf_select_class(&p) == PERF_BALANCED);
    base(&p); p.ncpu_online = 4;
    CHECK(perf_select_class(&p) == PERF_BALANCED);
    base(&p); p.temp_soc_c = 90;
    CHECK(perf_select_class(&p) == PERF_SAVER);
    base(&p); p.temp_battery_c = 41;
    CHECK(perf_select_class(&p) == PERF_BALANCED);

    base(&p);
    perf_load_t ld = { 1, 8, 1, 0, 0 };
    perf_ovr_t ov;
    memset(&ov, 0, sizeof ov);
    perf_make_plan(&p, &ld, &ov);
    CHECK(p.cls == PERF_TURBO);
    CHECK(p.part_jobs == 3);
    CHECK(p.erofs_workers == 2);
    CHECK(p.nice_level == 0);
    CHECK(p.zstd_threads >= 1);

    base(&p);
    ld = (perf_load_t){ 0, 1, 0, 0, 0 };
    perf_make_plan(&p, &ld, &ov);
    CHECK(p.part_jobs == 1);
    CHECK(p.erofs_workers <= 12);

    base(&p);
    p.storage = STORAGE_EMMC;
    ld = (perf_load_t){ 0, 8, 0, 0, 0 };
    perf_make_plan(&p, &ld, &ov);
    CHECK(p.part_jobs <= 2);

    base(&p);
    ld = (perf_load_t){ 0, 0, 0, 2, 0 };
    perf_make_plan(&p, &ld, &ov);
    CHECK(p.part_jobs == 1 && p.erofs_workers == 1 && p.brotli_jobs == 1);

    base(&p);
    ld = (perf_load_t){ 0, 8, 0, 0, 2 };
    memset(&ov, 0, sizeof ov);
    ov.has_jobs = ov.has_erofs_workers = ov.has_brotli_jobs = ov.has_zstd_threads = ov.has_nice = ov.has_reserve = 1;
    ov.jobs = 1; ov.erofs_workers = 1; ov.brotli_jobs = 1; ov.zstd_threads = 1; ov.nice_level = 12; ov.reserve_mb = 2048;
    perf_make_plan(&p, &ld, &ov);
    CHECK(p.part_jobs == 1 && p.erofs_workers == 1 && p.brotli_jobs == 1 && p.zstd_threads == 1);
    CHECK(p.nice_level == 12 && p.reserve_mb == 2048);

    for (int gb = 1; gb <= 16; gb++) {
        for (int nc = 2; nc <= 12; nc += 2) {
            for (int st = 0; st < 3; st++) {
                for (int hot = 0; hot < 3; hot++) {
                    base(&p);
                    p.mem_total_kb = (uint64_t)gb * 1024 * 1024;
                    p.mem_avail_kb = (uint64_t)gb * 1024 * 1024 / 2;
                    p.ncpu_online = nc;
                    p.ncpu_conf = nc;
                    p.big_cores = nc / 2 + nc % 2;
                    p.little_cores = nc / 2;
                    p.cpu_budget = p.big_cores + p.little_cores / 2;
                    if (p.cpu_budget < 1) p.cpu_budget = 1;
                    if (p.cpu_budget > 12) p.cpu_budget = 12;
                    p.storage = st == 0 ? STORAGE_EMMC : (st == 1 ? STORAGE_UFS : STORAGE_UNKNOWN);
                    p.temp_soc_c = hot == 0 ? 30 : (hot == 1 ? 85 : 95);
                    memset(&ov, 0, sizeof ov);
                    ld = (perf_load_t){ 1, 4, 1, 1, 2 };
                    perf_make_plan(&p, &ld, &ov);
                    CHECK(p.part_jobs >= 1);
                    CHECK(p.erofs_workers >= 1);
                    CHECK(p.brotli_jobs >= 1);
                    CHECK(p.zstd_threads >= 1);
                    CHECK(p.part_jobs <= 8);
                    CHECK(p.erofs_workers <= 12);
                    if (st == 0) CHECK(p.part_jobs <= 2);
                    int cpu_b = p.cpu_budget;
                    if (p.part_jobs * p.erofs_workers > (cpu_b > p.part_jobs ? cpu_b : p.part_jobs)) {
                        fprintf(stderr, "grid failure gb=%d nc=%d st=%d hot=%d part=%d workers=%d cpu=%d\n", gb, nc, st, hot, p.part_jobs, p.erofs_workers, cpu_b);
                        return 1;
                    }
                }
            }
        }
    }

    base(&p);
    setenv("DNA_PERF", "turbo", 1);
    setenv("DNA_JOBS", "5", 1);
    setenv("DNA_EROFS_WORKERS", "3", 1);
    setenv("DNA_BROTLI_JOBS", "2", 1);
    setenv("DNA_ZSTD_THREADS", "5", 1);
    setenv("DNA_NICE", "-2", 1);
    setenv("DNA_MEM_RESERVE_MB", "1234", 1);
    perf_ovr_from_env(&ov);
    CHECK(ov.has_profile && ov.profile == PERF_TURBO);
    CHECK(ov.has_jobs && ov.jobs == 5);
    CHECK(ov.has_erofs_workers && ov.erofs_workers == 3);
    CHECK(ov.has_brotli_jobs && ov.brotli_jobs == 2);
    CHECK(ov.has_zstd_threads && ov.zstd_threads == 5);
    CHECK(ov.has_nice && ov.nice_level == -2);
    CHECK(ov.has_reserve && ov.reserve_mb == 1234);
    setenv("DNA_JOBS", "abc", 1);
    perf_ovr_from_env(&ov);
    CHECK(!ov.has_jobs);
    setenv("DNA_PERF", "bogus", 1);
    perf_ovr_from_env(&ov);
    CHECK(!ov.has_profile);
    unsetenv("DNA_PERF"); unsetenv("DNA_JOBS"); unsetenv("DNA_EROFS_WORKERS");
    unsetenv("DNA_BROTLI_JOBS"); unsetenv("DNA_ZSTD_THREADS"); unsetenv("DNA_NICE"); unsetenv("DNA_MEM_RESERVE_MB");

    fprintf(stderr, "t_perf_plan: ok\n");
    return 0;
}
