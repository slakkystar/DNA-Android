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
          "sleep 0.25\n"
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
    if (pid == 0) {
        int rc = run_repack(pro);
        fflush(NULL);
        _exit(rc);
    }
    return pid;
}

static int wait_exit(pid_t pid) {
    for (int i = 0; i < 100; i++) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (r < 0 && errno != EINTR) return -1;
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -2;
}

static int pid_gone(pid_t pid) {
    if (pid <= 0) return 1;
    for (int i = 0; i < 50; i++) {
        if (kill(pid, 0) < 0 && errno == ESRCH) return 1;
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void dummy_handler(int sig) { (void)sig; }

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

    char foreign[4096]; snprintf(foreign, sizeof foreign, "%s/foreign.bin", test_path("proj/out"));
    test_write(foreign, "keep");
    CHECK(access(test_path("proj/out/system.img"), F_OK) != 0);
    remove(log);
    pid_t build = start_build(pro);
    wait_line(log, "S hang");
    CHECK(kill(build, SIGUSR1) == 0);
    CHECK(wait_exit(build) == 130);
    CHECK(access(test_path("proj/out/system.img"), F_OK) != 0);
    CHECK(access(test_path("proj/out/hang.img.tmp"), F_OK) != 0);
    CHECK(access(foreign, F_OK) == 0);

    FILE *pf = fopen(test_path("stop.log.pid.hang"), "r");
    CHECK(pf != NULL);
    pid_t tool_pid = 0;
    CHECK(fscanf(pf, "%d", &tool_pid) == 1);
    fclose(pf);
    CHECK(pid_gone(tool_pid));

    remove(log);
    build = start_build(pro);
    wait_line(log, "S hang");
    CHECK(kill(build, SIGTERM) == 0);
    CHECK(wait_exit(build) == 130);
    CHECK(access(test_path("proj/out/system.img"), F_OK) == 0);
    CHECK(access(test_path("proj/out/hang.img.tmp"), F_OK) != 0);
    CHECK(access(foreign, F_OK) == 0);

    char out[4096], jr[4096], outside[4096], sym[4096], dirn[4096];
    snprintf(out, sizeof out, "%s", test_path("proj/out"));
    snprintf(jr, sizeof jr, "%s/12345.run", test_path("run/.dna-run"));
    make_dir(test_path("run/.dna-run"));
    snprintf(outside, sizeof outside, "%s/outside.bin", test_tmpdir());
    snprintf(sym, sizeof sym, "%s/link", out);
    snprintf(dirn, sizeof dirn, "%s/adir", out);
    test_write(outside, "outside");
    test_write(test_path("proj/out/good.tmp"), "tmp");
    test_write(test_path("proj/out/good.out"), "out");
    CHECK(symlink(outside, sym) == 0);
    make_dir(dirn);
    char j[8192];
    snprintf(j, sizeof j,
             "START 12345 1\nDIR %s\nT %s/good.tmp\nO %s/good.out\n"
             "T %s/../outside.bin\nT %s\nT %s\nT %s\n",
             out, out, out, out, outside, sym, dirn);
    test_write(jr, j);
    g_stop = 0;
    CHECK(run_cleanup_file(jr, 2) == 130);
    CHECK(access(test_path("proj/out/good.tmp"), F_OK) != 0);
    CHECK(access(test_path("proj/out/good.out"), F_OK) != 0);
    CHECK(access(outside, F_OK) == 0);
    CHECK(access(sym, F_OK) == 0);
    CHECK(access(dirn, F_OK) == 0);
    CHECK(access(jr, F_OK) != 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = dummy_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);
    pid_t sender = fork();
    CHECK(sender >= 0);
    if (sender == 0) {
        struct timespec ts = { 0, 150000000 };
        nanosleep(&ts, NULL);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    g_stop = 0;
    char *sleep_argv[] = { "sleep", "0.5", NULL };
    CHECK(run(sleep_argv) == 0);
    CHECK(waitpid(sender, NULL, 0) == sender);
    signal(SIGUSR1, SIG_DFL);

    fprintf(stderr, "t_forcestop: ok\n");
    return 0;
}
