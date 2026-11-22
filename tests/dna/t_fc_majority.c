#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static int ref_majority(const fc_ent_t *e, size_t n, const char *parent, char *out, size_t on) {
    int best = 0, second = 0;
    const char *bc = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(e[i].parent, parent)) continue;
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(e[j].parent, parent) && !strcmp(e[j].ctx, e[i].ctx)) { seen = 1; break; }
        if (seen) continue;
        int cnt = 0;
        for (size_t j = i; j < n; j++)
            if (!strcmp(e[j].parent, parent) && !strcmp(e[j].ctx, e[i].ctx)) cnt++;
        if (cnt > best) { second = best; best = cnt; bc = e[i].ctx; }
        else if (cnt > second) second = cnt;
    }
    if (best >= 2 && best > second) { snprintf(out, on, "%s", bc); return 1; }
    return 0;
}

static unsigned rs = 0x9e3779b9u;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

static void check(const fcx_ctx_t *c, const char *parent) {
    char a[512], b[512];
    int x = fc_majority_ctx((fcx_ctx_t *)c, parent, a, sizeof a);
    int y = ref_majority(c->ents, c->nents, parent, b, sizeof b);
    if (x != y || (x && strcmp(a, b))) {
        fprintf(stderr, "mismatch parent=%s got=%d/%s exp=%d/%s n=%zu\n",
                parent, x, x ? a : "-", y, y ? b : "-", c->nents);
        exit(1);
    }
}

int main(void) {
    fc_ent_t ents[64];
    char pbuf[64][32], cbuf[64][32];
    fcx_ctx_t c;
    memset(&c, 0, sizeof c);
    c.ents = ents;
    const char *parents[] = { "/system", "/system/bin", "/vendor", "/vendor/lib", "/product" };
    const char *ctxs[] = { "u:object_r:a:s0", "u:object_r:b:s0", "u:object_r:c:s0", "u:object_r:d:s0" };
    int ops = 0;
    while (ops < 10000) {
        memset(&c, 0, sizeof c);
        c.ents = ents;
        for (int i = 0; i < 64 && ops < 10000; i++, ops++) {
            snprintf(pbuf[i], sizeof pbuf[i], "%s", parents[rnd() % 5]);
            snprintf(cbuf[i], sizeof cbuf[i], "%s", ctxs[rnd() % 4]);
            ents[i].parent = pbuf[i];
            ents[i].ctx = cbuf[i];
            ents[i].path = (char *)"";
            c.nents++;
            if (rnd() % 3 == 0) check(&c, parents[rnd() % 5]);
        }
        for (int i = 0; i < 5; i++) check(&c, parents[i]);
        maj_free(&c.maj);
    }

    memset(&c, 0, sizeof c);
    c.ents = ents;
    for (int i = 0; i < 3; i++) { ents[i].parent = (char *)"/x"; ents[i].ctx = (char *)"a"; ents[i].path = (char *)""; }
    for (int i = 3; i < 6; i++) { ents[i].parent = (char *)"/x"; ents[i].ctx = (char *)"b"; ents[i].path = (char *)""; }
    c.nents = 6;
    for (int i = 0; i < 5; i++) check(&c, "/x");
    maj_free(&c.maj);

    fprintf(stderr, "t_fc_majority: ok\n");
    return 0;
}
