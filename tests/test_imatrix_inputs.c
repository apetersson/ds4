#include "../ds4.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failed;

#define CHECK(name, condition)                                               \
    do {                                                                     \
        if (condition) {                                                     \
            fprintf(stdout, "ok %s\n", name);                              \
        } else {                                                             \
            fprintf(stderr, "FAIL %s\n", name);                            \
            failed++;                                                        \
        }                                                                    \
    } while (0)

int main(void) {
    char path[] = "/tmp/ds4-imatrix-inputs.XXXXXX";
    const int fd = mkstemp(path);
    CHECK("temporary prompt created", fd >= 0);
    if (fd < 0) return 1;

    const char prompt[] = "describe the image";
    const ssize_t written = write(fd, prompt, sizeof(prompt) - 1u);
    CHECK("temporary prompt written",
          written == (ssize_t)(sizeof(prompt) - 1u));
    CHECK("temporary prompt closed", close(fd) == 0);

    size_t len = 0;
    CHECK("bounded prompt accepts file at exact limit",
          ds4_test_imatrix_read_text_file(
              path, sizeof(prompt) - 1u, &len) &&
          len == sizeof(prompt) - 1u);
    CHECK("bounded prompt rejects oversized file before allocation",
          !ds4_test_imatrix_read_text_file(path, 4u, &len) && len == 0);
    unlink(path);

    char valid[] = "case-1\t/tmp/prompt.txt\t/tmp/a.png,/tmp/b.jpg";
    char missing[] = "case-1\t/tmp/prompt.txt";
    char empty_image[] = "case-1\t/tmp/prompt.txt\t/tmp/a.png,,/tmp/b.jpg";
    char too_many[256] = "case-1\t/tmp/prompt.txt\t";
    for (int i = 0; i < 17; i++) {
        strcat(too_many, i == 0 ? "x" : ",x");
    }
    char err[128] = {0};
    size_t image_count = 0;
    CHECK("vision manifest parses multiple images",
          ds4_test_imatrix_parse_vision_manifest_line(
              valid, &image_count, err, sizeof(err)) && image_count == 2u);
    CHECK("vision manifest rejects missing fields",
          !ds4_test_imatrix_parse_vision_manifest_line(
              missing, &image_count, err, sizeof(err)));
    CHECK("vision manifest rejects empty image paths",
          !ds4_test_imatrix_parse_vision_manifest_line(
              empty_image, &image_count, err, sizeof(err)));
    CHECK("vision manifest rejects more than 16 images",
          !ds4_test_imatrix_parse_vision_manifest_line(
              too_many, &image_count, err, sizeof(err)));

    uint64_t counter = 41;
    CHECK("imatrix counter adds without narrowing",
          ds4_test_imatrix_counter_add(&counter, 1) && counter == 42);
    counter = UINT64_MAX;
    CHECK("imatrix counter detects overflow",
          !ds4_test_imatrix_counter_add(&counter, 1) &&
          counter == UINT64_MAX);

    if (failed) {
        fprintf(stderr, "%d imatrix input test(s) failed\n", failed);
        return 1;
    }
    puts("imatrix input tests PASS");
    return 0;
}
