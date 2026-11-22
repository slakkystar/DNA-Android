#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"
#include <sys/wait.h>
#include <signal.h>

static void make_dir(const char *p) { CHECK(mkdir(p, 0755) == 0 || errno == EEXIST); }

static void make_tree(const char *pro, const char *name, size_t size) {
    char dir[4096], file[4096];
    snprintf(dir, sizeof dir, "%s/%s", pro, name);
    make_dir(dir);
    snprintf(file, sizeof file, "%s/blob", dir);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)size) == 0);
    CHECK(close(fd) == 0);
}

static void make_tool(const char *bin) {
    make_dir(bin);
    const char *path = test_path("bin/mkfs.erofs");
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\n"
          "out=\n"
          "for a in \"$@\"; do case \"$a\" in *.img.tmp) out=\"$a\";; esac; done\n"
          "name=$(basename \"$out\" .img.tmp)\n"
          "echo $$ > \"$STOP_LOG.pid.$name\"\n"
          "echo \"S $name\" >> \"$STOP_LOG\"\n"
          "if [ \"$name\" = hang ]; then sleep 30; fi\n"
          "sleep 0.2\n"
          ": > \"$out\"\n", f);
    fclose(f);
    CHECK(chmod(path, 0755) == 0);
}

static int has_line(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024]; int found = 0;
    while (fgets(line, sizeof line, f)) if (strstr(line, needle)) { found = 1; break; }
    fclose(f);
    return found;
}

static void wait_line(const char *path, const char *needle) {
    for (int i = 0; i < 100; i++) {
        if (has_line(path, needle)) return;
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    CHECK(0 && "tool did not start");
}

static int run_repack(const char *pro) {
    char *argv[] = { "system", "hang", "--profile", "saver", "--jobs", "1",
                     "--convert", "1", "--format", "0", "--auto", "1", NULL };
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", pro, 1);
    setenv("DNA_SPACE_CHECK", "0", 1);
    return cmd_repack((int)(sizeof argv / sizeof argv[0]) - 1, argv);
}

static pid_t start_build(const char *pro) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { int rc = run_repack(pro); fflush(NULL); _exit(rc); }
    return pid;
}

static int build_exit(pid_t pid) {
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 129;
}

static int read_pid(const char *path) {
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    int pid = 0;
    CHECK(fscanf(f, "%d", &pid) == 1);
    fclose(f);
    return pid;
}

static int pid_gone(pid_t pid) {
    for (int i = 0; i < 50; i++) {
        if (kill(pid, 0) < 0 && errno == ESRCH) return 1;
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int main(void) {
    char pro[4096], log[4096], bin[4096], runroot[4096];
    snprintf(pro, sizeof pro, "%s/proj", test_tmpdir());
    snprintf(log, sizeof log, "%s/stop.log", test_tmpdir());
    snprintf(bin, sizeof bin, "%s/bin", test_tmpdir());
    snprintf(runroot, sizeof runroot, "%s/run", test_tmpdir());
    make_dir(pro); make_dir(test_path("proj/out")); make_dir(runroot);
    make_tree(pro, "system", 1u << 20);
    make_tree(pro, "hang", 1u << 20);
    make_tool(bin);
    char path[8192];
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("STOP_LOG", log, 1);
    setenv("DNA_TMP", runroot, 1);
    test_write(test_path("proj/out/foreign.bin"), "keep");

    remove(log);
    pid_t build = start_build(pro);
    wait_line(log, "S hang");
    CHECK(cmd_stop(0, NULL) == 0);
    CHECK(build_exit(build) == 130);
    CHECK(access(test_path("proj/out/system.img"), F_OK) != 0);
    CHECK(access(test_path("proj/out/hang.img.tmp"), F_OK) != 0);
    CHECK(access(test_path("proj/out/foreign.bin"), F_OK) == 0);

    remove(log);
    remove(test_path("run/.dna-run/system.run"));
    build = start_build(pro);
    wait_line(log, "S hang");
    pid_t tool = read_pid(test_path("stop.log.pid.hang"));
    CHECK(kill(build, SIGKILL) == 0);
    CHECK(build_exit(build) == 129);
    CHECK(access(test_path("proj/out/system.img"), F_OK) == 0);
    CHECK(cmd_stop(0, NULL) == 0);
    CHECK(access(test_path("proj/out/system.img"), F_OK) == 0);
    CHECK(access(test_path("proj/out/hang.img.tmp"), F_OK) != 0);
    CHECK(pid_gone(tool));

    char empty[4096];
    snprintf(empty, sizeof empty, "%s/empty", test_tmpdir());
    make_dir(empty);
    setenv("DNA_TMP", empty, 1);
    CHECK(cmd_stop(0, NULL) == 0);

    fprintf(stderr, "t_stop_cmd: ok\n");
    return 0;
}
