#ifndef DNA_TEST_LIB_H
#define DNA_TEST_LIB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define SKIP(msg) do { \
    fprintf(stderr, "SKIP: %s\n", msg); \
    exit(0); \
} while (0)

static char *test_tmpdir(void) {
    static char path[4096];
    if (!path[0]) {
        const char *base = getenv("TMPDIR");
        if (!base || !*base) base = "/tmp";
        snprintf(path, sizeof path, "%s/dna-test-XXXXXX", base);
        if (!mkdtemp(path)) {
            perror("mkdtemp");
            exit(1);
        }
    }
    return path;
}

static char *test_path(const char *name) {
    static char path[4096];
    snprintf(path, sizeof path, "%s/%s", test_tmpdir(), name);
    return path;
}

static void test_write(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (data && fwrite(data, 1, strlen(data), f) != strlen(data)) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

static void test_write_bytes(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (len && fwrite(data, 1, len, f) != len) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

#endif
