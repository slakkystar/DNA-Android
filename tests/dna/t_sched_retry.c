#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

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
          "workers=0\n"
          "for a in \"$@\"; do\n"
          "  case \"$a\" in *.img.tmp) out=\"$a\";; --workers=*) workers=${a#--workers=};; esac\n"
          "done\n"
          "name=$(basename \"$out\" .img.tmp)\n"
          "echo \"S $name $workers\" >> \"$RETRY_LOG\"\n"
          "state=\"$RETRY_STATE/$name\"\n"
          "if [ \"$name\" = \"$RETRY_FAIL\" ] && [ ! -f \"$state\" ]; then\n"
          "  : > \"$state\"\n"
          "  kill -9 $PPID\n"
          "fi\n"
          "sleep 0.2\n"
          ": > \"$out\"\n"
          "echo \"E $name $workers\" >> \"$RETRY_LOG\"\n", f);
    fclose(f);
    CHECK(chmod(path, 0755) == 0);
}

int main(void) {
    char pro[4096], bin[4096], log[4096], state[4096];
    snprintf(pro, sizeof pro, "%s/proj", test_tmpdir());
    snprintf(bin, sizeof bin, "%s/bin", test_tmpdir());
    snprintf(log, sizeof log, "%s/retry.log", test_tmpdir());
    snprintf(state, sizeof state, "%s/state", test_tmpdir());
    CHECK(mkdir(pro, 0755) == 0);
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
    CHECK(mkdir(state, 0755) == 0);
    make_tree(pro, "system", 3u << 20);
    make_tree(pro, "vendor", 2u << 20);
    make_tree(pro, "product", 1u << 20);
    make_tool(bin);

    char path[8192];
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("RETRY_LOG", log, 1);
    setenv("RETRY_STATE", state, 1);
    setenv("RETRY_FAIL", "system", 1);
    setenv("DNA_PRO", pro, 1);
    setenv("DNA_DRO", pro, 1);
    setenv("DNA_JOBS", "2", 1);
    setenv("DNA_EROFS_WORKERS", "2", 1);
    setenv("DNA_SPACE_CHECK", "0", 1);

    char *argv[] = { "system", "vendor", "product", "--profile", "fast", "--jobs", "2",
                     "--erofs-workers", "2", "--convert", "1", "--format", "0", "--auto", "1", NULL };
    CHECK(cmd_repack((int)(sizeof argv / sizeof argv[0]) - 1, argv) == 0);

    FILE *f = fopen(log, "r");
    CHECK(f != NULL);
    char tag, name[64];
    int workers;
    char seq[16][64];
    int ws[16], n = 0;
    while (fscanf(f, " %c %63s %d", &tag, name, &workers) == 3 && n < 16) {
        if (tag != 'S') continue;
        snprintf(seq[n], sizeof seq[n], "%s", name);
        ws[n] = workers;
        n++;
    }
    fclose(f);
    CHECK(n == 4);
    CHECK(strcmp(seq[2], "system") == 0);
    CHECK(strcmp(seq[3], "product") == 0);
    CHECK(ws[0] == 2 && ws[1] == 2);
    CHECK(ws[2] == 1);

    fprintf(stderr, "t_sched_retry: ok\n");
    return 0;
}
