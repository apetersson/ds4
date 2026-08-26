#include "ds4_hf.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[3], "populate-server-dspark")) {
        fprintf(stderr,
                "usage: %s ENDPOINT CACHE_DIR populate-server-dspark\n",
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
    puts("populated");
    return 0;
}
