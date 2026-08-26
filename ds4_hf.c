#include "ds4_hf.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SEEN_REPO = 1u << 0,
    SEEN_FILE = 1u << 1,
    SEEN_TOKEN = 1u << 2,
    SEEN_REVISION = 1u << 3,
    SEEN_CACHE_DIR = 1u << 4,
    SEEN_OFFLINE = 1u << 5,
    SEEN_NO_VISION = 1u << 6,
    SEEN_VISION_PYTHON = 1u << 7,
    SEEN_VISION_ENCODER = 1u << 8,
    SEEN_VISION_TOWER = 1u << 9,
    SEEN_VISION_ADAPTER = 1u << 10,
    SEEN_MTP = 1u << 11,
    SEEN_DSPARK = 1u << 12,
};

static bool fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool visible_nonspace(const char *value) {
    if (!value || !value[0]) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (iscntrl(*p) || isspace(*p)) return false;
    }
    return true;
}

static bool valid_repo_part(const char *start, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (iscntrl(c) || isspace(c) || c == ':' || c == '\\') return false;
    }
    return true;
}

static bool parse_repo_spec(ds4_hf_cli_config *cfg, const char *value,
                            char *err, size_t errlen) {
    const char *colon = strchr(value, ':');
    if (colon && strchr(colon + 1, ':')) {
        return fail(err, errlen,
                    "invalid HF repository selector; expected OWNER/REPO[:SELECTOR]");
    }

    size_t repo_len = colon ? (size_t)(colon - value) : strlen(value);
    const char *slash = memchr(value, '/', repo_len);
    if (!slash || memchr(slash + 1, '/', repo_len - (size_t)(slash + 1 - value)) ||
        !valid_repo_part(value, (size_t)(slash - value)) ||
        !valid_repo_part(slash + 1, repo_len - (size_t)(slash + 1 - value)))
    {
        return fail(err, errlen,
                    "invalid HF repository selector; expected OWNER/REPO[:SELECTOR]");
    }
    if (repo_len >= sizeof(cfg->repo)) {
        return fail(err, errlen, "HF repository identifier is too long");
    }
    memcpy(cfg->repo, value, repo_len);
    cfg->repo[repo_len] = '\0';

    cfg->selector_set = colon != NULL;
    cfg->selector[0] = '\0';
    if (!colon) return true;

    const char *selector = colon + 1;
    size_t selector_len = strlen(selector);
    if (!visible_nonspace(selector) || strchr(selector, '/') ||
        selector_len >= sizeof(cfg->selector))
    {
        return fail(err, errlen,
                    "invalid HF selector; expected a non-empty selector without '/', ':' or whitespace");
    }
    memcpy(cfg->selector, selector, selector_len + 1);
    return true;
}

static bool valid_hf_file(const char *path) {
    if (!visible_nonspace(path) || path[0] == '/' || strchr(path, '\\')) return false;
    const char *part = path;
    for (const char *p = path;; p++) {
        if (*p != '/' && *p != '\0') continue;
        size_t len = (size_t)(p - part);
        if (len == 0 || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (*p == '\0') return true;
        part = p + 1;
    }
}

static bool mark_once(ds4_hf_cli_config *cfg, uint32_t bit, const char *arg,
                      char *err, size_t errlen) {
    if (cfg->seen & bit) return fail(err, errlen, "duplicate option: %s", arg);
    cfg->seen |= bit;
    return true;
}

static const char *next_value(int *index, int argc, char **argv,
                              const char *arg, char *err, size_t errlen) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') {
        fail(err, errlen, "missing value for %s", arg);
        return NULL;
    }
    return argv[++(*index)];
}

void ds4_hf_cli_init(ds4_hf_cli_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    const char *token = getenv("HF_TOKEN");
    const char *endpoint = getenv("HF_ENDPOINT");
    cfg->token = token && token[0] ? token : NULL;
    cfg->endpoint = endpoint && endpoint[0] ? endpoint : NULL;
    cfg->receiver_source = DS4_HF_RECEIVER_LOCAL;
}

ds4_hf_cli_parse_result ds4_hf_cli_parse_arg(ds4_hf_cli_config *cfg,
                                              bool server,
                                              int *index,
                                              int argc,
                                              char **argv,
                                              char *err,
                                              size_t errlen) {
    const char *arg = argv[*index];
    uint32_t bit = 0;
    const char **target = NULL;

    if (!strcmp(arg, "-hf") || !strcmp(arg, "-hfr") ||
        !strcmp(arg, "--hf-repo") || !strcmp(arg, "--hf")) {
        bit = SEEN_REPO;
    } else if (!strcmp(arg, "-hff") || !strcmp(arg, "--hf-file")) {
        bit = SEEN_FILE;
        target = &cfg->file;
    } else if (!strcmp(arg, "-hft") || !strcmp(arg, "--hf-token")) {
        bit = SEEN_TOKEN;
        target = &cfg->token;
    } else if (!strcmp(arg, "--hf-revision")) {
        bit = SEEN_REVISION;
        target = &cfg->revision;
    } else if (!strcmp(arg, "--hf-cache-dir")) {
        bit = SEEN_CACHE_DIR;
        target = &cfg->cache_dir;
    } else if (!strcmp(arg, "--offline") || !strcmp(arg, "--hf-offline")) {
        if (!mark_once(cfg, SEEN_OFFLINE, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->offline = true;
        return DS4_HF_CLI_MATCHED;
    } else if (!strcmp(arg, "--mtp")) {
        bit = SEEN_MTP;
        target = &cfg->mtp_path;
    } else if (!strcmp(arg, "--dspark")) {
        if (!mark_once(cfg, SEEN_DSPARK, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->dspark_requested = true;
        return DS4_HF_CLI_MATCHED;
    } else if (server && !strcmp(arg, "--no-vision")) {
        if (!mark_once(cfg, SEEN_NO_VISION, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->no_vision = true;
        return DS4_HF_CLI_MATCHED;
    } else if (server && !strcmp(arg, "--vision-python")) {
        bit = SEEN_VISION_PYTHON;
        target = &cfg->vision_python;
    } else if (server && !strcmp(arg, "--vision-encoder")) {
        bit = SEEN_VISION_ENCODER;
        target = &cfg->vision_encoder;
    } else if (server && !strcmp(arg, "--vision-tower")) {
        bit = SEEN_VISION_TOWER;
        target = &cfg->vision_tower;
    } else if (server && !strcmp(arg, "--vision-adapter")) {
        bit = SEEN_VISION_ADAPTER;
        target = &cfg->vision_adapter;
    } else {
        return DS4_HF_CLI_NO_MATCH;
    }

    if (!mark_once(cfg, bit, arg, err, errlen)) return DS4_HF_CLI_ERROR;
    const char *value = next_value(index, argc, argv, arg, err, errlen);
    if (!value) return DS4_HF_CLI_ERROR;

    if (bit == SEEN_REPO) {
        if (!parse_repo_spec(cfg, value, err, errlen)) return DS4_HF_CLI_ERROR;
    } else {
        if (!value[0]) {
            fail(err, errlen, "empty value for %s", arg);
            return DS4_HF_CLI_ERROR;
        }
        if (bit == SEEN_FILE && !valid_hf_file(value)) {
            fail(err, errlen, "invalid HF file path for %s", arg);
            return DS4_HF_CLI_ERROR;
        }
        if (bit == SEEN_REVISION && !visible_nonspace(value)) {
            fail(err, errlen, "invalid HF revision for %s", arg);
            return DS4_HF_CLI_ERROR;
        }
        *target = value;
        if (bit == SEEN_TOKEN) cfg->token_from_cli = true;
    }
    return DS4_HF_CLI_MATCHED;
}

bool ds4_hf_cli_validate(ds4_hf_cli_config *cfg,
                         bool model_explicit,
                         bool dspark_requested,
                         char *err,
                         size_t errlen) {
    bool have_repo = (cfg->seen & SEEN_REPO) != 0;
    if (have_repo && model_explicit) {
        return fail(err, errlen, "--model and --hf-repo are mutually exclusive");
    }
    if (!have_repo && (cfg->seen & SEEN_FILE)) {
        return fail(err, errlen, "--hf-file requires --hf-repo");
    }
    if (!have_repo && (cfg->seen & SEEN_TOKEN)) {
        return fail(err, errlen, "--hf-token requires --hf-repo");
    }
    if (!have_repo && (cfg->seen & SEEN_REVISION)) {
        return fail(err, errlen, "--hf-revision requires --hf-repo");
    }
    if (!have_repo && (cfg->seen & SEEN_CACHE_DIR)) {
        return fail(err, errlen, "--hf-cache-dir requires --hf-repo");
    }

    cfg->receiver_source = have_repo ? DS4_HF_RECEIVER_REPOSITORY :
                                       DS4_HF_RECEIVER_LOCAL;

    unsigned vision_count = (cfg->vision_python != NULL) +
                            (cfg->vision_encoder != NULL) +
                            (cfg->vision_tower != NULL) +
                            (cfg->vision_adapter != NULL);
    if (cfg->no_vision && vision_count) {
        return fail(err, errlen,
                    "--no-vision cannot be combined with explicit --vision-* options");
    }
    if (vision_count && vision_count != 4) {
        return fail(err, errlen,
                    "explicit vision override requires --vision-python, --vision-encoder, --vision-tower, and --vision-adapter together");
    }
    if (cfg->no_vision) cfg->vision_source = DS4_HF_VISION_DISABLED;
    else if (vision_count == 4) cfg->vision_source = DS4_HF_VISION_EXPLICIT;
    else if (have_repo) cfg->vision_source = DS4_HF_VISION_CATALOG;
    else cfg->vision_source = DS4_HF_VISION_NONE;

    cfg->dspark_requested = cfg->dspark_requested || dspark_requested;
    if (!cfg->dspark_requested) {
        cfg->dspark_source = DS4_HF_DSPARK_NONE;
    } else if (cfg->mtp_path) {
        cfg->dspark_source = DS4_HF_DSPARK_EXPLICIT_MTP;
    } else if (have_repo) {
        cfg->dspark_source = DS4_HF_DSPARK_CATALOG;
    } else {
        return fail(err, errlen,
                    "--dspark requires either explicit --mtp FILE or --hf-repo");
    }
    return true;
}

bool ds4_hf_selector_equal(const char *left, const char *right) {
    if (!left || !right) return left == right;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (tolower(a) != tolower(b)) return false;
    }
    return *left == *right;
}
