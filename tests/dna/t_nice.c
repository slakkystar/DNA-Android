#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"
#include <sys/stat.h>

static int nice_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    char line[4096];
    CHECK(fgets(line, sizeof line, f) != NULL);
    fclose(f);
    char *p = line;
    for (int field = 1; field <= 19; field++) {
        while (*p == ' ') p++;
        if (field == 19) return atoi(p);
        while (*p && *p != ' ') p++;
    }
    return -999;
}

int main(void) {
    char tools[4096], outfile[4096];
    snprintf(tools, sizeof tools, "%s/tools", test_tmpdir());
    snprintf(outfile, sizeof outfile, "%s", test_path("nice.txt"));
    CHECK(mkdir(tools, 0755) == 0);
    const char *script = test_path("tools/nicefake");
    FILE *f = fopen(script, "w");
    CHECK(f != NULL);
    fputs("#!/bin/sh\ncat /proc/self/stat > \"$NICEFILE\"\n", f);
    fclose(f);
    CHECK(chmod(script, 0755) == 0);
    char path[8192];
    snprintf(path, sizeof path, "%s:%s", tools, getenv("PATH"));
    setenv("PATH", path, 1);
    setenv("NICEFILE", outfile, 1);

    char *argv[] = { "nicefake", NULL };
    g_nice = 10;
    remove(outfile);
    CHECK(run(argv) == 0);
    CHECK(nice_from_file(outfile) == 10);

    g_nice = -100;
    int inherited = getpriority(PRIO_PROCESS, 0);
    remove(outfile);
    CHECK(run(argv) == 0);
    CHECK(nice_from_file(outfile) == inherited);

    fprintf(stderr, "t_nice: ok\n");
    return 0;
}
