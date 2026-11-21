#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"
#include <sys/wait.h>

static void make_tree(const char *pro, const char *name, size_t size) {
    char dir[4096], file[4096];
    snprintf(dir, sizeof dir, "%s/%s", pro, name);
    snprintf(file, sizeof file, "%s/blob", dir);
    CHECK(mkdir(dir, 0755) == 0);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)size) == 0);
    CHECK(close(fd) == 0);
}

static void make_tool(const char *bin) {
    CHECK(mkdir(bin, 0755) == 0);
    const char *path = test_path("bin/mkfs.erofs");
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\n"
          "out=\n"
          "for a in \"$@\"; do case \"$a\" in *.img.tmp) out=\"$a\";; esac; done\n"
          "name=$(basename \"$out\" .img.tmp)\n"
          "echo \"S $name\" >> \"$SCHED_LOG\"\n"
          "if [ \"$name\" = \"$SCHED_FAIL\" ]; then exit 1; fi\n"
          "sleep \"${SCHED_SLEEP:-1}\"\n"
          ": > \"$out\"\n"
          "echo \"E $name\" >> \"$SCHED_LOG\"\n", f);
    fclose(f);
    CHECK(chmod(path, 0755) == 0);
}

static int log_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) { found = 1; break; }
    fclose(f);
    return found;
}

static int max_overlap(const char *path, char first[64], char last[64]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char tag, name[64];
    int cur = 0, mx = 0, ns = 0;
    first[0] = last[0] = 0;
    while (fscanf(f, " %c %63s", &tag, name) == 2) {
        if (tag == 'S') {
            if (!ns++) snprintf(first, 64, "%s", name);
            snprintf(last, 64, "%s", name);
            if (++cur > mx) mx = cur;
        } else if (tag == 'E') {
            if (cur) cur--;
        }
    }
    fclose(f);
    return mx;
}

static int run_repack(const char *pro) {
    char *argv[] = { "system", "vendor", "product",
                     "--profile", "fast", "--jobs", "2", "--erofs-workers", "2",
                     "--convert", "1", "--format", "0", "--auto", "1", NULL };
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", pro, 1);
    setenv("DNA_JOBS", "2", 1);
    setenv("DNA_EROFS_WORKERS", "2", 1);
    setenv("DNA_SPACE_CHECK", "0", 1);
    return cmd_repack((int)(sizeof argv / sizeof argv[0]) - 1, argv);
}

int main(void) {
    char pro[4096], bin[4096], log[4096];
    snprintf(pro, sizeof pro, "%s/proj", test_tmpdir());
    snprintf(bin, sizeof bin, "%s/bin", test_tmpdir());
    snprintf(log, sizeof log, "%s/sched.log", test_tmpdir());
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    make_tree(pro, "system", 3u << 20);
    make_tree(pro, "vendor", 2u << 20);
    make_tree(pro, "product", 1u << 20);
    make_tool(bin);

    char path[8192];
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("SCHED_LOG", log, 1);
    setenv("SCHED_SLEEP", "0.6", 1);
    unsetenv("SCHED_FAIL");

    CHECK(run_repack(pro) == 0);
    char first[64], last[64];
    int mx = max_overlap(log, first, last);
    CHECK(mx == 2);
    CHECK(strcmp(first, "system") == 0 || strcmp(first, "vendor") == 0);
    CHECK(strcmp(last, "product") == 0);

    remove(log);
    setenv("SCHED_FAIL", "system", 1);
    CHECK(run_repack(pro) == 1);
    CHECK(log_has(log, "S system"));
    CHECK(!log_has(log, "S product"));
    unsetenv("SCHED_FAIL");

    remove(log);
    remove(test_path("proj/out/system.img"));
    setenv("SCHED_SLEEP", "30", 1);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(run_repack(pro));
    for (int i = 0; i < 50 && !log_has(log, "S system"); i++) {
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    CHECK(log_has(log, "S system"));
    kill(pid, SIGTERM);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 130);
    CHECK(access(test_path("proj/out/system.img.tmp"), F_OK) != 0);

    fprintf(stderr, "t_sched: ok\n");
    return 0;
}
