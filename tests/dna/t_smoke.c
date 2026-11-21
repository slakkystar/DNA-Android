#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

int main(void) {
    CHECK(meminfo_kb("MemTotal") > 0);
    puts("t_smoke: ok");
    return 0;
}
