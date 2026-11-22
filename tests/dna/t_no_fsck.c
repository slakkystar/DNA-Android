#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static void script(const char *dir, const char *name, const char *body) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    test_write(path, body);
    CHECK(chmod(path, 0755) == 0);
}

static void fixture(const char *pro) {
    char dir[4096], file[4096];
    snprintf(dir, sizeof dir, "%s/fsck", pro);
    snprintf(file, sizeof file, "%s/blob", dir);
    CHECK(mkdir(dir, 0755) == 0);
    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, 1u << 20) == 0);
    CHECK(close(fd) == 0);
    CHECK(mkdir(test_path("proj/config"), 0755) == 0);
    test_write(test_path("proj/config/fsck_fs_config"), "");
    CHECK(mkdir(test_path("proj/out"), 0755) == 0);
}

int main(void) {
    char pro[4096], bin[4096], path[8192], flog[4096], rlog[4096];
    snprintf(pro, sizeof pro, "%s", test_path("proj"));
    CHECK(mkdir(pro, 0755) == 0);
    fixture(pro);
    snprintf(bin, sizeof bin, "%s", test_path("bin"));
    CHECK(mkdir(bin, 0755) == 0);
    snprintf(flog, sizeof flog, "%s", test_path("fsck.log"));
    snprintf(rlog, sizeof rlog, "%s", test_path("resize.log"));
    script(bin, "mke2fs", "#!/bin/sh\nexit 0\n");
    script(bin, "e2fsdroid", "#!/bin/sh\nexit 0\n");
    script(bin, "e2fsck", "#!/bin/sh\n: > \"$FSCK_LOG\"\nexit 0\n");
    script(bin, "resize2fs", "#!/bin/sh\n: > \"$RESIZE_LOG\"\nexit 0\n");
    snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("FSCK_LOG", flog, 1);
    setenv("RESIZE_LOG", rlog, 1);

    ropts_t o;
    memset(&o, 0, sizeof o);
    o.rw = 1;
    o.autosize = 1;
    o.shrink = 1;

    part_start();
    CHECK(repack_ext4("fsck", pro, pro, &o) == 0);
    CHECK(access(flog, F_OK) == 0);
    CHECK(access(rlog, F_OK) == 0);

    unlink(flog);
    o.nofsck = 1;
    part_start();
    CHECK(repack_ext4("fsck", pro, pro, &o) == 0);
    CHECK(access(flog, F_OK) != 0);
    CHECK(access(rlog, F_OK) == 0);

    fprintf(stderr, "t_no_fsck: ok\n");
    return 0;
}
