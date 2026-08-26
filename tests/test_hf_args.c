#define _POSIX_C_SOURCE 200809L

#include "ds4_hf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(name, condition) do { \
    if (condition) printf("ok - %s\n", name); \
    else { fprintf(stderr, "not ok - %s\n", name); failures++; } \
} while (0)

static bool parse_common(ds4_hf_cli_config *cfg, bool server,
                         int argc, char **argv, bool model_explicit,
                         bool dspark_requested, char *err, size_t errlen) {
    ds4_hf_cli_init(cfg);
    for (int i = 1; i < argc; i++) {
        ds4_hf_cli_parse_result result =
            ds4_hf_cli_parse_arg(cfg, server, &i, argc, argv, err, errlen);
        if (result != DS4_HF_CLI_MATCHED) {
            if (result == DS4_HF_CLI_NO_MATCH && errlen) {
                snprintf(err, errlen, "unmatched option: %s", argv[i]);
            }
            return false;
        }
    }
    return ds4_hf_cli_validate(cfg, server, model_explicit, dspark_requested,
                               err, errlen);
}

static void test_alias(const char *alias, bool server) {
    char *argv[] = {"ds4", (char *)alias,
                    "ggml-org/GLM-4.7-Flash-GGUF:Q4_K_M"};
    ds4_hf_cli_config cfg;
    char err[256] = {0};
    bool ok = parse_common(&cfg, server, 3, argv, false, false,
                           err, sizeof(err));
    char name[128];
    snprintf(name, sizeof(name), "%s accepts %s", server ? "server" : "cli", alias);
    CHECK(name, ok && !strcmp(cfg.repo, "ggml-org/GLM-4.7-Flash-GGUF") &&
                !strcmp(cfg.selector, "Q4_K_M") &&
                cfg.receiver_source == DS4_HF_RECEIVER_REPOSITORY);
}

static void test_malformed(const char *value) {
    char *argv[] = {"ds4", "-hf", (char *)value};
    ds4_hf_cli_config cfg;
    char err[256] = {0};
    bool ok = parse_common(&cfg, false, 3, argv, false, false,
                           err, sizeof(err));
    char name[160];
    snprintf(name, sizeof(name), "reject malformed selector %s", value);
    CHECK(name, !ok && strstr(err, "invalid HF") != NULL);
}

int main(void) {
    const char *aliases[] = {"-hf", "-hfr", "--hf-repo", "--hf"};
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        test_alias(aliases[i], false);
        test_alias(aliases[i], true);
    }

    CHECK("selectors compare case-insensitively",
          ds4_hf_selector_equal("Headroom128", "headroom128") &&
          !ds4_hf_selector_equal("Headroom128", "Quality128"));

    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo",
                        "--vision-python", "/usr/bin/python3",
                        "--vision-encoder", "/opt/local/encoder.py"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 7, argv, false, false,
                               err, sizeof(err));
        CHECK("trusted local runtime uses exact catalog vision roles",
              ok && cfg.vision_source == DS4_HF_VISION_CATALOG &&
              !strcmp(cfg.vision_python, "/usr/bin/python3") &&
              !strcmp(cfg.vision_encoder, "/opt/local/encoder.py"));
    }
    {
        char *argv[] = {"ds4", "-hf", "owner/repo:Q4_K_M",
                        "-hff", "nested/model-Q8_0.gguf"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("exact file is retained as selector override",
              ok && cfg.selector_set &&
              !strcmp(cfg.file, "nested/model-Q8_0.gguf"));
    }

    setenv("HF_TOKEN", "environment-secret", 1);
    setenv("HF_ENDPOINT", "https://hf.example.test", 1);
    {
        char *argv[] = {"ds4", "--hf", "owner/repo"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("HF_TOKEN and HF_ENDPOINT are environment defaults",
              ok && !strcmp(cfg.token, "environment-secret") &&
              !strcmp(cfg.endpoint, "https://hf.example.test") &&
              !cfg.token_from_cli);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo",
                        "--hf-token", "cli-secret"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("explicit HF token overrides HF_TOKEN",
              ok && !strcmp(cfg.token, "cli-secret") && cfg.token_from_cli);
    }
    unsetenv("HF_TOKEN");
    unsetenv("HF_ENDPOINT");

    {
        char *argv[] = {"ds4", "--hf", "owner/repo",
                        "--hf-revision", "refs/pr/17",
                        "--hf-cache-dir", "/tmp/HF cache",
                        "--hf-offline"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 8, argv, false, false,
                               err, sizeof(err));
        CHECK("revision cache and offline controls are retained",
              ok && !strcmp(cfg.revision, "refs/pr/17") &&
              !strcmp(cfg.cache_dir, "/tmp/HF cache") && cfg.offline);
    }

    const char *bad_specs[] = {
        "repo", "/repo", "owner/", "owner/repo/extra", "owner/repo:",
        "owner/repo:a:b", "owner/repo:bad selector", "owner/repo:bad/selector",
    };
    for (size_t i = 0; i < sizeof(bad_specs) / sizeof(bad_specs[0]); i++) {
        test_malformed(bad_specs[i]);
    }

    {
        char *argv[] = {"ds4", "-hf", "owner/repo", "--hf", "other/repo"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("duplicate repository aliases are rejected",
              !ok && strstr(err, "duplicate option") != NULL);
    }
    {
        char *argv[] = {"ds4", "--offline", "--hf-offline"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("duplicate offline aliases are rejected",
              !ok && strstr(err, "duplicate option") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf-token", "top-secret",
                        "-hft", "must-not-appear"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("credential errors do not echo token values",
              !ok && !strstr(err, "top-secret") &&
              !strstr(err, "must-not-appear"));
    }
    {
        char *argv[] = {"ds4", "--hf"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 2, argv, false, false,
                               err, sizeof(err));
        CHECK("missing repository argument is rejected",
              !ok && strstr(err, "missing value") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf", "--offline"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("following option is not consumed as an HF value",
              !ok && strstr(err, "missing value for --hf") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, true, false,
                               err, sizeof(err));
        CHECK("explicit model conflicts with HF receiver",
              !ok && strstr(err, "mutually exclusive") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf-file", "model.gguf"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("exact file requires repository",
              !ok && strstr(err, "requires --hf-repo") != NULL);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo:Headroom128"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("server HF defaults to catalog vision",
              ok && cfg.vision_source == DS4_HF_VISION_CATALOG);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo:Headroom128",
                        "--no-vision"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 4, argv, false, false,
                               err, sizeof(err));
        CHECK("no-vision disables catalog vision planning",
              ok && cfg.vision_source == DS4_HF_VISION_DISABLED);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo",
                        "--vision-python", "/usr/bin/python3",
                        "--vision-encoder", "/opt/local/encoder.py",
                        "--vision-tower", "/models/tower.safetensors",
                        "--vision-adapter", "/models/adapter.safetensors"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 11, argv, false, false,
                               err, sizeof(err));
        CHECK("complete explicit vision bundle overrides catalog",
              ok && cfg.vision_source == DS4_HF_VISION_EXPLICIT);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo",
                        "--vision-tower", "/models/tower.safetensors"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("partial explicit vision bundle is rejected",
              !ok && strstr(err, "requires --vision-tower and --vision-adapter") != NULL);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo", "--no-vision",
                        "--vision-python", "/usr/bin/python3"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 6, argv, false, false,
                               err, sizeof(err));
        CHECK("no-vision rejects explicit vision mixing",
              !ok && strstr(err, "cannot be combined") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo", "--dspark"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 4, argv, false, false,
                               err, sizeof(err));
        CHECK("DSpark is catalog-planned only on explicit opt-in",
              ok && cfg.dspark_source == DS4_HF_DSPARK_CATALOG);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo", "--dspark",
                        "--mtp", "/models/support.gguf"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 6, argv, false, false,
                               err, sizeof(err));
        CHECK("explicit MTP takes precedence over catalog DSpark",
              ok && cfg.dspark_source == DS4_HF_DSPARK_EXPLICIT_MTP);
    }
    {
        char *argv[] = {"ds4", "--dspark"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 2, argv, false, false,
                               err, sizeof(err));
        CHECK("DSpark without explicit or catalog support is rejected",
              !ok && strstr(err, "requires either explicit --mtp") != NULL);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 3, argv, false, false,
                               err, sizeof(err));
        CHECK("text-only ds4 plans neither vision nor DSpark companions",
              ok && cfg.vision_source == DS4_HF_VISION_NONE &&
              cfg.dspark_source == DS4_HF_DSPARK_NONE);
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo",
                        "--list-hf-variants", "--json"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("variant listing accepts stable JSON diagnostics",
              ok && cfg.list_variants && !cfg.dry_run &&
              cfg.diagnostics_json);
    }
    {
        char *argv[] = {"ds4-server", "--hf", "owner/repo",
                        "--hf-dry-run", "--hf-json", "--dspark"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, true, 6, argv, false, false,
                               err, sizeof(err));
        CHECK("server dry run retains selected vision and DSpark roles",
              ok && cfg.dry_run && cfg.diagnostics_json &&
              cfg.vision_source == DS4_HF_VISION_CATALOG &&
              cfg.dspark_source == DS4_HF_DSPARK_CATALOG);
    }
    {
        char *argv[] = {"ds4", "--list-hf-variants"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 2, argv, false, false,
                               err, sizeof(err));
        CHECK("diagnostics require an HF repository",
              !ok && strstr(err, "require --hf-repo"));
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo",
                        "--list-hf-variants", "--hf-dry-run"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 5, argv, false, false,
                               err, sizeof(err));
        CHECK("listing and dry run are mutually exclusive",
              !ok && strstr(err, "mutually exclusive"));
    }
    {
        char *argv[] = {"ds4", "--hf", "owner/repo", "--json"};
        ds4_hf_cli_config cfg;
        char err[256] = {0};
        bool ok = parse_common(&cfg, false, 4, argv, false, false,
                               err, sizeof(err));
        CHECK("JSON diagnostics require a diagnostic operation",
              !ok && strstr(err, "requires --list-hf-variants"));
    }

    if (failures) {
        fprintf(stderr, "%d HF argument test(s) failed\n", failures);
        return 1;
    }
    printf("all HF argument tests passed\n");
    return 0;
}
