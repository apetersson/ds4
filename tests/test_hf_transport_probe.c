#include "ds4_hf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: test_hf_transport_probe OWNER/REPO REVISION|- TIMEOUT_MS\n");
        return 64;
    }

    char *end = NULL;
    errno = 0;
    long timeout_ms = strtol(argv[3], &end, 10);
    if (errno || !end || *end || timeout_ms <= 0 ||
        strlen(argv[1]) >= DS4_HF_REPO_MAX) {
        fprintf(stderr, "invalid probe arguments\n");
        return 64;
    }

    ds4_hf_cli_config cfg;
    ds4_hf_cli_init(&cfg);
    memcpy(cfg.repo, argv[1], strlen(argv[1]) + 1);
    cfg.revision = strcmp(argv[2], "-") ? argv[2] : NULL;

    ds4_hf_resolved_repo resolved;
    char err[512] = {0};
    ds4_hf_resolve_status status =
        ds4_hf_resolve_repository(&cfg, timeout_ms, &resolved, err, sizeof(err));
    if (status != DS4_HF_RESOLVE_OK) {
        printf("status=%s\n", ds4_hf_resolve_status_name(status));
        printf("diagnostic=%s\n", err);
        return 2;
    }

    char manifest_url[DS4_HF_URL_MAX];
    char receiver_url[DS4_HF_URL_MAX];
    if (!ds4_hf_resolved_file_url(&resolved, "variants.json",
                                  manifest_url, sizeof(manifest_url),
                                  err, sizeof(err)) ||
        !ds4_hf_resolved_file_url(&resolved, "nested/model.gguf",
                                  receiver_url, sizeof(receiver_url),
                                  err, sizeof(err))) {
        printf("status=url_error\n");
        printf("diagnostic=%s\n", err);
        return 2;
    }

    printf("status=ok\n");
    printf("commit=%s\n", resolved.commit);
    printf("manifest_url=%s\n", manifest_url);
    printf("receiver_url=%s\n", receiver_url);
    return 0;
}
