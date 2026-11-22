#define main dna_main
#include "../../pio/src/main/assets/tools/dna.c"
#undef main

#include "lib.h"

static int ref_token(const char *buf, const char *tok) {
    size_t n = strlen(tok);
    const char *p = buf;
    while ((p = strstr(p, tok))) {
        int left = (p == buf) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        int right = !(isalnum((unsigned char)p[n]) || p[n] == '_');
        if (left && right) return 1;
        p += n;
    }
    return 0;
}

static int ref_file(const char *path, const char *type) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > (128L << 20)) { fclose(f); return 0; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    int r = ref_token(buf, type);
    free(buf);
    return r;
}

static int ref_has(const char *type) {
    for (size_t i = 0; i < g_cil_n; i++)
        if (ref_file(g_cil_files[i], type)) return 1;
    return 0;
}

static unsigned rnd_state = 0x12345678u;
static unsigned rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void write_cil(const char *dir, const char *name, const char *data) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    test_write(path, data);
}

int main(void) {
    char root[4096], sub[4096];
    snprintf(root, sizeof root, "%s", test_path("dro"));
    snprintf(sub, sizeof sub, "%s", test_path("dro/sub"));
    CHECK(mkdir(root, 0755) == 0);
    CHECK(mkdir(sub, 0755) == 0);
    write_cil(root, "a.cil",
        "(type hal_foo)(allow x y (type hal_foo_default_exec))\n"
        "foo_bar baz-qux not_a_token\n# comment hal_comment_t\n(type hal_bar)\n");
    write_cil(root, "b.cil",
        "(xhal)(hal )(hal\n)(hal_)(_hal)(hal__x)(a hal b)(hal,foo)\n"
        "hal_foo hal_foo hal_foo\n");
    write_cil(sub, "c.cil", "alpha_beta gamma delta_gamma alpha_beta_delta\n");

    CHECK(fc_cil_has(root, "__probe__") == 0);
    const char *fixed[] = {
        "hal_foo", "hal_foo_default_exec", "type", "allow", "x", "y",
        "foo_bar", "baz", "qux", "not_a_token", "comment", "hal_comment_t",
        "hal_bar", "xhal", "hal", "hal_", "_hal", "hal__x", "a", "b", "foo",
        "alpha_beta", "gamma", "delta_gamma", "alpha_beta_delta", "probe",
        "hal_fo", "hal_fooo", "_hal_foo", "hal_foo_", "ALPHA_BETA", "baz_qux"
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        int got = fc_cil_has(root, fixed[i]);
        int exp = ref_has(fixed[i]);
        if (got != exp) fprintf(stderr, "mismatch: %s got=%d exp=%d\n", fixed[i], got, exp);
        CHECK(got == exp);
    }

    char q[64];
    for (int i = 0; i < 10000; i++) {
        unsigned n = 1 + rnd() % 12;
        size_t j = 0;
        for (unsigned k = 0; k < n; k++) {
            q[j++] = "abc_foo_hal_123XYZ"[rnd() % 17];
        }
        q[j] = 0;
        int got = fc_cil_has(root, q), exp = ref_has(q);
        if (got != exp) fprintf(stderr, "mismatch: '%s' got=%d exp=%d\n", q, got, exp);
        CHECK(got == exp);
    }

    char root2[4096];
    snprintf(root2, sizeof root2, "%s", test_path("dro2"));
    CHECK(mkdir(root2, 0755) == 0);
    write_cil(root2, "z.cil", "(type other_ctx) only_here\n");
    CHECK(fc_cil_has(root2, "other_ctx") == 1);
    CHECK(fc_cil_has(root2, "hal_foo") == 0);
    CHECK(fc_cil_has(root, "hal_foo") == 1);

    fprintf(stderr, "t_fc_cil: ok\n");
    return 0;
}
