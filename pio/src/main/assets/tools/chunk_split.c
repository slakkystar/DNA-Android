/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_BUF_SIZE (1024 * 1024)

#define SPARSE_HEADER_MAGIC 0xed26ff3a
#define CHUNK_TYPE_RAW 0xcac1
#define CHUNK_TYPE_DONT_CARE 0xcac3

typedef struct sparse_header {
  uint32_t magic;
  uint16_t major_version;
  uint16_t minor_version;
  uint16_t file_hdr_sz;
  uint16_t chunk_hdr_sz;
  uint32_t blk_sz;
  uint32_t total_blks;
  uint32_t total_chunks;
  uint32_t image_checksum;
} sparse_header_t;

typedef struct chunk_header {
  uint16_t chunk_type;
  uint16_t reserved1;
  uint32_t chunk_sz;
  uint32_t total_sz;
} chunk_header_t;

typedef struct split_header {
  sparse_header_t sparse;
  chunk_header_t dont_care;
  chunk_header_t raw;
} split_header_t;

_Static_assert(sizeof(split_header_t) == 52, "unexpected split header size");

static char *copy_buf;

static void error_exit(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void usage(const char *argv0, const char *error_fmt, ...) {
  const char *prog_name = strrchr(argv0, '/');
  va_list ap;

  prog_name = prog_name ? prog_name + 1 : argv0;

  fprintf(stderr, "Usage: %s [OPTIONS] <raw_image_file>\n", prog_name);
  fputs("The <raw_image_file> will be split into as many sparse\n"
        "files as needed.  Each sparse file will contain a single\n"
        "DONT CARE chunk to offset to the correct block and then\n"
        "a single RAW chunk containing a portion of the data from\n"
        "the raw image file.  The sparse files will be named by\n"
        "appending a number to the name of the raw image file.\n"
        "\n"
        "OPTIONS (Defaults are enclosed by square brackets):\n",
        stderr);
  fprintf(stderr, "  -s SUFFIX      Format appended number with SUFFIX [%s]\n", "%03d");
  fprintf(stderr, "  -B SIZE        Use a block size of SIZE [%s]\n", "4K");
  fprintf(stderr, "  -C SIZE        Use a chunk size of SIZE [%s]\n", "64M");
  fputs("SIZE is a decimal integer that may optionally be\n"
        "followed by a suffix that specifies a multiplier for\n"
        "the integer:\n"
        "       c         1 byte (the default when omitted)\n"
        "       w         2 bytes\n"
        "       b         512 bytes\n"
        "       kB        1000 bytes\n"
        "       K         1024 bytes\n"
        "       MB        1000*1000 bytes\n"
        "       M         1024*1024 bytes\n"
        "       GB        1000*1000*1000 bytes\n"
        "       G         1024*1024*1024 bytes\n",
        stderr);

  if (error_fmt && *error_fmt) {
    fputc('\n', stderr);
    va_start(ap, error_fmt);
    vfprintf(stderr, error_fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
  }
  exit(1);
}

static int parse_size(const char *size_str, uint64_t *out) {
  char *end;
  uint64_t size;
  uint64_t mult;

  errno = 0;
  size = strtoull(size_str, &end, 10);
  if (errno || end == size_str) {
    return -1;
  }

  if (*end == '\0') {
    mult = 1;
  } else if (strcmp(end, "c") == 0) {
    mult = 1;
  } else if (strcmp(end, "w") == 0) {
    mult = 2;
  } else if (strcmp(end, "b") == 0) {
    mult = 512;
  } else if (strcmp(end, "kB") == 0) {
    mult = 1000;
  } else if (strcmp(end, "K") == 0) {
    mult = 1024;
  } else if (strcmp(end, "MB") == 0) {
    mult = 1000ULL * 1000;
  } else if (strcmp(end, "M") == 0) {
    mult = 1024ULL * 1024;
  } else if (strcmp(end, "GB") == 0) {
    mult = 1000ULL * 1000 * 1000;
  } else if (strcmp(end, "G") == 0) {
    mult = 1024ULL * 1024 * 1024;
  } else {
    return -1;
  }

  if (size > UINT64_MAX / mult) {
    return -1;
  }

  *out = size * mult;
  return 0;
}

int main(int argc, char **argv) {
  const char *out_fmt = "%03d";
  const char *block_size_str = "4K";
  const char *chunk_size_str = "64M";
  const char *in_path;
  char *format;
  char *out_path;
  uint64_t block_size;
  uint64_t chunk_size;
  uint64_t blocks_per_chunk;
  uint64_t remaining;
  uint64_t current_size;
  struct stat in_st;
  split_header_t hdr;
  int in_fd;
  int out_fd;
  int opt;
  int chunk_index;

  while ((opt = getopt(argc, argv, "s:B:C:")) != -1) {
    switch (opt) {
      case 's':
        out_fmt = optarg;
        break;
      case 'B':
        block_size_str = optarg;
        break;
      case 'C':
        chunk_size_str = optarg;
        break;
      default:
        usage(argv[0], NULL);
    }
  }

  if (parse_size(block_size_str, &block_size) != 0) {
    usage(argv[0], "Can not parse \"%s\" as a block size.", block_size_str);
  }
  if (block_size & 0xfff) {
    usage(argv[0], "Block size is not a multiple of 4096.");
  }
  if (parse_size(chunk_size_str, &chunk_size) != 0) {
    usage(argv[0], "Can not parse \"%s\" as a chunk size.", chunk_size_str);
  }

  blocks_per_chunk = chunk_size / block_size;
  if (chunk_size % block_size) {
    usage(argv[0], "Chunk size is not a multiple of the block size.");
  }

  if (argc - optind != 1) {
    usage(argv[0], "Missing or extra arguments.");
  }

  in_path = argv[optind];
  in_fd = open(in_path, O_RDONLY);
  if (in_fd < 0) {
    error_exit("open \"%s\": %s", in_path, strerror(errno));
  }
  if (fstat(in_fd, &in_st) != 0) {
    error_exit("fstat \"%s\": %s", in_path, strerror(errno));
  }
  remaining = in_st.st_size;
  if (remaining % block_size) {
    error_exit("\"%s\" size (%llu) is not a multiple of the block size (%llu).\n",
               in_path, (unsigned long long)remaining,
               (unsigned long long)block_size);
  }

  copy_buf = malloc(COPY_BUF_SIZE);
  if (!copy_buf) {
    error_exit("malloc copy buffer: %s", strerror(errno));
  }

  format = malloc(strlen(out_fmt) + 3);
  if (!format) {
    error_exit("malloc format buffer: %s", strerror(errno));
  }
  format[0] = '%';
  format[1] = 's';
  strcpy(format + 2, out_fmt);

  int len = snprintf(copy_buf, COPY_BUF_SIZE, format, in_path, -1);
  if (len >= COPY_BUF_SIZE) {
    error_exit("Ridulously long suffix: %s", out_fmt);
  }
  out_path = malloc(len + 1);
  if (!out_path) {
    error_exit("malloc output path buffer: %s", strerror(errno));
  }

  hdr.sparse.magic = SPARSE_HEADER_MAGIC;
  hdr.sparse.major_version = 1;
  hdr.sparse.minor_version = 0;
  hdr.sparse.file_hdr_sz = 28;
  hdr.sparse.chunk_hdr_sz = 12;
  hdr.sparse.blk_sz = (uint32_t)block_size;
  hdr.sparse.total_blks = 0;
  hdr.sparse.total_chunks = 2;
  hdr.sparse.image_checksum = 0;

  hdr.dont_care.chunk_type = CHUNK_TYPE_DONT_CARE;
  hdr.dont_care.reserved1 = 0;
  hdr.dont_care.chunk_sz = 0;
  hdr.dont_care.total_sz = 12;

  hdr.raw.chunk_type = CHUNK_TYPE_RAW;
  hdr.raw.reserved1 = 0;
  hdr.raw.chunk_sz = (uint32_t)blocks_per_chunk;
  hdr.raw.total_sz = (uint32_t)(chunk_size + 12);

  if (remaining == 0) {
    if (close(in_fd) != 0) {
      error_exit("close \"%s\": %s", in_path, strerror(errno));
    }
    exit(0);
  }

  chunk_index = 1;
  current_size = chunk_size;
  for (;;) {
    if (remaining < chunk_size) {
      current_size = remaining;
      hdr.raw.chunk_sz = (uint32_t)(remaining / block_size);
      hdr.raw.total_sz = (uint32_t)(remaining + 12);
    }

    uint32_t prev_blocks =
        (uint32_t)blocks_per_chunk * (uint32_t)(chunk_index - 1);
    hdr.dont_care.chunk_sz = prev_blocks;
    hdr.sparse.total_blks =
        (uint32_t)(current_size / block_size) + prev_blocks;

    sprintf(out_path, format, in_path, chunk_index);

    out_fd = creat(out_path, 0666);
    if (out_fd < 0) {
      error_exit("\"%s\": %s", out_path, strerror(errno));
    }

    ssize_t written = write(out_fd, &hdr, sizeof(hdr));
    if (written < 0) {
      error_exit("\"%s\": %s", out_path, strerror(errno));
    }
    if (written != sizeof(hdr)) {
      error_exit("\"%s\": Short write (%lu)", out_path,
                 (unsigned long)written);
    }

    uint64_t left_to_write = current_size;
    while (left_to_write) {
      size_t to_read = left_to_write < COPY_BUF_SIZE
                           ? (size_t)left_to_write
                           : COPY_BUF_SIZE;
      ssize_t got = read(in_fd, copy_buf, to_read);
      if (got < 0) {
        error_exit("\"%s\": %s", in_path, strerror(errno));
      }
      if (got == 0) {
        error_exit("\"%s\": Unexpected EOF", in_path);
      }
      ssize_t put = write(out_fd, copy_buf, got);
      if (put < 0) {
        error_exit("\"%s\": %s", out_path, strerror(errno));
      }
      if (put != got) {
        error_exit("\"%s\": Short data write (%lu)", out_path,
                   (unsigned long)put);
      }
      left_to_write -= got;
    }

    if (close(out_fd) != 0) {
      error_exit("close \"%s\": %s", out_path, strerror(errno));
    }

    chunk_index++;
    remaining -= current_size;
    if (remaining == 0) {
      break;
    }
    current_size = chunk_size;
  }

  if (close(in_fd) != 0) {
    error_exit("close \"%s\": %s", in_path, strerror(errno));
  }

  exit(0);
}
