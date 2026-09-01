#include "ds4_hf.h"

#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

int main(int argc, char **argv) {
    bool inject_mmproj = argc == 4 &&
                         !strcmp(argv[3], "populate-invalid-mmproj");
    bool corrupt_dspark = argc == 4 &&
                          !strcmp(argv[3], "corrupt-dspark");
    if (argc != 4 || (strcmp(argv[3], "populate-server-dspark") &&
                      !inject_mmproj && !corrupt_dspark)) {
        fprintf(stderr,
                "usage: %s ENDPOINT CACHE_DIR "
                "{populate-server-dspark|populate-invalid-mmproj|corrupt-dspark}\n",
                argv[0]);
        return 64;
    }
    ds4_hf_cli_config cfg;
    ds4_hf_cli_init(&cfg);
    snprintf(cfg.repo, sizeof(cfg.repo), "%s", "owner/repo");
    snprintf(cfg.selector, sizeof(cfg.selector), "%s",
             "Headroom128-IQ2_XXS");
    cfg.selector_set = true;
    cfg.endpoint = argv[1];
    cfg.cache_dir = argv[2];
    cfg.receiver_source = DS4_HF_RECEIVER_REPOSITORY;
    cfg.vision_source = DS4_HF_VISION_CATALOG;
    cfg.dspark_source = DS4_HF_DSPARK_CATALOG;
    cfg.dry_run = true;

    ds4_hf_diagnostics diagnostics;
    char err[2048] = {0};
    if (!ds4_hf_diagnostics_prepare(&cfg, &diagnostics,
                                    err, sizeof(err)) ||
        !ds4_hf_acquisition_execute(&cfg, &diagnostics.plan, 5000L,
                                    err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (inject_mmproj || corrupt_dspark) {
        ds4_hf_artifact_role role = inject_mmproj
                                        ? DS4_HF_ROLE_LLAMA_CPP_MMPROJ
                                        : DS4_HF_ROLE_DSPARK;
        ds4_hf_acquisition_artifact *target = NULL;
        for (size_t i = 0; i < diagnostics.plan.artifact_count; i++) {
            if (diagnostics.plan.artifacts[i].role == role) {
                target = &diagnostics.plan.artifacts[i];
                break;
            }
        }
        if (!target || (!inject_mmproj && chmod(target->destination, 0600))) {
            perror("prepare invalid cache entry");
            return 1;
        }
        FILE *fp = fopen(target->destination, "wb");
        if (!fp) {
            perror("open invalid cache entry");
            return 1;
        }
        bool wrote = fputs("invalid", fp) != EOF;
        if (fclose(fp) || !wrote) {
            perror("write invalid cache entry");
            return 1;
        }
    }
    puts("populated");
    return 0;
}
