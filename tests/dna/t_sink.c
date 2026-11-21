#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

int main(void) {
    const char *path = test_path("sink.sparse");
    sink_t k;
    memset(&k, 0, sizeof k);
    k.o = fopen(path, "wb");
    CHECK(k.o != NULL);
    uint8_t hdr[28] = {0};
    CHECK(fwrite(hdr, 1, 28, k.o) == 28);
    k.sparse = 1;
    k.hdr = ftello(k.o);
    sk_chunk(&k, 0xCAC1, 1, 4096, 0, 0);
    uint8_t filler[4096];
    memset(filler, 0xCD, sizeof filler);
    CHECK(fwrite(filler, 1, 4096, k.o) == 4096);
    k.run = 1;
    k.rblocks = SINK_MAX_RAW_BLOCKS;
    k.blocks = SINK_MAX_RAW_BLOCKS;

    uint8_t blk[4096];
    for (int i = 0; i < 4096; i++) blk[i] = (uint8_t)i;
    sk_block(&k, blk);
    CHECK(k.run == 1);
    CHECK(k.rblocks == 1);
    CHECK(k.blocks == SINK_MAX_RAW_BLOCKS + 1);
    CHECK(!k.err);
    CHECK(sk_finish(&k, (SINK_MAX_RAW_BLOCKS + 1ULL) * 4096) == 0);
    CHECK(fclose(k.o) == 0);

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    uint8_t h[28];
    CHECK(fread(h, 1, 28, f) == 28);
    CHECK(le32(h) == 0xED26FF3Au);
    CHECK(le32(h + 20) == 2);
    CHECK(le32(h + 16) == SINK_MAX_RAW_BLOCKS + 1);
    uint8_t c1[12];
    CHECK(fread(c1, 1, 12, f) == 12);
    uint32_t b1 = le32(c1 + 4), t1 = le32(c1 + 8);
    CHECK(le16(c1) == 0xCAC1);
    CHECK(b1 == SINK_MAX_RAW_BLOCKS);
    CHECK((uint64_t)t1 == 12ull + (uint64_t)b1 * 4096);
    fclose(f);

    fprintf(stderr, "t_sink: ok\n");
    return 0;
}
