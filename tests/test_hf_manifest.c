#include "ds4_hf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(name, condition) do { \
    if (condition) printf("ok - %s\n", name); \
    else { fprintf(stderr, "not ok - %s\n", name); failures++; } \
} while (0)

static char *read_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    rewind(file);
    char *contents = malloc((size_t)length + 1);
    if (!contents) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(contents, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(contents);
        return NULL;
    }
    contents[read] = '\0';
    *size_out = read;
    return contents;
}

static char *replace_once(const char *source, const char *needle,
                          const char *replacement, size_t *size_out) {
    const char *at = strstr(source, needle);
    if (!at) return NULL;
    size_t prefix = (size_t)(at - source);
    size_t needle_len = strlen(needle);
    size_t replacement_len = strlen(replacement);
    size_t source_len = strlen(source);
    size_t result_len = source_len - needle_len + replacement_len;
    char *result = malloc(result_len + 1);
    if (!result) return NULL;
    memcpy(result, source, prefix);
    memcpy(result + prefix, replacement, replacement_len);
    memcpy(result + prefix + replacement_len, at + needle_len,
           source_len - prefix - needle_len + 1);
    *size_out = result_len;
    return result;
}

static void expect_rejected(const char *name, const char *fixture,
                            const char *needle, const char *replacement,
                            const char *expected_error) {
    size_t size = 0;
    char *mutated = replace_once(fixture, needle, replacement, &size);
    ds4_hf_manifest manifest;
    char err[512] = {0};
    bool ok = mutated && ds4_hf_manifest_parse(mutated, size, &manifest,
                                                err, sizeof(err));
    CHECK(name, mutated && !ok && strstr(err, expected_error));
    if (mutated && (ok || !strstr(err, expected_error))) {
        fprintf(stderr, "  diagnostic: %s\n", err);
    }
    free(mutated);
}

static void test_production_fixture(const char *json, size_t json_len) {
    ds4_hf_manifest manifest;
    char err[512] = {0};
    bool ok = ds4_hf_manifest_parse(json, json_len, &manifest, err, sizeof(err));
    CHECK("production-shaped variants v2 fixture parses", ok);
    if (!ok) {
        fprintf(stderr, "  diagnostic: %s\n", err);
        return;
    }
    CHECK("repository, version, default, and two variants are retained",
          manifest.schema_version == 2 && manifest.variant_count == 2 &&
          !strcmp(manifest.repository,
                  "apetersson/DeepSeek-V4-Flash-0731-Abliterated-Vision") &&
          !strcmp(manifest.default_selector, "Headroom128-IQ2_XXS"));

    const ds4_hf_manifest_variant *headroom =
        ds4_hf_manifest_find_variant(&manifest, "headroom128-iq2_xxs");
    const ds4_hf_manifest_variant *quality =
        ds4_hf_manifest_find_variant(&manifest, "QUALITY128-IQ2_XXS_XL");
    CHECK("Headroom128 typed receiver/vision/mmproj/DSpark records are retained",
          headroom && headroom->is_default && headroom->has_dspark &&
          headroom->receiver.bytes == UINT64_C(143327232000) &&
          headroom->ds4_vision.tower.supports_ds4 &&
          !headroom->ds4_vision.tower.supports_llama_cpp &&
          headroom->llama_cpp_mmproj.supports_llama_cpp &&
          !strcmp(headroom->llama_cpp_mmproj.gguf_projector_type,
                  "deepencoder_v2_dsv4") &&
          (headroom->receiver.optional_capabilities & DS4_HF_CAP_SSD_STREAMING));
    CHECK("Quality128 typed records remain independently selectable",
          quality && !quality->is_default && quality->has_dspark &&
          !strcmp(quality->receiver.profile, "Quality128") &&
          !strcmp(quality->dspark.precision, "Q8_0"));

    CHECK("shared DeepEncoderV2 metadata is exact",
          manifest.shared_vision.image_token_id == 129279 &&
          !strcmp(manifest.shared_vision.image_token, "<｜image｜>") &&
          manifest.shared_vision.image_size == 1024 &&
          manifest.shared_vision.encoder_dim == 896 &&
          manifest.shared_vision.receiver_dim == 4096 &&
          manifest.shared_vision.tokens_per_view == 256 &&
          manifest.shared_vision.separator_tokens == 1 &&
          !strcmp(manifest.shared_vision.token_formula, "views*256+1") &&
          manifest.shared_vision.global_view_first &&
          !strcmp(manifest.shared_vision.crop_order, "row-major"));
    CHECK("257, 769, 1025, and 1281 visual rows follow views*256+1",
          ds4_hf_manifest_visual_rows_valid(&manifest, 257) &&
          ds4_hf_manifest_visual_rows_valid(&manifest, 769) &&
          ds4_hf_manifest_visual_rows_valid(&manifest, 1025) &&
          ds4_hf_manifest_visual_rows_valid(&manifest, 1281));
    CHECK("non-formula and out-of-range visual rows are rejected",
          !ds4_hf_manifest_visual_rows_valid(&manifest, 256) &&
          !ds4_hf_manifest_visual_rows_valid(&manifest, 1024) &&
          !ds4_hf_manifest_visual_rows_valid(&manifest, 1537));
}

static void test_llama_discovery_without_manifest(void) {
    char err[512] = {0};
    CHECK("Headroom128 satisfies llama.cpp sibling discovery without manifest data",
          ds4_hf_llama_siblings_valid(
              "Headroom128-IQ2_XXS/DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
              "Headroom128-IQ2_XXS/mmproj-DeepSeek-V4-Flash-0731-DeepEncoderV2-BF16.gguf",
              "Headroom128-IQ2_XXS/dspark-DeepSeek-V4-Flash-0731-Headroom128-Q8_0.gguf",
              err, sizeof(err)));
    memset(err, 0, sizeof(err));
    CHECK("Quality128 satisfies llama.cpp sibling discovery without manifest data",
          ds4_hf_llama_siblings_valid(
              "Quality128-IQ2_XXS_XL/DeepSeek-V4-Flash-0731-Abliterated-Vision-Quality128-IQ2_XXS_XL.gguf",
              "Quality128-IQ2_XXS_XL/mmproj-DeepSeek-V4-Flash-0731-DeepEncoderV2-BF16.gguf",
              "Quality128-IQ2_XXS_XL/dspark-DeepSeek-V4-Flash-0731-Quality128-Q8_0.gguf",
              err, sizeof(err)));
    CHECK("nested repository directories remain valid sibling locations",
          ds4_hf_llama_siblings_valid(
              "catalog/Headroom128/main.gguf",
              "catalog/Headroom128/mmproj-vision.gguf",
              "catalog/Headroom128/dspark-support.gguf",
              err, sizeof(err)));
    CHECK("llama.cpp discovery rejects cross-directory mmproj decoy",
          !ds4_hf_llama_siblings_valid(
              "Headroom128-IQ2_XXS/main.gguf",
              "Quality128-IQ2_XXS_XL/mmproj-decoy.gguf", NULL,
              err, sizeof(err)));
    CHECK("llama.cpp discovery requires lowercase companion prefixes",
          !ds4_hf_llama_siblings_valid(
              "Headroom128-IQ2_XXS/main.gguf",
              "Headroom128-IQ2_XXS/MMProj-decoy.gguf", NULL,
              err, sizeof(err)));
}

static void test_schema_rejections(const char *json) {
    expect_rejected("unsupported major version fails actionably", json,
                    "\"schema_version\": 2", "\"schema_version\": 3",
                    "unsupported variants.json major version 3");
    expect_rejected("unknown required capability fails actionably", json,
                    "\"required\": [\"deepseek4\", \"text-generation\"]",
                    "\"required\": [\"deepseek4\", \"unknown-required\"]",
                    "unknown required capability 'unknown-required'");
    expect_rejected("duplicate selectors are case-insensitively rejected", json,
                    "\"selector\": \"Quality128-IQ2_XXS_XL\"",
                    "\"selector\": \"headroom128-iq2_xxs\"",
                    "duplicate selector");
    expect_rejected("unsafe traversal paths are rejected", json,
                    "Headroom128-IQ2_XXS/DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
                    "../escape.gguf", "unsafe artifact path");
    expect_rejected("artifact paths cannot inject command-line options", json,
                    "Headroom128-IQ2_XXS/DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
                    "Headroom128-IQ2_XXS/--checkpoint.gguf", "unsafe artifact path");
    expect_rejected("escaped NUL cannot truncate validated artifact paths", json,
                    "Headroom128-IQ2_XXS/DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
                    "Headroom128-IQ2_XXS/main.gguf\\u0000../escape",
                    "escaped control character");
    expect_rejected("malformed hashes are rejected", json,
                    "1111111111111111111111111111111111111111111111111111111111111111",
                    "not-a-sha256", "malformed SHA-256");
    expect_rejected("zero artifact sizes are rejected", json,
                    "\"bytes\": 143327232000", "\"bytes\": 0",
                    "invalid zero byte count");
    expect_rejected("fractional artifact sizes are rejected", json,
                    "\"bytes\": 143327232000", "\"bytes\": 1.5",
                    "expected integer");
    expect_rejected("conflicting defaults are rejected", json,
                    "\"default\": false", "\"default\": true",
                    "default conflicts");
    expect_rejected("incomplete DS4 companion bundles are rejected", json,
                    "\"config\": {", "\"future_config\": {",
                    "incomplete DS4 vision bundle");
    expect_rejected("missing llama.cpp companion bundles are rejected", json,
                    "\"llama_cpp_mmproj\": {", "\"future_mmproj\": {",
                    "variant is missing");
    expect_rejected("mismatched runtime declarations are rejected", json,
                    "\"llama_cpp\": {\"minimum_revision\": \"deepseek4-main-v1\"}",
                    "\"future_runtime\": {\"minimum_revision\": \"deepseek4-main-v1\"}",
                    "runtime constraints inconsistent");
    expect_rejected("runtime revisions cannot contain shell fragments", json,
                    "\"minimum_revision\": \"52bba7525122fe8a6cac57c1d0bf45a409bed5b1\"",
                    "\"minimum_revision\": \"$(run-repository-code)\"",
                    "minimum_revision is missing or unsafe");
    expect_rejected("mismatched role capabilities are rejected", json,
                    "\"optional\": [\"ssd-streaming\"]",
                    "\"optional\": [\"ds4-vision\"]",
                    "capabilities inconsistent");
    expect_rejected("incompatible receiver GGUF metadata is rejected", json,
                    "\"gguf_metadata\": {\"architecture\": \"deepseek4\"}",
                    "\"gguf_metadata\": {\"architecture\": \"clip\"}",
                    "incompatible GGUF metadata");
}

static void test_non_executable_boundary(const char *json) {
    const char *denied[] = {
        "argv", "args", "env", "environment", "executable", "interpreter", "shell",
        "command", "auto_map", "trust_remote_code",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        char name[160];
        snprintf(name, sizeof(name), "manifest cannot inject %s", denied[i]);
        expect_rejected(name, json, "future_optional_top_level", denied[i],
                        "executable configuration and is forbidden");
    }
}

static void test_resource_limits(void) {
    ds4_hf_manifest manifest;
    char err[512] = {0};
    size_t oversize = DS4_HF_MANIFEST_MAX_BYTES + 1u;
    char *large = malloc(oversize);
    if (large) memset(large, ' ', oversize);
    CHECK("manifest byte limit is enforced before parsing",
          large && !ds4_hf_manifest_parse(large, oversize, &manifest,
                                           err, sizeof(err)) &&
          strstr(err, "byte parser limit"));
    free(large);

    char nested[256];
    size_t at = 0;
    at += (size_t)snprintf(nested + at, sizeof(nested) - at, "{\"unknown\":");
    for (unsigned i = 0; i < DS4_HF_MANIFEST_MAX_DEPTH + 1; i++) nested[at++] = '[';
    nested[at++] = '0';
    for (unsigned i = 0; i < DS4_HF_MANIFEST_MAX_DEPTH + 1; i++) nested[at++] = ']';
    nested[at++] = '}';
    nested[at] = '\0';
    memset(err, 0, sizeof(err));
    CHECK("manifest nesting limit is enforced",
          !ds4_hf_manifest_parse(nested, at, &manifest, err, sizeof(err)) &&
          strstr(err, "nesting limit"));
}

int main(void) {
    size_t json_len = 0;
    char *json = read_file("tests/fixtures/hf/variants-v2.json", &json_len);
    CHECK("production fixture is readable", json != NULL);
    if (!json) return 1;

    test_production_fixture(json, json_len);
    test_llama_discovery_without_manifest();
    test_schema_rejections(json);
    test_non_executable_boundary(json);
    test_resource_limits();

    free(json);
    if (failures) {
        fprintf(stderr, "%d HF manifest test(s) failed\n", failures);
        return 1;
    }
    printf("all HF manifest tests passed\n");
    return 0;
}
