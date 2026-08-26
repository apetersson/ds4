#ifndef DS4_HF_H
#define DS4_HF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS4_HF_REPO_MAX 256
#define DS4_HF_SELECTOR_MAX 128

typedef enum {
    DS4_HF_CLI_NO_MATCH,
    DS4_HF_CLI_MATCHED,
    DS4_HF_CLI_ERROR,
} ds4_hf_cli_parse_result;

typedef enum {
    DS4_HF_RECEIVER_LOCAL,
    DS4_HF_RECEIVER_REPOSITORY,
} ds4_hf_receiver_source;

typedef enum {
    DS4_HF_VISION_NONE,
    DS4_HF_VISION_CATALOG,
    DS4_HF_VISION_EXPLICIT,
    DS4_HF_VISION_DISABLED,
} ds4_hf_vision_source;

typedef enum {
    DS4_HF_DSPARK_NONE,
    DS4_HF_DSPARK_CATALOG,
    DS4_HF_DSPARK_EXPLICIT_MTP,
} ds4_hf_dspark_source;

/* Data-only command-line contract. Repository access, manifest parsing,
 * acquisition, and runtime wiring are deliberately owned by later resolver
 * stages. Environment-backed pointers remain owned by the process. */
typedef struct {
    char repo[DS4_HF_REPO_MAX];
    char selector[DS4_HF_SELECTOR_MAX];
    bool selector_set;
    const char *file;
    const char *token;
    const char *endpoint;
    const char *revision;
    const char *cache_dir;
    bool token_from_cli;
    bool offline;

    const char *vision_python;
    const char *vision_encoder;
    const char *vision_tower;
    const char *vision_adapter;
    bool no_vision;

    const char *mtp_path;
    bool dspark_requested;

    ds4_hf_receiver_source receiver_source;
    ds4_hf_vision_source vision_source;
    ds4_hf_dspark_source dspark_source;

    uint32_t seen;
} ds4_hf_cli_config;

void ds4_hf_cli_init(ds4_hf_cli_config *cfg);

/* Parse one shared option. Vision options are recognized only for ds4-server.
 * Errors never include credential values. */
ds4_hf_cli_parse_result ds4_hf_cli_parse_arg(ds4_hf_cli_config *cfg,
                                              bool server,
                                              int *index,
                                              int argc,
                                              char **argv,
                                              char *err,
                                              size_t errlen);

/* Resolve receiver and companion precedence after the frontend has parsed its
 * existing options. This performs no I/O and must run before model allocation. */
bool ds4_hf_cli_validate(ds4_hf_cli_config *cfg,
                         bool server,
                         bool model_explicit,
                         bool dspark_requested,
                         char *err,
                         size_t errlen);

bool ds4_hf_selector_equal(const char *left, const char *right);

#endif
