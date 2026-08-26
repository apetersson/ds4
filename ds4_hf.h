#ifndef DS4_HF_H
#define DS4_HF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS4_HF_REPO_MAX 256
#define DS4_HF_SELECTOR_MAX 128
#define DS4_HF_ENDPOINT_MAX 512
#define DS4_HF_COMMIT_SHA_LEN 40
#define DS4_HF_URL_MAX 4096

/* variants.json v2 is deliberately bounded and data-only. These limits are
 * part of the parser contract, not tunables supplied by a repository. */
#define DS4_HF_MANIFEST_VERSION 2
#define DS4_HF_MANIFEST_MAX_BYTES (256u * 1024u)
#define DS4_HF_MANIFEST_MAX_DEPTH 16
#define DS4_HF_MANIFEST_MAX_TOKENS 8192
#define DS4_HF_MANIFEST_MAX_VARIANTS 16
#define DS4_HF_MANIFEST_MAX_CAPABILITIES 16
#define DS4_HF_PATH_MAX 512
#define DS4_HF_SHA256_HEX_SIZE 65
#define DS4_HF_METADATA_MAX 128
#define DS4_HF_ACQUISITION_MAX_ARTIFACTS 6
#define DS4_HF_CACHE_PATH_MAX 4096

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

typedef enum {
    DS4_HF_CAP_DEEPSEEK4 = 1u << 0,
    DS4_HF_CAP_TEXT_GENERATION = 1u << 1,
    DS4_HF_CAP_DS4_VISION = 1u << 2,
    DS4_HF_CAP_LLAMA_CPP_MMPROJ = 1u << 3,
    DS4_HF_CAP_DSPARK = 1u << 4,
    DS4_HF_CAP_ROUTE_TOKEN_ID = 1u << 5,
    DS4_HF_CAP_SSD_STREAMING = 1u << 6,
} ds4_hf_manifest_capability;

typedef struct {
    char path[DS4_HF_PATH_MAX];
    uint64_t bytes;
    char sha256[DS4_HF_SHA256_HEX_SIZE];
    char precision[DS4_HF_METADATA_MAX];
    char profile[DS4_HF_METADATA_MAX];
    uint32_t required_capabilities;
    uint32_t optional_capabilities;
    bool supports_ds4;
    bool supports_llama_cpp;
    char ds4_minimum_revision[DS4_HF_METADATA_MAX];
    char llama_cpp_minimum_revision[DS4_HF_METADATA_MAX];
    char gguf_architecture[DS4_HF_METADATA_MAX];
    char gguf_projector_type[DS4_HF_METADATA_MAX];
} ds4_hf_manifest_artifact;

typedef struct {
    ds4_hf_manifest_artifact tower;
    ds4_hf_manifest_artifact projector;
    ds4_hf_manifest_artifact config;
} ds4_hf_manifest_vision_bundle;

/* Independently computed digests of canonical name/shape/BF16 tensor streams.
 * These are intentionally distinct from artifact file hashes: equal values
 * attest lossless tensor identity across the raw DS4 and GGUF representations
 * without trusting filenames, directory locality, or shared LFS objects. */
typedef struct {
    char canonicalization[DS4_HF_METADATA_MAX];
    char source_tower_sha256[DS4_HF_SHA256_HEX_SIZE];
    char source_projector_sha256[DS4_HF_SHA256_HEX_SIZE];
    char mmproj_tower_sha256[DS4_HF_SHA256_HEX_SIZE];
    char mmproj_projector_sha256[DS4_HF_SHA256_HEX_SIZE];
} ds4_hf_manifest_bf16_identity;

typedef struct {
    char selector[DS4_HF_SELECTOR_MAX];
    char directory[DS4_HF_PATH_MAX];
    bool is_default;
    ds4_hf_manifest_artifact receiver;
    ds4_hf_manifest_vision_bundle ds4_vision;
    ds4_hf_manifest_artifact llama_cpp_mmproj;
    ds4_hf_manifest_bf16_identity bf16_tensor_identity;
    bool has_dspark;
    ds4_hf_manifest_artifact dspark;
} ds4_hf_manifest_variant;

typedef struct {
    char image_token[DS4_HF_METADATA_MAX];
    uint32_t image_token_id;
    uint32_t image_size;
    uint32_t encoder_dim;
    uint32_t receiver_dim;
    uint32_t tokens_per_view;
    uint32_t separator_tokens;
    uint32_t minimum_views;
    uint32_t maximum_views;
    char token_formula[DS4_HF_METADATA_MAX];
    uint32_t tile_limit;
    uint32_t tile_threshold_pixels;
    bool global_view_first;
    char color_space[DS4_HF_METADATA_MAX];
    char crop_boundaries[DS4_HF_METADATA_MAX];
    char crop_order[DS4_HF_METADATA_MAX];
    char crop_count_rule[DS4_HF_METADATA_MAX];
    char grid_selection[DS4_HF_METADATA_MAX];
    char grid_tie_break[DS4_HF_METADATA_MAX];
    char resize[DS4_HF_METADATA_MAX];
    char separator_placement[DS4_HF_METADATA_MAX];
    double mean[3];
    double std[3];
} ds4_hf_manifest_vision_metadata;

typedef struct {
    uint32_t schema_version;
    char repository[DS4_HF_REPO_MAX];
    char default_selector[DS4_HF_SELECTOR_MAX];
    ds4_hf_manifest_vision_metadata shared_vision;
    size_t variant_count;
    ds4_hf_manifest_variant variants[DS4_HF_MANIFEST_MAX_VARIANTS];
} ds4_hf_manifest;

/* Summary of metadata read from the ordinary main and discovered companion
 * GGUFs. It is intentionally separate from variants.json so sibling discovery
 * and embedded-metadata compatibility can be proven without the DS4 catalog. */
typedef struct {
    char main_architecture[DS4_HF_METADATA_MAX];
    char main_image_token[DS4_HF_METADATA_MAX];
    uint32_t main_image_token_id;
    bool main_has_vision_tensors;
    bool has_dspark;
    char dspark_architecture[DS4_HF_METADATA_MAX];

    char mmproj_architecture[DS4_HF_METADATA_MAX];
    char mmproj_projector_type[DS4_HF_METADATA_MAX];
    char mmproj_precision[DS4_HF_METADATA_MAX];
    bool mmproj_has_vision_encoder;
    char mmproj_image_token[DS4_HF_METADATA_MAX];
    uint32_t mmproj_image_token_id;
    uint32_t mmproj_image_size;
    uint32_t mmproj_patch_size;
    uint32_t mmproj_embedding_length;
    uint32_t mmproj_encoder_dim;
    uint32_t mmproj_projection_dim;
    uint32_t mmproj_tokens_per_view;
    uint32_t mmproj_separator_tokens;
    uint32_t mmproj_tile_limit;
    uint32_t mmproj_tile_threshold_pixels;
    bool mmproj_global_view_first;
    bool mmproj_separator_last;
    char mmproj_color_space[DS4_HF_METADATA_MAX];
    char mmproj_crop_boundaries[DS4_HF_METADATA_MAX];
    char mmproj_crop_order[DS4_HF_METADATA_MAX];
    char mmproj_crop_count_rule[DS4_HF_METADATA_MAX];
    char mmproj_grid_selection[DS4_HF_METADATA_MAX];
    char mmproj_grid_tie_break[DS4_HF_METADATA_MAX];
    char mmproj_resize[DS4_HF_METADATA_MAX];
    double mmproj_image_mean[3];
    double mmproj_image_std[3];
} ds4_hf_llama_gguf_metadata;

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

typedef enum {
    DS4_HF_RESOLVE_OK = 0,
    DS4_HF_RESOLVE_PRIVATE_OR_GATED,
    DS4_HF_RESOLVE_REPOSITORY_NOT_FOUND,
    DS4_HF_RESOLVE_REVISION_NOT_FOUND,
    DS4_HF_RESOLVE_AUTHENTICATION_FAILED,
    DS4_HF_RESOLVE_NETWORK_FAILED,
    DS4_HF_RESOLVE_TIMEOUT,
    DS4_HF_RESOLVE_MALFORMED_RESPONSE,
    DS4_HF_RESOLVE_INVALID_ARGUMENT,
} ds4_hf_resolve_status;

/* A launch-scoped repository identity. Every metadata or artifact URL must be
 * derived from this record so a moving branch cannot mix repository states. */
typedef struct {
    char endpoint[DS4_HF_ENDPOINT_MAX];
    char repo[DS4_HF_REPO_MAX];
    char commit[DS4_HF_COMMIT_SHA_LEN + 1];
} ds4_hf_resolved_repo;

/* Resolve cfg->revision (or the repository default) through the Hub model API.
 * Credentials are sent as an in-memory HTTP header and are never logged or
 * passed to another process. timeout_ms <= 0 selects the 30 second default. */
ds4_hf_resolve_status ds4_hf_resolve_repository(
    const ds4_hf_cli_config *cfg,
    long timeout_ms,
    ds4_hf_resolved_repo *resolved,
    char *err,
    size_t errlen);

/* Build an immutable /resolve/<commit>/ URL for a manifest or artifact path.
 * This performs no network access or acquisition. */
bool ds4_hf_resolved_file_url(const ds4_hf_resolved_repo *resolved,
                              const char *repo_path,
                              char *url,
                              size_t url_len,
                              char *err,
                              size_t errlen);

const char *ds4_hf_resolve_status_name(ds4_hf_resolve_status status);

/* Parse and validate an in-memory variants.json document. The parser performs
 * no I/O, allocation, environment access, process launch, or runtime loading. */
bool ds4_hf_manifest_parse(const char *json,
                           size_t json_len,
                           ds4_hf_manifest *manifest,
                           char *err,
                           size_t errlen);

const ds4_hf_manifest_variant *ds4_hf_manifest_find_variant(
    const ds4_hf_manifest *manifest, const char *selector);

typedef enum {
    DS4_HF_ROLE_RECEIVER,
    DS4_HF_ROLE_VISION_TOWER,
    DS4_HF_ROLE_VISION_PROJECTOR,
    DS4_HF_ROLE_VISION_CONFIG,
    DS4_HF_ROLE_LLAMA_CPP_MMPROJ,
    DS4_HF_ROLE_DSPARK,
} ds4_hf_artifact_role;

typedef struct {
    ds4_hf_artifact_role role;
    bool requested;
    bool cache_hit;
    char repo_path[DS4_HF_PATH_MAX];
    uint64_t bytes;
    char sha256[DS4_HF_SHA256_HEX_SIZE];
    char destination[DS4_HF_CACHE_PATH_MAX];
} ds4_hf_acquisition_artifact;

/* A complete selected-variant view for acquisition and later diagnostics.
 * Unrequested roles remain present with requested=false so listing/dry-run can
 * expose llama.cpp's sibling mmproj without causing DS4 to download it. */
typedef struct {
    char endpoint[DS4_HF_ENDPOINT_MAX];
    char repository[DS4_HF_REPO_MAX];
    char revision[DS4_HF_COMMIT_SHA_LEN + 1];
    char selector[DS4_HF_SELECTOR_MAX];
    char cache_root[DS4_HF_CACHE_PATH_MAX];
    char integrity_seal[DS4_HF_SHA256_HEX_SIZE];
    size_t artifact_count;
    ds4_hf_acquisition_artifact artifacts[DS4_HF_ACQUISITION_MAX_ARTIFACTS];
} ds4_hf_acquisition_plan;

/* Launch-scoped, verified handoff from repository selection to a frontend.
 * Requested artifacts stay open until the frontend has opened/mapped the
 * receiver (and any companion it actually supports).  open_paths name the
 * held descriptors, never the mutable cache directory entries. */
typedef struct {
    ds4_hf_acquisition_plan plan;
    uint32_t verified_roles;
    int verified_fds[DS4_HF_ACQUISITION_MAX_ARTIFACTS];
    char open_paths[DS4_HF_ACQUISITION_MAX_ARTIFACTS][64];
    bool repository;
    bool vision_bundle_verified;
    ds4_hf_manifest_vision_metadata vision_metadata;
} ds4_hf_runtime;

/* Select one manifest variant and derive revision-pinned cache destinations.
 * Catalog vision and DSpark follow cfg's validated runtime sources. The mmproj
 * is metadata-only unless materialize_llama_cpp_mmproj is explicitly true. */
bool ds4_hf_acquisition_plan_build(
    const ds4_hf_cli_config *cfg,
    const ds4_hf_resolved_repo *resolved,
    const ds4_hf_manifest *manifest,
    bool materialize_llama_cpp_mmproj,
    ds4_hf_acquisition_plan *plan,
    char *err,
    size_t errlen);

/* Acquire requested plan entries through libcurl. Transfers follow HTTP(S)
 * redirects used by HF LFS/Xet, resume private .part files under a process
 * lock, and publish only exact-size, manifest-SHA-256-verified files with an
 * immutable role sidecar. Cache hits are fully rehashed. timeout_ms <= 0
 * leaves the full-transfer timeout unlimited. */
bool ds4_hf_acquisition_execute(const ds4_hf_cli_config *cfg,
                                ds4_hf_acquisition_plan *plan,
                                long timeout_ms,
                                char *err,
                                size_t errlen);

/* Open one requested cache artifact without following symlinks and return a
 * descriptor only after its sealed plan identity, role sidecar, byte count,
 * SHA-256, and stable directory entry all match. The caller owns *fd_out and
 * must keep it open until loading/mapping is complete so path replacement
 * cannot cross the verification-to-use boundary. */
bool ds4_hf_acquisition_open_verified(
    const ds4_hf_acquisition_plan *plan,
    size_t artifact_index,
    int *fd_out,
    char *err,
    size_t errlen);

/* Resolve one immutable repository revision, fetch and parse its bounded
 * variants.json, acquire only runtime-requested roles, then hold verified
 * descriptors for the frontend.  All HF network activity for a successful
 * launch is complete when this function returns. */
bool ds4_hf_runtime_prepare(const ds4_hf_cli_config *cfg,
                            ds4_hf_runtime *runtime,
                            char *err,
                            size_t errlen);

/* Return the descriptor path for a verified requested role, or NULL. */
const char *ds4_hf_runtime_open_path(const ds4_hf_runtime *runtime,
                                     ds4_hf_artifact_role role);

/* Close held handoff descriptors without discarding immutable diagnostics. */
void ds4_hf_runtime_close_verified(ds4_hf_runtime *runtime);

bool ds4_hf_runtime_role_verified(const ds4_hf_runtime *runtime,
                                  ds4_hf_artifact_role role);

/* Inspect only the bounded JSON headers of the held, hash-verified tower and
 * projector descriptors and require the exact DeepEncoderV2/896-to-4096
 * tensor namespaces, shapes, dtype, and route metadata. This is safe to call
 * before receiver allocation. */
bool ds4_hf_runtime_vision_artifacts_compatible(
    const ds4_hf_runtime *runtime, char *err, size_t errlen);

/* Validate the manifest-selected, hash-verified DS4 vision bundle against the
 * receiver's actual image-token lookup before image requests are enabled. */
bool ds4_hf_runtime_vision_compatible(const ds4_hf_runtime *runtime,
                                      uint32_t receiver_image_token_id,
                                      char *err,
                                      size_t errlen);

const char *ds4_hf_artifact_role_name(ds4_hf_artifact_role role);

/* Accept exactly rows = views * 256 + 1 within the manifest's view range. */
bool ds4_hf_manifest_visual_rows_valid(
    const ds4_hf_manifest *manifest, uint32_t rows);

/* Reference crop boundary: [index*extent/parts, (index+1)*extent/parts),
 * evaluated with integer floor division and widened intermediates. */
bool ds4_hf_manifest_crop_bounds(uint32_t extent,
                                 uint32_t parts,
                                 uint32_t index,
                                 uint32_t *start,
                                 uint32_t *end);

/* Validate llama.cpp's data-independent companion naming contract. This does
 * not inspect or trust variants.json: callers provide discovered sibling paths. */
bool ds4_hf_llama_siblings_valid(const char *receiver_path,
                                 const char *mmproj_path,
                                 const char *dspark_path,
                                 char *err,
                                 size_t errlen);

/* Match llama.cpp's case-insensitive tagged-primary rule for safe literal
 * selectors: the primary basename must contain selector followed by '.'/'-'. */
bool ds4_hf_llama_primary_selectable(const char *selector,
                                     const char *receiver_path);

/* Validate metadata summaries extracted from discovered GGUFs. This performs
 * no file loading and has no dependency on variants.json. */
bool ds4_hf_llama_gguf_metadata_valid(
    const ds4_hf_llama_gguf_metadata *metadata,
    char *err,
    size_t errlen);

#endif
