#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static uint64_t fake_free(const char *path) {
    (void)path;
    return 0;
}

static uint64_t fake_huge(const char *path) {
    (void)path;
    return 16ULL << 30;
}

int main(void) {
    char pro[4096];
    snprintf(pro, sizeof pro, "%s", test_path("proj"));
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(test_path("proj/system"), 0755) == 0);
    CHECK(mkdir(test_path("proj/config"), 0755) == 0);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    test_write(test_path("proj/system/file"), "hello");
    test_write(test_path("proj/config/system_fs_config"), "/system 0 0 0755\n/system/file 0 0 0644\n");
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", pro, 1);
    unsetenv("DNA_TMP");
    unsetenv("TMPDIR");
    unsetenv("DNA_SPACE_CHECK");
    char *names[] = { "system" };
    ropts_t o;
    memset(&o, 0, sizeof o);
    o.autosize = 1;
    o.out_kind = 0;

    g_free_fn = fake_free;
    CHECK(perf_check_space(pro, pro, names, 1, 0, &o, 1) == 1);
    g_free_fn = fake_huge;
    CHECK(perf_check_space(pro, pro, names, 1, 0, &o, 1) == 0);

    setenv("DNA_SPACE_CHECK", "0", 1);
    g_free_fn = fake_free;
    CHECK(perf_check_space(pro, pro, names, 1, 0, &o, 1) == 0);
    unsetenv("DNA_SPACE_CHECK");
    g_free_fn = NULL;

    fprintf(stderr, "t_space: ok\n");
    return 0;
}
