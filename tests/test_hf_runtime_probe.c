#include "ds4_hf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const ds4_hf_acquisition_artifact *find_artifact(
    const ds4_hf_runtime *runtime, ds4_hf_artifact_role role) {
    for (size_t i = 0; i < runtime->plan.artifact_count; i++) {
        if (runtime->plan.artifacts[i].role == role) {
            return &runtime->plan.artifacts[i];
        }
    }
    return NULL;
}

static bool files_equal(const char *left, const char *right) {
    int a = open(left, O_RDONLY);
    int b = open(right, O_RDONLY);
    if (a < 0 || b < 0) {
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        return false;
    }
    bool equal = true;
    for (;;) {
        unsigned char av[4096], bv[4096];
        ssize_t an = read(a, av, sizeof(av));
        ssize_t bn = read(b, bv, sizeof(bv));
        if (an < 0 && errno == EINTR) continue;
        if (bn < 0 && errno == EINTR) continue;
        if (an < 0 || bn < 0 || an != bn ||
            (an > 0 && memcmp(av, bv, (size_t)an))) {
            equal = false;
            break;
        }
        if (an == 0) break;
    }
    close(a);
    close(b);
    return equal;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[3], "cli") && strcmp(argv[3], "server") &&
                      strcmp(argv[3], "no-vision") && strcmp(argv[3], "explicit"))) {
        fprintf(stderr, "usage: %s ENDPOINT CACHE_DIR cli|server|no-vision|explicit\n", argv[0]);
        return 2;
    }
    ds4_hf_cli_config cfg;
    ds4_hf_cli_init(&cfg);
    snprintf(cfg.repo, sizeof(cfg.repo), "%s", "owner/repo");
    snprintf(cfg.selector, sizeof(cfg.selector), "%s", "Headroom128-IQ2_XXS");
    cfg.selector_set = true;
    cfg.endpoint = argv[1];
    cfg.cache_dir = argv[2];
    cfg.receiver_source = DS4_HF_RECEIVER_REPOSITORY;
    if (!strcmp(argv[3], "server")) {
        cfg.vision_source = DS4_HF_VISION_CATALOG;
    } else if (!strcmp(argv[3], "no-vision")) {
        cfg.vision_source = DS4_HF_VISION_DISABLED;
    } else if (!strcmp(argv[3], "explicit")) {
        cfg.vision_source = DS4_HF_VISION_EXPLICIT;
        cfg.vision_python = "/trusted/python";
        cfg.vision_encoder = "/trusted/encoder.py";
        cfg.vision_tower = "/trusted/tower.safetensors";
        cfg.vision_adapter = "/trusted/projector.safetensors";
    } else {
        cfg.vision_source = DS4_HF_VISION_NONE;
    }

    ds4_hf_runtime runtime;
    char err[1024] = {0};
    if (!ds4_hf_runtime_prepare(&cfg, &runtime, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    const ds4_hf_acquisition_artifact *receiver =
        find_artifact(&runtime, DS4_HF_ROLE_RECEIVER);
    const char *open_path = ds4_hf_runtime_open_path(
        &runtime, DS4_HF_ROLE_RECEIVER);
    bool receiver_equal = receiver && open_path &&
                          files_equal(receiver->destination, open_path);

    printf("repository=%s\n", runtime.plan.repository);
    printf("revision=%s\n", runtime.plan.revision);
    printf("selector=%s\n", runtime.plan.selector);
    printf("receiver=%s\n", receiver ? receiver->repo_path : "");
    printf("receiver_equal=%s\n", receiver_equal ? "true" : "false");
    printf("vision_verified=%s\n",
           runtime.vision_bundle_verified ? "true" : "false");
    char compat_err[512] = {0};
    bool compat = runtime.vision_bundle_verified &&
        ds4_hf_runtime_vision_compatible(&runtime, 129279,
                                         compat_err, sizeof(compat_err));
    printf("vision_compatible=%s\n", compat ? "true" : "false");
    if (runtime.vision_bundle_verified && !compat) {
        fprintf(stderr, "%s\n", compat_err);
        ds4_hf_runtime_close_verified(&runtime);
        return 1;
    }
    memset(compat_err, 0, sizeof(compat_err));
    bool mismatch_rejected = runtime.vision_bundle_verified &&
        !ds4_hf_runtime_vision_compatible(&runtime, 7,
                                          compat_err, sizeof(compat_err)) &&
        strstr(compat_err, "receiver image token id 7") != NULL;
    printf("vision_mismatch_rejected=%s\n",
           mismatch_rejected ? "true" : "false");
    printf("image_token_id=%u\n", runtime.vision_metadata.image_token_id);
    printf("verified_roles=");
    bool first = true;
    for (size_t i = 0; i < runtime.plan.artifact_count; i++) {
        ds4_hf_artifact_role role = runtime.plan.artifacts[i].role;
        if (!ds4_hf_runtime_role_verified(&runtime, role)) continue;
        printf("%s%s", first ? "" : ",", ds4_hf_artifact_role_name(role));
        first = false;
    }
    printf("\nREADY\n");
    fflush(stdout);
    usleep(500000);
    printf("DONE\n");
    ds4_hf_runtime_close_verified(&runtime);
    return receiver_equal ? 0 : 1;
}
