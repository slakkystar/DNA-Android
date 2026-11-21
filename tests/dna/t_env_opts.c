#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static char *slurp(const char *path) {
    static char buf[4096];
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return buf; }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void empty(const char *path) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fclose(f);
}

int main(void) {
    char root[4096];
    snprintf(root, sizeof root, "%s/env", test_tmpdir());
    CHECK(mkdir_p(test_path("env/sys/power")) == 0);
    CHECK(mkdir_p(test_path("env/dev/cpuset/top-app")) == 0);
    empty(test_path("env/sys/power/wake_lock"));
    empty(test_path("env/sys/power/wake_unlock"));
    empty(test_path("env/dev/cpuset/top-app/tasks"));
    g_sysroot = root;

    setenv("DNA_WAKELOCK", "1", 1);
    start_wakelock();
    CHECK(g_wake_locked == 1);
    CHECK(strcmp(slurp(test_path("env/sys/power/wake_lock")), "dna-repack") == 0);
    stop_handler(SIGTERM);
    CHECK(g_stop == 1);
    CHECK(g_wake_locked == 0);
    CHECK(strcmp(slurp(test_path("env/sys/power/wake_unlock")), "dna-repack") == 0);
    g_stop = 0;

    empty(test_path("env/sys/power/wake_lock"));
    unsetenv("DNA_WAKELOCK");
    start_wakelock();
    CHECK(g_wake_locked == 0);
    CHECK(strcmp(slurp(test_path("env/sys/power/wake_lock")), "") == 0);

    setenv("DNA_CPUSET", "top-app", 1);
    cpuset_add_self();
    char pidbuf[64];
    snprintf(pidbuf, sizeof pidbuf, "%d\n", (int)getpid());
    CHECK(strcmp(slurp(test_path("env/dev/cpuset/top-app/tasks")), pidbuf) == 0);
    empty(test_path("env/dev/cpuset/top-app/tasks"));
    setenv("DNA_CPUSET", "../top-app", 1);
    cpuset_add_self();
    CHECK(strcmp(slurp(test_path("env/dev/cpuset/top-app/tasks")), "") == 0);
    unsetenv("DNA_CPUSET");

    char oom[4096];
    snprintf(oom, sizeof oom, "%s", test_path("oom_adj"));
    g_oom_path = oom;
    empty(oom);
    setenv("DNA_PROTECT", "1", 1);
    setenv("DNA_OOM_SCORE_ADJ", "-500", 1);
    oom_protect();
    CHECK(strcmp(slurp(oom), "-300\n") == 0);
    setenv("DNA_OOM_SCORE_ADJ", "50", 1);
    oom_protect();
    CHECK(strcmp(slurp(oom), "-250\n") == 0);
    unsetenv("DNA_OOM_SCORE_ADJ");
    oom_protect();
    CHECK(strcmp(slurp(oom), "-250\n") == 0);
    empty(oom);
    unsetenv("DNA_PROTECT");
    setenv("DNA_OOM_SCORE_ADJ", "-300", 1);
    oom_protect();
    CHECK(strcmp(slurp(oom), "") == 0);
    unsetenv("DNA_OOM_SCORE_ADJ");

    g_sysroot = "";
    g_oom_path = "/proc/self/oom_score_adj";
    fprintf(stderr, "t_env_opts: ok\n");
    return 0;
}
