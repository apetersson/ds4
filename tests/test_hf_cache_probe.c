#include "ds4_hf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const sha =
    "0123456789abcdef0123456789abcdef01234567";

static void set_artifact(ds4_hf_manifest_artifact *artifact,
                         const char *path, uint64_t bytes, char digest) {
    memset(artifact, 0, sizeof(*artifact));
    snprintf(artifact->path, sizeof(artifact->path), "%s", path);
    artifact->bytes = bytes;
    memset(artifact->sha256, digest, DS4_HF_SHA256_HEX_SIZE - 1);
    artifact->sha256[DS4_HF_SHA256_HEX_SIZE - 1] = '\0';
}

static void set_variant(ds4_hf_manifest_variant *variant,
                        const char *selector, const char *directory,
                        char prefix) {
    char path[DS4_HF_PATH_MAX];
    memset(variant, 0, sizeof(*variant));
    snprintf(variant->selector, sizeof(variant->selector), "%s", selector);
    snprintf(variant->directory, sizeof(variant->directory), "%s", directory);

    snprintf(path, sizeof(path), "%s/%c-receiver.gguf", directory, prefix);
    set_artifact(&variant->receiver, path, 19, '1');
    snprintf(path, sizeof(path), "%s/%c-tower.safetensors", directory, prefix);
    set_artifact(&variant->ds4_vision.tower, path, 17, '2');
    snprintf(path, sizeof(path), "%s/%c-projector.safetensors", directory, prefix);
    set_artifact(&variant->ds4_vision.projector, path, 21, '3');
    snprintf(path, sizeof(path), "%s/%c-config.json", directory, prefix);
    set_artifact(&variant->ds4_vision.config, path, 16, '4');
    snprintf(path, sizeof(path), "%s/mmproj-%c.gguf", directory, prefix);
    set_artifact(&variant->llama_cpp_mmproj, path, 18, '5');
    snprintf(path, sizeof(path), "%s/dspark-%c.gguf", directory, prefix);
    set_artifact(&variant->dspark, path, 20, '6');
    variant->has_dspark = true;
}

static int has_mode(const char *modes, const char *wanted) {
    size_t wanted_len = strlen(wanted);
    for (const char *p = modes; p && *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == wanted_len && !memcmp(p, wanted, len)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "usage: test_hf_cache_probe CACHE|- ENDPOINT SELECTOR MODES TIMEOUT_MS\n");
        return 64;
    }
    char *end = NULL;
    errno = 0;
    long timeout_ms = strtol(argv[5], &end, 10);
    if (errno || !end || *end || timeout_ms <= 0) return 64;

    ds4_hf_cli_config cfg;
    ds4_hf_cli_init(&cfg);
    snprintf(cfg.repo, sizeof(cfg.repo), "%s", "owner/repo");
    snprintf(cfg.selector, sizeof(cfg.selector), "%s", argv[3]);
    cfg.selector_set = true;
    cfg.cache_dir = strcmp(argv[1], "-") ? argv[1] : NULL;
    cfg.endpoint = argv[2];
    cfg.offline = has_mode(argv[4], "offline");
    cfg.vision_source = has_mode(argv[4], "vision") ?
        DS4_HF_VISION_CATALOG : DS4_HF_VISION_NONE;
    cfg.dspark_source = has_mode(argv[4], "dspark") ?
        DS4_HF_DSPARK_CATALOG : DS4_HF_DSPARK_NONE;

    ds4_hf_resolved_repo resolved = {0};
    snprintf(resolved.endpoint, sizeof(resolved.endpoint), "%s", argv[2]);
    snprintf(resolved.repo, sizeof(resolved.repo), "%s", "owner/repo");
    snprintf(resolved.commit, sizeof(resolved.commit), "%s", sha);

    ds4_hf_manifest manifest = {0};
    manifest.schema_version = DS4_HF_MANIFEST_VERSION;
    snprintf(manifest.repository, sizeof(manifest.repository), "%s", "owner/repo");
    snprintf(manifest.default_selector, sizeof(manifest.default_selector),
             "%s", "Headroom128");
    manifest.variant_count = 2;
    set_variant(&manifest.variants[0], "Headroom128", "Headroom128", 'H');
    manifest.variants[0].is_default = true;
    set_variant(&manifest.variants[1], "Quality128", "Quality128", 'Q');

    ds4_hf_acquisition_plan plan;
    char err[2048] = {0};
    if (!ds4_hf_acquisition_plan_build(
            &cfg, &resolved, &manifest, has_mode(argv[4], "materialize"),
            &plan, err, sizeof(err))) {
        printf("status=plan_error\n");
        printf("diagnostic=%s\n", err);
        return 2;
    }
    printf("cache_root=%s\n", plan.cache_root);
    for (size_t i = 0; i < plan.artifact_count; i++) {
        printf("plan=%s|%s|%d|%" PRIu64 "|%s\n",
               ds4_hf_artifact_role_name(plan.artifacts[i].role),
               plan.artifacts[i].repo_path, plan.artifacts[i].requested,
               plan.artifacts[i].bytes, plan.artifacts[i].destination);
    }
    if (!ds4_hf_acquisition_execute(&cfg, &plan, timeout_ms,
                                    err, sizeof(err))) {
        printf("status=acquire_error\n");
        printf("diagnostic=%s\n", err);
        return 2;
    }
    printf("status=ok\n");
    for (size_t i = 0; i < plan.artifact_count; i++) {
        if (plan.artifacts[i].requested) {
            printf("result=%s|%d\n",
                   ds4_hf_artifact_role_name(plan.artifacts[i].role),
                   plan.artifacts[i].cache_hit);
        }
    }
    return 0;
}
