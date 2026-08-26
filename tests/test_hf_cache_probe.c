#include "ds4_hf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const sha =
    "0123456789abcdef0123456789abcdef01234567";

static void set_artifact(ds4_hf_manifest_artifact *artifact,
                         const char *path, uint64_t bytes, const char *digest) {
    memset(artifact, 0, sizeof(*artifact));
    snprintf(artifact->path, sizeof(artifact->path), "%s", path);
    artifact->bytes = bytes;
    snprintf(artifact->sha256, sizeof(artifact->sha256), "%s", digest);
}

static void set_variant(ds4_hf_manifest_variant *variant,
                        const char *selector, const char *directory,
                        char prefix) {
    char path[DS4_HF_PATH_MAX];
    memset(variant, 0, sizeof(*variant));
    snprintf(variant->selector, sizeof(variant->selector), "%s", selector);
    snprintf(variant->directory, sizeof(variant->directory), "%s", directory);

    snprintf(path, sizeof(path), "%s/%c-receiver.gguf", directory, prefix);
    set_artifact(&variant->receiver, path, 19,
                 prefix == 'H' ?
                 "b606599a2a7ef50bc2e1f9a9573b116d1d3dff5f0992d907daad405f0ae98e84" :
                 "eb78f6cd9cf0e345f36a921343b7f61bf414cc559481b9397fa6492003234b5c");
    snprintf(path, sizeof(path), "%s/%c-tower.safetensors", directory, prefix);
    set_artifact(&variant->ds4_vision.tower, path, 17,
                 prefix == 'H' ?
                 "e64d12be0172a028c4c365af6e8fa40363d81d0fa44348a012ae100ea95e255e" :
                 "5d90699309c23f950654f0466c59ec770683a98248e17fdb31e18ee4361ae249");
    snprintf(path, sizeof(path), "%s/%c-projector.safetensors", directory, prefix);
    set_artifact(&variant->ds4_vision.projector, path, 21,
                 prefix == 'H' ?
                 "7c74da5ef8c62e140b8b62eae2ec4bb1b396c2b8a58e9c746a8d163722fe4ea0" :
                 "737c20349cd5cd24b63422d9d7c16b6ad994a2ca0c55834e253128975f9e6d7b");
    snprintf(path, sizeof(path), "%s/%c-config.json", directory, prefix);
    set_artifact(&variant->ds4_vision.config, path, 16,
                 prefix == 'H' ?
                 "f2ae753bb90816ccc812077dcea1b8bd8e9790078111b9a39cb4ad6b25e99c58" :
                 "be05806d001243c34b1e02ed9b54f06031a2105d01b1ff0580e5ba1b67a18b0f");
    snprintf(path, sizeof(path), "%s/mmproj-%c.gguf", directory, prefix);
    set_artifact(&variant->llama_cpp_mmproj, path, 18,
                 prefix == 'H' ?
                 "78b5d7f4ada22a1d22cfc73a9325b62ccedb753fc2e3ecf96898db2bab1f1cb3" :
                 "ffff977280449d926a0fdb7f9f0ef1a23677ec80ef193c29a0eff9cd515ee869");
    snprintf(path, sizeof(path), "%s/dspark-%c.gguf", directory, prefix);
    set_artifact(&variant->dspark, path, 20,
                 prefix == 'H' ?
                 "54fd7945bbf0e72474f62b3796b16a0d7b65da59e350a8d17671c290dc04a8c1" :
                 "9f11787f4d0a334faa76aaa7a87b0818536fd6825be2a16b734d29ec2922d205");
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
    if (has_mode(argv[4], "sha-multiblock")) {
        set_artifact(
            &manifest.variants[0].receiver,
            "Headroom128/H-sha256-multiblock.gguf", UINT64_C(131073),
            "357154df0673730b1e63e65bfc71f15c43d85bd1761a14e572eef18dbdb6e89e");
    }

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
    printf("integrity_seal=%s\n", plan.integrity_seal);
    if (has_mode(argv[4], "mutate-plan")) {
        plan.artifacts[0].bytes++;
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
    if (has_mode(argv[4], "verified-hold")) {
        int verified_fd = -1;
        memset(err, 0, sizeof(err));
        if (!ds4_hf_acquisition_open_verified(&plan, 0, &verified_fd,
                                              err, sizeof(err))) {
            printf("status=verified_open_error\n");
            printf("diagnostic=%s\n", err);
            return 3;
        }
        printf("verified_ready=receiver\n");
        fflush(stdout);
        if (getchar() == EOF) {
            close(verified_fd);
            return 64;
        }
        unsigned char first_byte = 0;
        if (pread(verified_fd, &first_byte, 1, 0) != 1) {
            close(verified_fd);
            return 3;
        }
        close(verified_fd);
        int replacement_fd = -1;
        memset(err, 0, sizeof(err));
        if (ds4_hf_acquisition_open_verified(&plan, 0, &replacement_fd,
                                             err, sizeof(err))) {
            close(replacement_fd);
            printf("status=replacement_not_rejected\n");
            return 3;
        }
        printf("held_first_byte=%u\n", (unsigned)first_byte);
        printf("replacement=rejected\n");
        printf("replacement_diagnostic=%s\n", err);
    }
    return 0;
}
