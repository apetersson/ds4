#include "ds4_hf.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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
    SEEN_LIST_VARIANTS = 1u << 13,
    SEEN_DRY_RUN = 1u << 14,
    SEEN_DIAGNOSTICS_JSON = 1u << 15,
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
    } else if (!strcmp(arg, "--list-hf-variants")) {
        if (!mark_once(cfg, SEEN_LIST_VARIANTS, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->list_variants = true;
        return DS4_HF_CLI_MATCHED;
    } else if (!strcmp(arg, "--hf-dry-run")) {
        if (!mark_once(cfg, SEEN_DRY_RUN, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->dry_run = true;
        return DS4_HF_CLI_MATCHED;
    } else if (!strcmp(arg, "--json") || !strcmp(arg, "--hf-json")) {
        if (!mark_once(cfg, SEEN_DIAGNOSTICS_JSON, arg, err, errlen)) return DS4_HF_CLI_ERROR;
        cfg->diagnostics_json = true;
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
                         bool server,
                         bool model_explicit,
                         bool dspark_requested,
                         char *err,
                         size_t errlen) {
    bool have_repo = (cfg->seen & SEEN_REPO) != 0;
    if (cfg->list_variants && cfg->dry_run) {
        return fail(err, errlen,
                    "--list-hf-variants and --hf-dry-run are mutually exclusive");
    }
    if ((cfg->list_variants || cfg->dry_run) && !have_repo) {
        return fail(err, errlen,
                    "HF diagnostics require --hf-repo OWNER/REPO[:SELECTOR]");
    }
    if (cfg->diagnostics_json && !cfg->list_variants && !cfg->dry_run) {
        return fail(err, errlen,
                    "--json/--hf-json requires --list-hf-variants or --hf-dry-run");
    }
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

    unsigned vision_program_count = (cfg->vision_python != NULL) +
                                    (cfg->vision_encoder != NULL);
    unsigned vision_weight_count = (cfg->vision_tower != NULL) +
                                   (cfg->vision_adapter != NULL);
    unsigned vision_count = vision_program_count + vision_weight_count;
    if (cfg->no_vision && vision_count) {
        return fail(err, errlen,
                    "--no-vision cannot be combined with explicit --vision-* options");
    }
    if (vision_program_count == 1) {
        return fail(err, errlen,
                    "trusted local vision runtime requires --vision-python and --vision-encoder together");
    }
    if (vision_weight_count == 1) {
        return fail(err, errlen,
                    "explicit vision override requires --vision-tower and --vision-adapter together");
    }
    if (vision_weight_count && vision_program_count != 2) {
        return fail(err, errlen,
                    "explicit vision override requires --vision-python, --vision-encoder, --vision-tower, and --vision-adapter together");
    }
    if (vision_program_count && !vision_weight_count && !(have_repo && server)) {
        return fail(err, errlen,
                    "--vision-python and --vision-encoder require -hf catalog weights or a complete explicit local vision bundle");
    }
    if (cfg->no_vision) cfg->vision_source = DS4_HF_VISION_DISABLED;
    else if (vision_weight_count == 2) cfg->vision_source = DS4_HF_VISION_EXPLICIT;
    else if (have_repo && server) cfg->vision_source = DS4_HF_VISION_CATALOG;
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
    if (cfg->dry_run && cfg->vision_source == DS4_HF_VISION_EXPLICIT) {
        return fail(err, errlen,
                    "--hf-dry-run cannot report explicit local --vision-tower/--vision-adapter weights; use catalog vision or --no-vision");
    }
    if (cfg->dry_run && cfg->mtp_path) {
        return fail(err, errlen,
                    "--hf-dry-run cannot report an explicit local --mtp support file; omit --mtp to plan catalog DSpark");
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

typedef struct {
    const char *p;
    const char *end;
    unsigned depth;
    unsigned tokens;
    char *err;
    size_t errlen;
} manifest_json_parser;

typedef enum {
    ARTIFACT_RECEIVER,
    ARTIFACT_DS4_VISION,
    ARTIFACT_LLAMA_MMPROJ,
    ARTIFACT_DSPARK,
} manifest_artifact_role;

static bool json_fail(manifest_json_parser *jp, const char *fmt, ...) {
    if (jp->err && jp->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(jp->err, jp->errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static void manifest_json_ws(manifest_json_parser *jp) {
    while (jp->p < jp->end && isspace((unsigned char)*jp->p)) jp->p++;
}

static bool manifest_json_charge(manifest_json_parser *jp) {
    if (++jp->tokens > DS4_HF_MANIFEST_MAX_TOKENS) {
        return json_fail(jp, "manifest exceeds the %u-token parser limit",
                         DS4_HF_MANIFEST_MAX_TOKENS);
    }
    return true;
}

static bool manifest_json_open(manifest_json_parser *jp, char ch) {
    manifest_json_ws(jp);
    if (jp->p >= jp->end || *jp->p != ch) {
        return json_fail(jp, "expected '%c' in manifest", ch);
    }
    if (!manifest_json_charge(jp)) return false;
    if (++jp->depth > DS4_HF_MANIFEST_MAX_DEPTH) {
        return json_fail(jp, "manifest exceeds the %u-level nesting limit",
                         DS4_HF_MANIFEST_MAX_DEPTH);
    }
    jp->p++;
    return true;
}

static bool manifest_json_close(manifest_json_parser *jp, char ch) {
    manifest_json_ws(jp);
    if (jp->p >= jp->end || *jp->p != ch) {
        return json_fail(jp, "expected '%c' in manifest", ch);
    }
    jp->p++;
    jp->depth--;
    return true;
}

static int manifest_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void sha256_text_lower(char hash[DS4_HF_SHA256_HEX_SIZE]) {
    for (size_t i = 0; i < DS4_HF_SHA256_HEX_SIZE && hash[i]; i++) {
        hash[i] = (char)tolower((unsigned char)hash[i]);
    }
}

static bool manifest_json_u16(manifest_json_parser *jp, uint32_t *out) {
    if ((size_t)(jp->end - jp->p) < 4) return json_fail(jp, "truncated Unicode escape");
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) {
        int digit = manifest_hex_digit(jp->p[i]);
        if (digit < 0) return json_fail(jp, "malformed Unicode escape");
        value = value * 16u + (uint32_t)digit;
    }
    jp->p += 4;
    *out = value;
    return true;
}

static bool manifest_string_byte(manifest_json_parser *jp, char *out,
                                 size_t cap, size_t *len, unsigned char byte) {
    if (out) {
        if (*len + 1 >= cap) return json_fail(jp, "manifest string exceeds its field limit");
        out[*len] = (char)byte;
    }
    (*len)++;
    return true;
}

static bool manifest_string_codepoint(manifest_json_parser *jp, char *out,
                                      size_t cap, size_t *len, uint32_t cp) {
    if (cp <= 0x7f) return manifest_string_byte(jp, out, cap, len, (unsigned char)cp);
    if (cp <= 0x7ff) {
        return manifest_string_byte(jp, out, cap, len, (unsigned char)(0xc0 | (cp >> 6))) &&
               manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | (cp & 0x3f)));
    }
    if (cp <= 0xffff) {
        return manifest_string_byte(jp, out, cap, len, (unsigned char)(0xe0 | (cp >> 12))) &&
               manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | ((cp >> 6) & 0x3f))) &&
               manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | (cp & 0x3f)));
    }
    return manifest_string_byte(jp, out, cap, len, (unsigned char)(0xf0 | (cp >> 18))) &&
           manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | ((cp >> 12) & 0x3f))) &&
           manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | ((cp >> 6) & 0x3f))) &&
           manifest_string_byte(jp, out, cap, len, (unsigned char)(0x80 | (cp & 0x3f)));
}

static bool manifest_json_string(manifest_json_parser *jp, char *out, size_t cap) {
    manifest_json_ws(jp);
    if (jp->p >= jp->end || *jp->p++ != '"') return json_fail(jp, "expected JSON string");
    size_t len = 0;
    while (jp->p < jp->end) {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '"') {
            if (out) out[len] = '\0';
            return true;
        }
        if (c < 0x20) return json_fail(jp, "control character in manifest string");
        if (c != '\\') {
            if (!manifest_string_byte(jp, out, cap, &len, c)) return false;
            continue;
        }
        if (jp->p >= jp->end) return json_fail(jp, "truncated JSON escape");
        c = (unsigned char)*jp->p++;
        switch (c) {
        case '"': case '\\': case '/':
            if (!manifest_string_byte(jp, out, cap, &len, c)) return false;
            break;
        case 'b': case 'f': case 'n': case 'r': case 't':
            if (out) return json_fail(jp, "control escape is not allowed in manifest fields");
            break;
        case 'u': {
            uint32_t cp;
            if (!manifest_json_u16(jp, &cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if ((size_t)(jp->end - jp->p) < 6 || jp->p[0] != '\\' || jp->p[1] != 'u') {
                    return json_fail(jp, "unpaired high Unicode surrogate");
                }
                jp->p += 2;
                uint32_t low;
                if (!manifest_json_u16(jp, &low) || low < 0xdc00 || low > 0xdfff) {
                    return json_fail(jp, "invalid low Unicode surrogate");
                }
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                return json_fail(jp, "unpaired low Unicode surrogate");
            }
            if (out && cp < 0x20) {
                return json_fail(jp, "escaped control character is not allowed in manifest fields");
            }
            if (!manifest_string_codepoint(jp, out, cap, &len, cp)) return false;
            break;
        }
        default:
            return json_fail(jp, "invalid JSON escape");
        }
    }
    return json_fail(jp, "unterminated manifest string");
}

static bool manifest_json_colon(manifest_json_parser *jp) {
    manifest_json_ws(jp);
    if (jp->p >= jp->end || *jp->p++ != ':') return json_fail(jp, "expected ':' in manifest");
    return true;
}

static bool manifest_json_next(manifest_json_parser *jp, char close, bool *more) {
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == ',') {
        jp->p++;
        *more = true;
        return true;
    }
    if (!manifest_json_close(jp, close)) return false;
    *more = false;
    return true;
}

static bool manifest_json_uint64(manifest_json_parser *jp, uint64_t *out) {
    manifest_json_ws(jp);
    if (!manifest_json_charge(jp)) return false;
    if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) {
        return json_fail(jp, "expected unsigned integer in manifest");
    }
    if (*jp->p == '0' && jp->p + 1 < jp->end && isdigit((unsigned char)jp->p[1])) {
        return json_fail(jp, "leading zero in manifest integer");
    }
    uint64_t value = 0;
    do {
        unsigned digit = (unsigned)(*jp->p - '0');
        if (value > (UINT64_MAX - digit) / 10u) return json_fail(jp, "manifest integer overflow");
        value = value * 10u + digit;
        jp->p++;
    } while (jp->p < jp->end && isdigit((unsigned char)*jp->p));
    if (jp->p < jp->end && (*jp->p == '.' || *jp->p == 'e' || *jp->p == 'E')) {
        return json_fail(jp, "expected integer, not fractional manifest number");
    }
    *out = value;
    return true;
}

static bool manifest_json_u32(manifest_json_parser *jp, uint32_t *out) {
    uint64_t value;
    if (!manifest_json_uint64(jp, &value)) return false;
    if (value > UINT32_MAX) return json_fail(jp, "manifest integer exceeds uint32 range");
    *out = (uint32_t)value;
    return true;
}

static bool manifest_json_bool(manifest_json_parser *jp, bool *out) {
    manifest_json_ws(jp);
    if (!manifest_json_charge(jp)) return false;
    size_t remain = (size_t)(jp->end - jp->p);
    if (remain >= 4 && !memcmp(jp->p, "true", 4)) {
        jp->p += 4;
        *out = true;
        return true;
    }
    if (remain >= 5 && !memcmp(jp->p, "false", 5)) {
        jp->p += 5;
        *out = false;
        return true;
    }
    return json_fail(jp, "expected boolean in manifest");
}

static bool manifest_json_scan_number(manifest_json_parser *jp,
                                      const char **start_out,
                                      size_t *len_out) {
    manifest_json_ws(jp);
    if (!manifest_json_charge(jp)) return false;
    const char *start = jp->p;
    if (jp->p < jp->end && *jp->p == '-') jp->p++;
    if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) return json_fail(jp, "expected number");
    if (*jp->p == '0') jp->p++;
    else while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    if (jp->p < jp->end && *jp->p == '.') {
        jp->p++;
        if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) return json_fail(jp, "malformed number");
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
        jp->p++;
        if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-')) jp->p++;
        if (jp->p >= jp->end || !isdigit((unsigned char)*jp->p)) return json_fail(jp, "malformed exponent");
        while (jp->p < jp->end && isdigit((unsigned char)*jp->p)) jp->p++;
    }
    *start_out = start;
    *len_out = (size_t)(jp->p - start);
    return true;
}

static bool manifest_json_double(manifest_json_parser *jp, double *out) {
    const char *start;
    size_t len;
    if (!manifest_json_scan_number(jp, &start, &len)) return false;
    if (len >= 64) return json_fail(jp, "manifest number is too long");
    char text[64];
    memcpy(text, start, len);
    text[len] = '\0';
    errno = 0;
    char *number_end;
    double value = strtod(text, &number_end);
    if (errno || *number_end || !isfinite(value)) return json_fail(jp, "invalid manifest number");
    *out = value;
    return true;
}

static bool manifest_key_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left++) != tolower((unsigned char)*right++)) return false;
    }
    return *left == *right;
}

static bool manifest_dangerous_key(const char *key) {
    static const char *const denied[] = {
        "argv", "args", "environment", "env", "executable", "interpreter",
        "shell", "command", "entrypoint", "python", "auto_map",
        "remote_code", "trust_remote_code",
    };
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        if (manifest_key_equal(key, denied[i])) return true;
    }
    return false;
}

static bool manifest_json_key(manifest_json_parser *jp, char *key, size_t cap) {
    if (!manifest_json_charge(jp) || !manifest_json_string(jp, key, cap)) return false;
    if (manifest_dangerous_key(key)) {
        return json_fail(jp, "manifest field '%s' is executable configuration and is forbidden", key);
    }
    return manifest_json_colon(jp);
}

static bool manifest_json_skip_value(manifest_json_parser *jp);

static bool manifest_json_skip_object(manifest_json_parser *jp) {
    if (!manifest_json_open(jp, '{')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return manifest_json_close(jp, '}');
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key)) || !manifest_json_skip_value(jp) ||
            !manifest_json_next(jp, '}', &more)) return false;
    }
    return true;
}

static bool manifest_json_skip_array(manifest_json_parser *jp) {
    if (!manifest_json_open(jp, '[')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') return manifest_json_close(jp, ']');
    bool more = true;
    while (more) {
        if (!manifest_json_skip_value(jp) || !manifest_json_next(jp, ']', &more)) return false;
    }
    return true;
}

static bool manifest_json_skip_number(manifest_json_parser *jp) {
    const char *start;
    size_t len;
    return manifest_json_scan_number(jp, &start, &len);
}

static bool manifest_json_skip_value(manifest_json_parser *jp) {
    manifest_json_ws(jp);
    if (jp->p >= jp->end) return json_fail(jp, "missing manifest value");
    if (*jp->p == '{') return manifest_json_skip_object(jp);
    if (*jp->p == '[') return manifest_json_skip_array(jp);
    if (*jp->p == '"') {
        if (!manifest_json_charge(jp)) return false;
        return manifest_json_string(jp, NULL, 0);
    }
    if (*jp->p == '-' || isdigit((unsigned char)*jp->p)) return manifest_json_skip_number(jp);
    bool ignored;
    if (*jp->p == 't' || *jp->p == 'f') return manifest_json_bool(jp, &ignored);
    if ((size_t)(jp->end - jp->p) >= 4 && !memcmp(jp->p, "null", 4)) {
        if (!manifest_json_charge(jp)) return false;
        jp->p += 4;
        return true;
    }
    return json_fail(jp, "invalid manifest value");
}

static bool manifest_mark_field(manifest_json_parser *jp, uint64_t *seen,
                                uint64_t bit, const char *key) {
    if (*seen & bit) return json_fail(jp, "duplicate manifest field '%s'", key);
    *seen |= bit;
    return true;
}

static uint32_t manifest_capability(const char *name) {
    if (!strcmp(name, "deepseek4")) return DS4_HF_CAP_DEEPSEEK4;
    if (!strcmp(name, "text-generation")) return DS4_HF_CAP_TEXT_GENERATION;
    if (!strcmp(name, "ds4-vision")) return DS4_HF_CAP_DS4_VISION;
    if (!strcmp(name, "llama-cpp-mmproj")) return DS4_HF_CAP_LLAMA_CPP_MMPROJ;
    if (!strcmp(name, "dspark")) return DS4_HF_CAP_DSPARK;
    if (!strcmp(name, "route-token-id")) return DS4_HF_CAP_ROUTE_TOKEN_ID;
    if (!strcmp(name, "ssd-streaming")) return DS4_HF_CAP_SSD_STREAMING;
    return 0;
}

static bool manifest_parse_capability_array(manifest_json_parser *jp,
                                            bool required,
                                            ds4_hf_manifest_artifact *artifact) {
    uint32_t *bits = required ? &artifact->required_capabilities
                              : &artifact->optional_capabilities;
    if (!manifest_json_open(jp, '[')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') return manifest_json_close(jp, ']');
    unsigned count = 0;
    bool more = true;
    while (more) {
        char capability[DS4_HF_METADATA_MAX];
        if (++count > DS4_HF_MANIFEST_MAX_CAPABILITIES) {
            return json_fail(jp, "capability list exceeds the %u-entry limit",
                             DS4_HF_MANIFEST_MAX_CAPABILITIES);
        }
        if (!manifest_json_charge(jp) ||
            !manifest_json_string(jp, capability, sizeof(capability))) return false;
        uint32_t bit = manifest_capability(capability);
        if (!bit && required) {
            return json_fail(jp, "unknown required capability '%s'", capability);
        }
        if (bit) {
            if (*bits & bit) return json_fail(jp, "duplicate capability '%s'", capability);
            *bits |= bit;
        } else {
            for (size_t i = 0;
                 i < artifact->unknown_optional_capability_count; i++) {
                if (!strcmp(artifact->unknown_optional_capabilities[i],
                            capability)) {
                    return json_fail(jp, "duplicate capability '%s'",
                                     capability);
                }
            }
            size_t index = artifact->unknown_optional_capability_count++;
            memcpy(artifact->unknown_optional_capabilities[index], capability,
                   strlen(capability) + 1);
        }
        if (!manifest_json_next(jp, ']', &more)) return false;
    }
    return true;
}

static bool manifest_parse_capabilities(manifest_json_parser *jp,
                                        ds4_hf_manifest_artifact *artifact) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "capabilities object is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "required")) {
            if (!manifest_mark_field(jp, &seen, 1, key) ||
                !manifest_parse_capability_array(jp, true, artifact)) return false;
        } else if (!strcmp(key, "optional")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_parse_capability_array(jp, false, artifact)) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (!(seen & 1)) return json_fail(jp, "capabilities.required is missing");
    return true;
}

static bool manifest_safe_atom(const char *value);

static bool manifest_parse_revision(manifest_json_parser *jp, char *revision,
                                    size_t revision_cap) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "runtime constraint is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "minimum_revision")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, revision, revision_cap)) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (!(seen & 1) || !manifest_safe_atom(revision)) {
        return json_fail(jp, "runtime minimum_revision is missing or unsafe");
    }
    return true;
}

static bool manifest_parse_runtime(manifest_json_parser *jp,
                                   ds4_hf_manifest_artifact *artifact) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "runtime object is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "ds4")) {
            if (!manifest_mark_field(jp, &seen, 1, key) ||
                !manifest_parse_revision(jp, artifact->ds4_minimum_revision,
                                         sizeof(artifact->ds4_minimum_revision))) return false;
            artifact->supports_ds4 = true;
        } else if (!strcmp(key, "llama_cpp")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_parse_revision(jp, artifact->llama_cpp_minimum_revision,
                                         sizeof(artifact->llama_cpp_minimum_revision))) return false;
            artifact->supports_llama_cpp = true;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (!seen) return json_fail(jp, "runtime has no supported DS4 or llama.cpp constraint");
    return true;
}

static bool manifest_parse_gguf_metadata(manifest_json_parser *jp,
                                         ds4_hf_manifest_artifact *artifact) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return manifest_json_close(jp, '}');
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "architecture")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->gguf_architecture,
                                      sizeof(artifact->gguf_architecture))) return false;
        } else if (!strcmp(key, "projector_type")) {
            if (!manifest_mark_field(jp, &seen, 2, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->gguf_projector_type,
                                      sizeof(artifact->gguf_projector_type))) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    return true;
}

static bool manifest_parse_artifact(manifest_json_parser *jp,
                                    ds4_hf_manifest_artifact *artifact) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "artifact record is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "path")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->path, sizeof(artifact->path))) return false;
        } else if (!strcmp(key, "bytes")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_json_uint64(jp, &artifact->bytes)) return false;
        } else if (!strcmp(key, "sha256")) {
            if (!manifest_mark_field(jp, &seen, 4, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->sha256, sizeof(artifact->sha256))) return false;
        } else if (!strcmp(key, "precision")) {
            if (!manifest_mark_field(jp, &seen, 8, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->precision, sizeof(artifact->precision))) return false;
        } else if (!strcmp(key, "profile")) {
            if (!manifest_mark_field(jp, &seen, 16, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, artifact->profile, sizeof(artifact->profile))) return false;
        } else if (!strcmp(key, "capabilities")) {
            if (!manifest_mark_field(jp, &seen, 32, key) ||
                !manifest_parse_capabilities(jp, artifact)) return false;
        } else if (!strcmp(key, "runtime")) {
            if (!manifest_mark_field(jp, &seen, 64, key) ||
                !manifest_parse_runtime(jp, artifact)) return false;
        } else if (!strcmp(key, "gguf_metadata")) {
            if (!manifest_mark_field(jp, &seen, 128, key) ||
                !manifest_parse_gguf_metadata(jp, artifact)) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if ((seen & 127u) != 127u) return json_fail(jp, "artifact record is missing a required field");
    sha256_text_lower(artifact->sha256);
    return true;
}

static bool manifest_suffix(const char *value, const char *suffix) {
    size_t value_len = strlen(value), suffix_len = strlen(suffix);
    return value_len >= suffix_len && !strcmp(value + value_len - suffix_len, suffix);
}

static bool manifest_safe_atom(const char *value) {
    if (!value || !value[0]) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '+')) return false;
    }
    return true;
}

static bool manifest_safe_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') || strchr(path, ':')) return false;
    const char *part = path;
    for (const char *p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c && !(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/')) return false;
        if (c != '/' && c != '\0') continue;
        size_t len = (size_t)(p - part);
        if (!len || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.') || part[0] == '-') return false;
        if (!c) return true;
        part = p + 1;
    }
}

static bool manifest_valid_sha256(const char *hash) {
    if (strlen(hash) != 64) return false;
    for (size_t i = 0; i < 64; i++) if (manifest_hex_digit(hash[i]) < 0) return false;
    return true;
}

static bool manifest_validate_artifact(manifest_json_parser *jp,
                                       const ds4_hf_manifest_artifact *artifact,
                                       manifest_artifact_role role) {
    if (!manifest_safe_path(artifact->path)) return json_fail(jp, "unsafe artifact path '%s'", artifact->path);
    if (!artifact->bytes) return json_fail(jp, "artifact '%s' has invalid zero byte count", artifact->path);
    if (!manifest_valid_sha256(artifact->sha256)) return json_fail(jp, "artifact '%s' has malformed SHA-256", artifact->path);
    if (!manifest_safe_atom(artifact->precision) || !manifest_safe_atom(artifact->profile)) {
        return json_fail(jp, "artifact '%s' has unsafe precision or profile", artifact->path);
    }

    uint32_t required = 0, allowed = 0;
    bool want_ds4 = false, want_llama = false, want_gguf = false;
    switch (role) {
    case ARTIFACT_RECEIVER:
        required = DS4_HF_CAP_DEEPSEEK4 | DS4_HF_CAP_TEXT_GENERATION;
        allowed = required | DS4_HF_CAP_SSD_STREAMING;
        want_ds4 = want_llama = want_gguf = true;
        break;
    case ARTIFACT_DS4_VISION:
        required = allowed = DS4_HF_CAP_DS4_VISION;
        want_ds4 = true;
        break;
    case ARTIFACT_LLAMA_MMPROJ:
        required = allowed = DS4_HF_CAP_LLAMA_CPP_MMPROJ | DS4_HF_CAP_ROUTE_TOKEN_ID;
        want_llama = want_gguf = true;
        break;
    case ARTIFACT_DSPARK:
        required = DS4_HF_CAP_DSPARK;
        allowed = required | DS4_HF_CAP_SSD_STREAMING;
        want_ds4 = want_llama = want_gguf = true;
        break;
    }
    uint32_t declared = artifact->required_capabilities | artifact->optional_capabilities;
    if ((artifact->required_capabilities & required) != required || (declared & ~allowed)) {
        return json_fail(jp, "artifact '%s' has capabilities inconsistent with its role", artifact->path);
    }
    if (artifact->supports_ds4 != want_ds4 || artifact->supports_llama_cpp != want_llama) {
        return json_fail(jp, "artifact '%s' has runtime constraints inconsistent with its role", artifact->path);
    }
    if (want_gguf) {
        if (!manifest_suffix(artifact->path, ".gguf")) return json_fail(jp, "GGUF artifact '%s' must end in .gguf", artifact->path);
        if ((role == ARTIFACT_LLAMA_MMPROJ &&
             (strcmp(artifact->gguf_architecture, "clip") ||
              strcmp(artifact->gguf_projector_type, "deepencoder_v2_dsv4"))) ||
            (role != ARTIFACT_LLAMA_MMPROJ && strcmp(artifact->gguf_architecture, "deepseek4"))) {
            return json_fail(jp, "artifact '%s' has incompatible GGUF metadata", artifact->path);
        }
    } else if (artifact->gguf_architecture[0] || artifact->gguf_projector_type[0]) {
        return json_fail(jp, "non-GGUF artifact '%s' declares GGUF metadata", artifact->path);
    }
    return true;
}

static bool manifest_parse_vision_bundle(manifest_json_parser *jp,
                                         ds4_hf_manifest_vision_bundle *bundle) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "DS4 vision bundle is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "tower")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_parse_artifact(jp, &bundle->tower)) return false;
        } else if (!strcmp(key, "projector")) {
            if (!manifest_mark_field(jp, &seen, 2, key) || !manifest_parse_artifact(jp, &bundle->projector)) return false;
        } else if (!strcmp(key, "config")) {
            if (!manifest_mark_field(jp, &seen, 4, key) || !manifest_parse_artifact(jp, &bundle->config)) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (!(seen & 1)) return json_fail(jp, "incomplete DS4 vision bundle: missing exact role ds4_vision.tower");
    if (!(seen & 2)) return json_fail(jp, "incomplete DS4 vision bundle: missing exact role ds4_vision.projector");
    if (!(seen & 4)) return json_fail(jp, "incomplete DS4 vision bundle: missing exact role ds4_vision.config");
    return true;
}

static bool manifest_parse_triple(manifest_json_parser *jp, double values[3]) {
    if (!manifest_json_open(jp, '[')) return false;
    for (unsigned i = 0; i < 3; i++) {
        if (!manifest_json_double(jp, &values[i])) return false;
        manifest_json_ws(jp);
        if (i != 2) {
            if (jp->p >= jp->end || *jp->p++ != ',') return json_fail(jp, "expected three preprocessing values");
        }
    }
    return manifest_json_close(jp, ']');
}

static bool manifest_parse_preprocessing(manifest_json_parser *jp,
                                         ds4_hf_manifest_vision_metadata *vision) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "preprocessing contract is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "tile_limit")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_u32(jp, &vision->tile_limit)) return false;
        } else if (!strcmp(key, "tile_threshold_pixels")) {
            if (!manifest_mark_field(jp, &seen, 2, key) || !manifest_json_u32(jp, &vision->tile_threshold_pixels)) return false;
        } else if (!strcmp(key, "global_view_first")) {
            if (!manifest_mark_field(jp, &seen, 4, key) || !manifest_json_bool(jp, &vision->global_view_first)) return false;
        } else if (!strcmp(key, "crop_order")) {
            if (!manifest_mark_field(jp, &seen, 8, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->crop_order, sizeof(vision->crop_order))) return false;
        } else if (!strcmp(key, "resize")) {
            if (!manifest_mark_field(jp, &seen, 16, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->resize, sizeof(vision->resize))) return false;
        } else if (!strcmp(key, "mean")) {
            if (!manifest_mark_field(jp, &seen, 32, key) || !manifest_parse_triple(jp, vision->mean)) return false;
        } else if (!strcmp(key, "std")) {
            if (!manifest_mark_field(jp, &seen, 64, key) || !manifest_parse_triple(jp, vision->std)) return false;
        } else if (!strcmp(key, "color_space")) {
            if (!manifest_mark_field(jp, &seen, 128, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->color_space, sizeof(vision->color_space))) return false;
        } else if (!strcmp(key, "crop_boundaries")) {
            if (!manifest_mark_field(jp, &seen, 256, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->crop_boundaries, sizeof(vision->crop_boundaries))) return false;
        } else if (!strcmp(key, "grid_selection")) {
            if (!manifest_mark_field(jp, &seen, 512, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->grid_selection, sizeof(vision->grid_selection))) return false;
        } else if (!strcmp(key, "grid_tie_break")) {
            if (!manifest_mark_field(jp, &seen, 1024, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->grid_tie_break, sizeof(vision->grid_tie_break))) return false;
        } else if (!strcmp(key, "separator_placement")) {
            if (!manifest_mark_field(jp, &seen, 2048, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->separator_placement,
                                      sizeof(vision->separator_placement))) return false;
        } else if (!strcmp(key, "crop_count_rule")) {
            if (!manifest_mark_field(jp, &seen, 4096, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->crop_count_rule,
                                      sizeof(vision->crop_count_rule))) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (seen != 8191) return json_fail(jp, "preprocessing contract is incomplete");
    return true;
}

static bool manifest_parse_shared_vision(manifest_json_parser *jp,
                                         ds4_hf_manifest_vision_metadata *vision) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "shared_vision is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "image_token")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->image_token, sizeof(vision->image_token))) return false;
        } else if (!strcmp(key, "image_token_id")) {
            if (!manifest_mark_field(jp, &seen, 2, key) || !manifest_json_u32(jp, &vision->image_token_id)) return false;
        } else if (!strcmp(key, "image_size")) {
            if (!manifest_mark_field(jp, &seen, 4, key) || !manifest_json_u32(jp, &vision->image_size)) return false;
        } else if (!strcmp(key, "encoder_dim")) {
            if (!manifest_mark_field(jp, &seen, 8, key) || !manifest_json_u32(jp, &vision->encoder_dim)) return false;
        } else if (!strcmp(key, "receiver_dim")) {
            if (!manifest_mark_field(jp, &seen, 16, key) || !manifest_json_u32(jp, &vision->receiver_dim)) return false;
        } else if (!strcmp(key, "tokens_per_view")) {
            if (!manifest_mark_field(jp, &seen, 32, key) || !manifest_json_u32(jp, &vision->tokens_per_view)) return false;
        } else if (!strcmp(key, "separator_tokens")) {
            if (!manifest_mark_field(jp, &seen, 64, key) || !manifest_json_u32(jp, &vision->separator_tokens)) return false;
        } else if (!strcmp(key, "minimum_views")) {
            if (!manifest_mark_field(jp, &seen, 128, key) || !manifest_json_u32(jp, &vision->minimum_views)) return false;
        } else if (!strcmp(key, "maximum_views")) {
            if (!manifest_mark_field(jp, &seen, 256, key) || !manifest_json_u32(jp, &vision->maximum_views)) return false;
        } else if (!strcmp(key, "token_formula")) {
            if (!manifest_mark_field(jp, &seen, 512, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, vision->token_formula, sizeof(vision->token_formula))) return false;
        } else if (!strcmp(key, "preprocessing")) {
            if (!manifest_mark_field(jp, &seen, 1024, key) ||
                !manifest_parse_preprocessing(jp, vision)) return false;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (seen != 2047) return json_fail(jp, "shared_vision contract is incomplete");
    return true;
}

static bool manifest_validate_shared_vision(manifest_json_parser *jp,
                                            const ds4_hf_manifest_vision_metadata *vision) {
    if (strcmp(vision->image_token, "<｜image｜>") || vision->image_token_id != 129279 ||
        vision->image_size != 1024 || vision->encoder_dim != 896 ||
        vision->receiver_dim != 4096 || vision->tokens_per_view != 256 ||
        vision->separator_tokens != 1 || vision->minimum_views != 1 ||
        vision->maximum_views != 5 || strcmp(vision->token_formula, "views*256+1")) {
        return json_fail(jp, "shared_vision is incompatible with the DS4 DeepEncoderV2 contract");
    }
    if (vision->tile_limit != 4 || vision->tile_threshold_pixels != 1536 ||
        !vision->global_view_first || strcmp(vision->color_space, "RGB") ||
        strcmp(vision->crop_boundaries, "floor-proportional-v1") ||
        strcmp(vision->crop_order, "row-major") ||
        strcmp(vision->crop_count_rule, "zero-or-2-through-4") ||
        strcmp(vision->grid_selection, "closest-aspect-ratio") ||
        strcmp(vision->grid_tie_break, "more-tiles") ||
        strcmp(vision->resize, "1024x1024-bicubic") ||
        strcmp(vision->separator_placement, "last")) {
        return json_fail(jp, "shared_vision preprocessing contract is incompatible");
    }
    for (unsigned i = 0; i < 3; i++) {
        if (vision->mean[i] != 0.5 || vision->std[i] != 0.5) {
            return json_fail(jp, "shared_vision mean/std must be [0.5,0.5,0.5]");
        }
    }
    return true;
}

static bool manifest_parse_bf16_identity_hashes(
    manifest_json_parser *jp, char tower[DS4_HF_SHA256_HEX_SIZE],
    char projector[DS4_HF_SHA256_HEX_SIZE]) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        return json_fail(jp, "BF16 tensor identity side is empty");
    }
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "tower_sha256")) {
            if (!manifest_mark_field(jp, &seen, 1, key) ||
                !manifest_json_charge(jp) ||
                !manifest_json_string(jp, tower, DS4_HF_SHA256_HEX_SIZE)) {
                return false;
            }
        } else if (!strcmp(key, "projector_sha256")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_json_charge(jp) ||
                !manifest_json_string(jp, projector,
                                      DS4_HF_SHA256_HEX_SIZE)) {
                return false;
            }
        } else if (!manifest_json_skip_value(jp)) {
            return false;
        }
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (seen != 3) {
        return json_fail(jp, "BF16 tensor identity side is incomplete");
    }
    sha256_text_lower(tower);
    sha256_text_lower(projector);
    return true;
}

static bool manifest_parse_bf16_identity(
    manifest_json_parser *jp, ds4_hf_manifest_bf16_identity *identity) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        return json_fail(jp, "BF16 tensor identity record is empty");
    }
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "canonicalization")) {
            if (!manifest_mark_field(jp, &seen, 1, key) ||
                !manifest_json_charge(jp) ||
                !manifest_json_string(jp, identity->canonicalization,
                                      sizeof(identity->canonicalization))) {
                return false;
            }
        } else if (!strcmp(key, "source")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_parse_bf16_identity_hashes(
                    jp, identity->source_tower_sha256,
                    identity->source_projector_sha256)) return false;
        } else if (!strcmp(key, "llama_cpp_mmproj")) {
            if (!manifest_mark_field(jp, &seen, 4, key) ||
                !manifest_parse_bf16_identity_hashes(
                    jp, identity->mmproj_tower_sha256,
                    identity->mmproj_projector_sha256)) return false;
        } else if (!manifest_json_skip_value(jp)) {
            return false;
        }
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if (seen != 7) {
        return json_fail(jp, "BF16 tensor identity record is incomplete");
    }
    return true;
}

static bool manifest_parse_variant(manifest_json_parser *jp,
                                   ds4_hf_manifest_variant *variant) {
    if (!manifest_json_open(jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') return json_fail(jp, "variant record is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "selector")) {
            if (!manifest_mark_field(jp, &seen, 1, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, variant->selector, sizeof(variant->selector))) return false;
        } else if (!strcmp(key, "directory")) {
            if (!manifest_mark_field(jp, &seen, 2, key) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, variant->directory, sizeof(variant->directory))) return false;
        } else if (!strcmp(key, "default")) {
            if (!manifest_mark_field(jp, &seen, 4, key) || !manifest_json_bool(jp, &variant->is_default)) return false;
        } else if (!strcmp(key, "receiver")) {
            if (!manifest_mark_field(jp, &seen, 8, key) || !manifest_parse_artifact(jp, &variant->receiver)) return false;
        } else if (!strcmp(key, "ds4_vision")) {
            if (!manifest_mark_field(jp, &seen, 16, key) || !manifest_parse_vision_bundle(jp, &variant->ds4_vision)) return false;
        } else if (!strcmp(key, "llama_cpp_mmproj")) {
            if (!manifest_mark_field(jp, &seen, 32, key) ||
                !manifest_parse_artifact(jp, &variant->llama_cpp_mmproj)) return false;
        } else if (!strcmp(key, "bf16_tensor_identity")) {
            if (!manifest_mark_field(jp, &seen, 64, key) ||
                !manifest_parse_bf16_identity(
                    jp, &variant->bf16_tensor_identity)) return false;
        } else if (!strcmp(key, "dspark")) {
            if (!manifest_mark_field(jp, &seen, 128, key) ||
                !manifest_parse_artifact(jp, &variant->dspark)) return false;
            variant->has_dspark = true;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if ((seen & 127u) != 127u) {
        return json_fail(
            jp, "variant is missing selector, directory, default, receiver, "
                "ds4_vision, llama_cpp_mmproj, or bf16_tensor_identity");
    }
    return true;
}

static bool manifest_parse_variants(manifest_json_parser *jp, ds4_hf_manifest *manifest) {
    if (!manifest_json_open(jp, '[')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') return json_fail(jp, "manifest variants array is empty");
    bool more = true;
    while (more) {
        if (manifest->variant_count >= DS4_HF_MANIFEST_MAX_VARIANTS) {
            return json_fail(jp, "manifest exceeds the %u-variant limit",
                             DS4_HF_MANIFEST_MAX_VARIANTS);
        }
        if (!manifest_parse_variant(jp, &manifest->variants[manifest->variant_count++]) ||
            !manifest_json_next(jp, ']', &more)) return false;
    }
    return true;
}

static bool manifest_same_directory(const char *directory, const char *path) {
    size_t len = strlen(directory);
    return !strncmp(directory, path, len) && path[len] == '/' && path[len + 1] != '\0' &&
           strchr(path + len + 1, '/') == NULL;
}

static const char *manifest_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool manifest_split_gguf_name(const char *filename) {
    size_t len = strlen(filename);
    if (len <= 20) return false;
    const char *tail = filename + len - 20;
    if (tail[0] != '-' || memcmp(tail + 6, "-of-", 4) ||
        strcmp(tail + 15, ".gguf")) return false;
    for (unsigned i = 1; i <= 5; i++) if (!isdigit((unsigned char)tail[i])) return false;
    for (unsigned i = 10; i <= 14; i++) if (!isdigit((unsigned char)tail[i])) return false;
    return true;
}

static bool string_contains_case(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (!needle_len) return true;
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) ==
                   tolower((unsigned char)needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

bool ds4_hf_llama_primary_selectable(const char *selector,
                                     const char *receiver_path) {
    if (!selector || !selector[0] || !manifest_safe_path(receiver_path)) return false;
    const char *filename = manifest_basename(receiver_path);
    static const char *const excluded[] = {
        "mmproj", "imatrix", "mtp-", "eagle3-", "dflash-", "dspark-",
        "support",
    };
    if (!manifest_suffix(filename, ".gguf") || manifest_split_gguf_name(filename)) return false;
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        if (string_contains_case(filename, excluded[i])) return false;
    }
    size_t selector_len = strlen(selector);
    for (const char *candidate = filename; *candidate; candidate++) {
        size_t i = 0;
        while (i < selector_len && candidate[i] &&
               tolower((unsigned char)candidate[i]) ==
                   tolower((unsigned char)selector[i])) i++;
        if (i == selector_len && (candidate[i] == '.' || candidate[i] == '-')) return true;
    }
    return false;
}

static bool manifest_path_parent_equal(const char *left, const char *right) {
    const char *left_slash = strrchr(left, '/');
    const char *right_slash = strrchr(right, '/');
    if (!left_slash || !right_slash) return left_slash == right_slash;
    size_t left_len = (size_t)(left_slash - left);
    return left_len == (size_t)(right_slash - right) && !memcmp(left, right, left_len);
}

bool ds4_hf_llama_siblings_valid(const char *receiver_path,
                                 const char *mmproj_path,
                                 const char *dspark_path,
                                 char *err,
                                 size_t errlen) {
    if (!manifest_safe_path(receiver_path) || !manifest_safe_path(mmproj_path) ||
        (dspark_path && !manifest_safe_path(dspark_path))) {
        return fail(err, errlen, "llama.cpp sibling path is unsafe");
    }
    const char *receiver = manifest_basename(receiver_path);
    const char *mmproj = manifest_basename(mmproj_path);
    if (!manifest_suffix(receiver, ".gguf") || !manifest_suffix(mmproj, ".gguf") ||
        !strncmp(receiver, "mmproj-", 7) || !strncmp(receiver, "dspark-", 7) ||
        strncmp(mmproj, "mmproj-", 7) || !manifest_path_parent_equal(receiver_path, mmproj_path)) {
        return fail(err, errlen, "receiver and lowercase mmproj- companion do not satisfy llama.cpp sibling discovery");
    }
    if (dspark_path) {
        const char *dspark = manifest_basename(dspark_path);
        if (!manifest_suffix(dspark, ".gguf") || strncmp(dspark, "dspark-", 7) ||
            !manifest_path_parent_equal(receiver_path, dspark_path)) {
            return fail(err, errlen, "lowercase dspark- companion does not satisfy llama.cpp sibling discovery");
        }
    }
    return true;
}

bool ds4_hf_llama_gguf_metadata_valid(
    const ds4_hf_llama_gguf_metadata *metadata,
    char *err,
    size_t errlen) {
    if (!metadata) return fail(err, errlen, "llama.cpp GGUF metadata summary is required");
    if (strcmp(metadata->main_architecture, "deepseek4") ||
        strcmp(metadata->main_image_token, "<｜image｜>") ||
        metadata->main_image_token_id != 129279 || metadata->main_has_vision_tensors) {
        return fail(err, errlen,
                    "main GGUF must be an ordinary deepseek4 receiver with image token 129279 and no vision tensors");
    }
    if (metadata->has_dspark && strcmp(metadata->dspark_architecture, "deepseek4")) {
        return fail(err, errlen, "DSpark GGUF architecture must be deepseek4");
    }
    if (strcmp(metadata->mmproj_architecture, "clip") ||
        strcmp(metadata->mmproj_projector_type, "deepencoder_v2_dsv4") ||
        strcmp(metadata->mmproj_precision, "BF16") ||
        !metadata->mmproj_has_vision_encoder ||
        strcmp(metadata->mmproj_image_token, "<｜image｜>") ||
        metadata->mmproj_image_token_id != 129279 ||
        metadata->mmproj_image_size != 1024 ||
        metadata->mmproj_patch_size != 16 ||
        metadata->mmproj_embedding_length != 768 ||
        metadata->mmproj_encoder_dim != 896 ||
        metadata->mmproj_projection_dim != 4096 ||
        metadata->mmproj_tokens_per_view != 256 ||
        metadata->mmproj_separator_tokens != 1 ||
        metadata->mmproj_tile_limit != 4 ||
        metadata->mmproj_tile_threshold_pixels != 1536 ||
        !metadata->mmproj_global_view_first || !metadata->mmproj_separator_last ||
        strcmp(metadata->mmproj_color_space, "RGB") ||
        strcmp(metadata->mmproj_crop_boundaries, "floor-proportional-v1") ||
        strcmp(metadata->mmproj_crop_order, "row-major") ||
        strcmp(metadata->mmproj_crop_count_rule, "zero-or-2-through-4") ||
        strcmp(metadata->mmproj_grid_selection, "closest-aspect-ratio") ||
        strcmp(metadata->mmproj_grid_tie_break, "more-tiles") ||
        strcmp(metadata->mmproj_resize, "1024x1024-bicubic")) {
        return fail(err, errlen,
                    "mmproj GGUF metadata is incompatible with DeepEncoderV2 DS4 routing");
    }
    for (unsigned i = 0; i < 3; i++) {
        if (metadata->mmproj_image_mean[i] != 0.5 ||
            metadata->mmproj_image_std[i] != 0.5) {
            return fail(err, errlen,
                        "mmproj GGUF normalization metadata is incompatible with DeepEncoderV2");
        }
    }
    return true;
}

static bool manifest_validate_variant(manifest_json_parser *jp,
                                      const ds4_hf_manifest_variant *variant) {
    if (!manifest_safe_atom(variant->selector) || strchr(variant->selector, '+') ||
        !manifest_safe_path(variant->directory)) {
        return json_fail(jp, "variant selector or directory is unsafe");
    }
    if (!manifest_validate_artifact(jp, &variant->receiver, ARTIFACT_RECEIVER) ||
        !manifest_validate_artifact(jp, &variant->ds4_vision.tower, ARTIFACT_DS4_VISION) ||
        !manifest_validate_artifact(jp, &variant->ds4_vision.projector, ARTIFACT_DS4_VISION) ||
        !manifest_validate_artifact(jp, &variant->ds4_vision.config, ARTIFACT_DS4_VISION) ||
        !manifest_validate_artifact(jp, &variant->llama_cpp_mmproj, ARTIFACT_LLAMA_MMPROJ) ||
        (variant->has_dspark && !manifest_validate_artifact(jp, &variant->dspark, ARTIFACT_DSPARK))) return false;
    if (!manifest_suffix(variant->ds4_vision.tower.path, ".safetensors") ||
        !manifest_suffix(variant->ds4_vision.projector.path, ".safetensors") ||
        !manifest_suffix(variant->ds4_vision.config.path, ".json")) {
        return json_fail(jp, "DS4 vision bundle paths must be data-only safetensors/safetensors/JSON roles");
    }
    if (strcmp(variant->ds4_vision.tower.precision, "BF16") ||
        strcmp(variant->ds4_vision.tower.profile, "DeepEncoderV2")) {
        return json_fail(
            jp, "exact role ds4_vision.tower requires BF16 DeepEncoderV2 metadata");
    }
    if (strcmp(variant->ds4_vision.projector.precision, "BF16") ||
        strcmp(variant->ds4_vision.projector.profile, "896-to-4096")) {
        return json_fail(
            jp, "exact role ds4_vision.projector requires BF16 896-to-4096 metadata");
    }
    if (strcmp(variant->ds4_vision.config.precision, "JSON") ||
        strcmp(variant->ds4_vision.config.profile, "DeepEncoderV2")) {
        return json_fail(
            jp, "exact role ds4_vision.config requires JSON DeepEncoderV2 metadata");
    }
    const ds4_hf_manifest_bf16_identity *identity =
        &variant->bf16_tensor_identity;
    if (strcmp(variant->llama_cpp_mmproj.precision, "BF16") ||
        strcmp(identity->canonicalization,
               "named-tensor-name-shape-bf16le-v1") ||
        !manifest_valid_sha256(identity->source_tower_sha256) ||
        !manifest_valid_sha256(identity->source_projector_sha256) ||
        !manifest_valid_sha256(identity->mmproj_tower_sha256) ||
        !manifest_valid_sha256(identity->mmproj_projector_sha256)) {
        return json_fail(
            jp, "BF16 tensor identity contract is missing or malformed");
    }
    if (strcmp(identity->source_tower_sha256,
               identity->mmproj_tower_sha256) ||
        strcmp(identity->source_projector_sha256,
               identity->mmproj_projector_sha256)) {
        return json_fail(
            jp, "raw DS4 and llama.cpp mmproj BF16 tensor identities differ");
    }
    const char *paths[] = {
        variant->receiver.path, variant->ds4_vision.tower.path,
        variant->ds4_vision.projector.path, variant->ds4_vision.config.path,
        variant->llama_cpp_mmproj.path,
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (!manifest_same_directory(variant->directory, paths[i])) {
            return json_fail(jp, "variant '%s' artifact is outside its declared sibling directory", variant->selector);
        }
    }
    if (variant->has_dspark && !manifest_same_directory(variant->directory, variant->dspark.path)) {
        return json_fail(jp, "variant '%s' DSpark artifact is outside its sibling directory", variant->selector);
    }
    if (variant->has_dspark &&
        strcmp(variant->receiver.profile, variant->dspark.profile)) {
        return json_fail(jp,
                         "variant '%s' receiver and DSpark profiles differ",
                         variant->selector);
    }
    if (!ds4_hf_llama_primary_selectable(variant->selector,
                                         variant->receiver.path)) {
        return json_fail(jp,
                         "variant '%s' selector cannot select its primary GGUF under llama.cpp tag rules",
                         variant->selector);
    }
    char sibling_error[256];
    if (!ds4_hf_llama_siblings_valid(variant->receiver.path,
                                     variant->llama_cpp_mmproj.path,
                                     variant->has_dspark ? variant->dspark.path : NULL,
                                     sibling_error, sizeof(sibling_error))) {
        return json_fail(jp, "variant '%s': %s", variant->selector, sibling_error);
    }
    return true;
}

static bool manifest_valid_repository_part(const char *part, size_t len) {
    if (!len || len > 96 || part[0] == '.' || part[0] == '-' ||
        part[len - 1] == '.' || part[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)part[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) return false;
        if (i && ((c == '.' && part[i - 1] == '.') ||
                  (c == '-' && part[i - 1] == '-'))) return false;
    }
    return true;
}

static bool manifest_valid_repository(const char *repository) {
    const char *slash = strchr(repository, '/');
    if (!slash || slash == repository || !slash[1] || strchr(slash + 1, '/')) return false;
    return manifest_valid_repository_part(repository, (size_t)(slash - repository)) &&
           manifest_valid_repository_part(slash + 1, strlen(slash + 1));
}

bool ds4_hf_manifest_parse(const char *json,
                           size_t json_len,
                           ds4_hf_manifest *manifest,
                           char *err,
                           size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (!json || !manifest) return fail(err, errlen, "manifest input and output are required");
    if (!json_len) return fail(err, errlen, "manifest is empty");
    if (json_len > DS4_HF_MANIFEST_MAX_BYTES) {
        return fail(err, errlen, "manifest exceeds the %u-byte parser limit",
                    DS4_HF_MANIFEST_MAX_BYTES);
    }
    memset(manifest, 0, sizeof(*manifest));
    manifest_json_parser jp = {json, json + json_len, 0, 0, err, errlen};
    if (!manifest_json_open(&jp, '{')) return false;
    uint64_t seen = 0;
    manifest_json_ws(&jp);
    if (jp.p < jp.end && *jp.p == '}') return json_fail(&jp, "manifest object is empty");
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(&jp, key, sizeof(key))) return false;
        if (!strcmp(key, "schema_version")) {
            if (!manifest_mark_field(&jp, &seen, 1, key) ||
                !manifest_json_u32(&jp, &manifest->schema_version)) return false;
        } else if (!strcmp(key, "repository")) {
            if (!manifest_mark_field(&jp, &seen, 2, key) || !manifest_json_charge(&jp) ||
                !manifest_json_string(&jp, manifest->repository, sizeof(manifest->repository))) return false;
        } else if (!strcmp(key, "default_selector")) {
            if (!manifest_mark_field(&jp, &seen, 4, key) || !manifest_json_charge(&jp) ||
                !manifest_json_string(&jp, manifest->default_selector,
                                      sizeof(manifest->default_selector))) return false;
        } else if (!strcmp(key, "shared_vision")) {
            if (!manifest_mark_field(&jp, &seen, 8, key) ||
                !manifest_parse_shared_vision(&jp, &manifest->shared_vision)) return false;
        } else if (!strcmp(key, "variants")) {
            if (!manifest_mark_field(&jp, &seen, 16, key) ||
                !manifest_parse_variants(&jp, manifest)) return false;
        } else if (!manifest_json_skip_value(&jp)) return false;
        if (!manifest_json_next(&jp, '}', &more)) return false;
    }
    manifest_json_ws(&jp);
    if (jp.p != jp.end) return json_fail(&jp, "trailing data after manifest object");
    if (seen != 31) return json_fail(&jp, "manifest is missing a required top-level field");
    if (manifest->schema_version != DS4_HF_MANIFEST_VERSION) {
        return json_fail(&jp, "unsupported variants.json major version %u; this runtime supports version %u",
                         manifest->schema_version, DS4_HF_MANIFEST_VERSION);
    }
    if (!manifest_valid_repository(manifest->repository)) return json_fail(&jp, "manifest repository must be OWNER/REPO");
    if (!manifest_safe_atom(manifest->default_selector)) return json_fail(&jp, "manifest default_selector is unsafe");
    if (!manifest_validate_shared_vision(&jp, &manifest->shared_vision)) return false;

    size_t defaults = 0;
    for (size_t i = 0; i < manifest->variant_count; i++) {
        ds4_hf_manifest_variant *variant = &manifest->variants[i];
        for (size_t j = 0; j < i; j++) {
            if (ds4_hf_selector_equal(variant->selector, manifest->variants[j].selector)) {
                return json_fail(&jp, "duplicate selector '%s'", variant->selector);
            }
        }
        if (!manifest_validate_variant(&jp, variant)) return false;
        if (variant->is_default) {
            defaults++;
            if (!ds4_hf_selector_equal(variant->selector, manifest->default_selector)) {
                return json_fail(&jp, "variant default conflicts with default_selector '%s'",
                                 manifest->default_selector);
            }
        }
    }
    if (defaults != 1) return json_fail(&jp, "manifest must declare exactly one default variant");
    return true;
}

const ds4_hf_manifest_variant *ds4_hf_manifest_find_variant(
    const ds4_hf_manifest *manifest, const char *selector) {
    if (!manifest || !selector) return NULL;
    for (size_t i = 0; i < manifest->variant_count; i++) {
        if (ds4_hf_selector_equal(manifest->variants[i].selector, selector)) {
            return &manifest->variants[i];
        }
    }
    return NULL;
}

bool ds4_hf_manifest_visual_rows_valid(const ds4_hf_manifest *manifest,
                                       uint32_t rows) {
    if (!manifest || rows < manifest->shared_vision.separator_tokens) return false;
    uint32_t payload = rows - manifest->shared_vision.separator_tokens;
    if (!manifest->shared_vision.tokens_per_view ||
        payload % manifest->shared_vision.tokens_per_view) return false;
    uint32_t views = payload / manifest->shared_vision.tokens_per_view;
    if (strcmp(manifest->shared_vision.crop_count_rule,
               "zero-or-2-through-4")) return false;
    return (views == 1 || (views >= 3 && views <= 5)) &&
           views >= manifest->shared_vision.minimum_views &&
           views <= manifest->shared_vision.maximum_views;
}

bool ds4_hf_manifest_crop_bounds(uint32_t extent,
                                 uint32_t parts,
                                 uint32_t index,
                                 uint32_t *start,
                                 uint32_t *end) {
    if (!extent || !parts || index >= parts || !start || !end) return false;
    *start = (uint32_t)(((uint64_t)index * extent) / parts);
    *end = (uint32_t)(((uint64_t)(index + 1u) * extent) / parts);
    return true;
}

enum {
    HF_RESPONSE_MAX = 1024 * 1024,
    HF_TOKEN_MAX = 4096,
    HF_JSON_DEPTH_MAX = 64,
};

typedef struct {
    char *data;
    size_t len;
    bool too_large;
    char error_code[64];
} hf_http_response;

static void secure_clear(void *ptr, size_t len) {
    volatile unsigned char *p = ptr;
    while (len--) *p++ = 0;
}

static bool copy_string(char *dst, size_t dst_len, const char *src) {
    size_t len = src ? strlen(src) : 0;
    if (!dst_len || len >= dst_len) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static bool normalize_endpoint(const char *input, char *endpoint,
                               size_t endpoint_len) {
    const char *value = input && input[0] ? input : "https://huggingface.co";
    if (strncmp(value, "https://", 8) && strncmp(value, "http://", 7)) {
        return false;
    }
    const char *authority = strstr(value, "://") + 3;
    if (!authority[0] || strchr(authority, '?') || strchr(authority, '#')) {
        return false;
    }
    const char *path = strchr(authority, '/');
    const char *authority_end = path ? path : value + strlen(value);
    if (memchr(authority, '@', (size_t)(authority_end - authority))) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (iscntrl(*p) || isspace(*p)) return false;
    }
    size_t len = strlen(value);
    while (len && value[len - 1] == '/') len--;
    if (len == 0 || len >= endpoint_len) return false;
    memcpy(endpoint, value, len);
    endpoint[len] = '\0';
    return true;
}

static bool url_append(char *url, size_t url_len, size_t *used,
                       const char *text, size_t text_len) {
    if (*used >= url_len || text_len >= url_len - *used) return false;
    memcpy(url + *used, text, text_len);
    *used += text_len;
    url[*used] = '\0';
    return true;
}

static bool url_append_encoded(char *url, size_t url_len, size_t *used,
                               const char *value, bool keep_slashes) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        unsigned char c = *p;
        bool unreserved = isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved || (keep_slashes && c == '/')) {
            char one = (char)c;
            if (!url_append(url, url_len, used, &one, 1)) return false;
        } else {
            char encoded[3] = {'%', hex[c >> 4], hex[c & 15]};
            if (!url_append(url, url_len, used, encoded, sizeof(encoded))) return false;
        }
    }
    return true;
}

static bool safe_repo_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) return false;
    const char *part = path;
    for (const unsigned char *p = (const unsigned char *)path;; p++) {
        if (*p && iscntrl(*p)) return false;
        if (*p != '/' && *p != '\0') continue;
        size_t len = (size_t)((const char *)p - part);
        if (len == 0 || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!*p) return true;
        part = (const char *)p + 1;
    }
}

static bool valid_commit(const char *commit) {
    if (!commit || strlen(commit) != DS4_HF_COMMIT_SHA_LEN) return false;
    for (size_t i = 0; i < DS4_HF_COMMIT_SHA_LEN; i++) {
        if (!isxdigit((unsigned char)commit[i])) return false;
    }
    return true;
}

static bool safe_repo_id(const char *repo) {
    if (!repo) return false;
    const char *slash = strchr(repo, '/');
    if (!slash || slash == repo || !slash[1] || strchr(slash + 1, '/')) return false;
    const char *parts[] = {repo, slash + 1};
    const size_t lengths[] = {(size_t)(slash - repo), strlen(slash + 1)};
    for (size_t part = 0; part < 2; part++) {
        if ((lengths[part] == 1 && parts[part][0] == '.') ||
            (lengths[part] == 2 && parts[part][0] == '.' && parts[part][1] == '.')) {
            return false;
        }
        for (size_t i = 0; i < lengths[part]; i++) {
            unsigned char c = (unsigned char)parts[part][i];
            if (!isalnum(c) && c != '-' && c != '_' && c != '.') return false;
        }
    }
    return true;
}

bool ds4_hf_resolved_file_url(const ds4_hf_resolved_repo *resolved,
                              const char *repo_path,
                              char *url,
                              size_t url_len,
                              char *err,
                              size_t errlen) {
    if (!resolved || !url || !url_len || !resolved->endpoint[0] ||
        !resolved->repo[0] || !valid_commit(resolved->commit) ||
        !safe_repo_path(repo_path)) {
        return fail(err, errlen, "invalid immutable HF repository file request");
    }
    size_t used = 0;
    url[0] = '\0';
    if (!url_append(url, url_len, &used, resolved->endpoint,
                    strlen(resolved->endpoint)) ||
        !url_append(url, url_len, &used, "/", 1) ||
        !url_append_encoded(url, url_len, &used, resolved->repo, true) ||
        !url_append(url, url_len, &used, "/resolve/", 9) ||
        !url_append(url, url_len, &used, resolved->commit,
                    DS4_HF_COMMIT_SHA_LEN) ||
        !url_append(url, url_len, &used, "/", 1) ||
        !url_append_encoded(url, url_len, &used, repo_path, true)) {
        url[0] = '\0';
        return fail(err, errlen, "immutable HF repository file URL is too long");
    }
    return true;
}

static bool read_token_file(const char *path, char token[HF_TOKEN_MAX]) {
    if (!path || !path[0]) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    size_t len = fread(token, 1, HF_TOKEN_MAX - 1, fp);
    int extra = fgetc(fp);
    bool read_ok = !ferror(fp) && extra == EOF;
    fclose(fp);
    if (!read_ok) {
        secure_clear(token, HF_TOKEN_MAX);
        return false;
    }
    token[len] = '\0';
    size_t start = 0;
    while (start < len && isspace((unsigned char)token[start])) start++;
    while (len > start && isspace((unsigned char)token[len - 1])) len--;
    if (start) memmove(token, token + start, len - start);
    len -= start;
    token[len] = '\0';
    return len != 0;
}

static bool path_join(char *path, size_t path_len, const char *base,
                      const char *suffix) {
    if (!base || !base[0]) return false;
    int written = snprintf(path, path_len, "%s%s%s", base,
                           base[strlen(base) - 1] == '/' ? "" : "/", suffix);
    return written > 0 && (size_t)written < path_len;
}

static bool discover_token(const ds4_hf_cli_config *cfg,
                           char token[HF_TOKEN_MAX]) {
    memset(token, 0, HF_TOKEN_MAX);
    if (cfg->token && cfg->token[0]) return copy_string(token, HF_TOKEN_MAX, cfg->token);

    const char *token_path = getenv("HF_TOKEN_PATH");
    if (token_path && token_path[0]) return read_token_file(token_path, token);

    char path[DS4_HF_URL_MAX];
    const char *hf_home = getenv("HF_HOME");
    if (hf_home && path_join(path, sizeof(path), hf_home, "token")) {
        return read_token_file(path, token);
    }
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && path_join(path, sizeof(path), xdg, "huggingface/token")) {
        return read_token_file(path, token);
    }
    const char *home = getenv("HOME");
    if (home && path_join(path, sizeof(path), home, ".cache/huggingface/token")) {
        return read_token_file(path, token);
    }
    return false;
}

static size_t response_body_cb(char *data, size_t size, size_t count, void *userdata) {
    hf_http_response *response = userdata;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > HF_RESPONSE_MAX - response->len) {
        response->too_large = true;
        return 0;
    }
    memcpy(response->data + response->len, data, bytes);
    response->len += bytes;
    response->data[response->len] = '\0';
    return bytes;
}

static size_t response_header_cb(char *data, size_t size, size_t count, void *userdata) {
    hf_http_response *response = userdata;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    static const char header[] = "X-Error-Code:";
    if (bytes >= sizeof(header) - 1 &&
        !strncasecmp(data, header, sizeof(header) - 1)) {
        const char *start = data + sizeof(header) - 1;
        const char *end = data + bytes;
        while (start < end && isspace((unsigned char)*start)) start++;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        size_t len = (size_t)(end - start);
        if (len >= sizeof(response->error_code)) len = sizeof(response->error_code) - 1;
        memcpy(response->error_code, start, len);
        response->error_code[len] = '\0';
    }
    return bytes;
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *skip_json_string(const char *p, const char *end) {
    if (p == end || *p != '"') return NULL;
    for (p++; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL;
        if (c != '\\') continue;
        if (++p == end || !strchr("\"\\/bfnrtu", *p)) return NULL;
        if (*p == 'u') {
            for (int i = 0; i < 4; i++) {
                if (++p == end || !isxdigit((unsigned char)*p)) return NULL;
            }
        }
    }
    return NULL;
}

static const char *skip_json_value(const char *p, const char *end, unsigned depth);

static const char *skip_json_sequence(const char *p, const char *end,
                                      unsigned depth, char close, bool object) {
    p = skip_ws(p, end);
    if (p < end && *p == close) return p + 1;
    for (;;) {
        if (object) {
            p = skip_json_string(p, end);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p == end || *p++ != ':') return NULL;
        }
        p = skip_json_value(skip_ws(p, end), end, depth + 1);
        if (!p) return NULL;
        p = skip_ws(p, end);
        if (p == end) return NULL;
        if (*p == close) return p + 1;
        if (*p++ != ',') return NULL;
        p = skip_ws(p, end);
    }
}

static const char *skip_json_number(const char *p, const char *end) {
    if (p < end && *p == '-') p++;
    if (p == end) return NULL;
    if (*p == '0') p++;
    else {
        if (!isdigit((unsigned char)*p) || *p == '0') return NULL;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    if (p < end && *p == '.') {
        if (++p == end || !isdigit((unsigned char)*p)) return NULL;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-')) p++;
        if (p == end || !isdigit((unsigned char)*p)) return NULL;
        while (p < end && isdigit((unsigned char)*p)) p++;
    }
    return p;
}

static const char *skip_json_value(const char *p, const char *end, unsigned depth) {
    if (depth > HF_JSON_DEPTH_MAX || p == end) return NULL;
    if (*p == '"') return skip_json_string(p, end);
    if (*p == '{') return skip_json_sequence(p + 1, end, depth, '}', true);
    if (*p == '[') return skip_json_sequence(p + 1, end, depth, ']', false);
    if ((size_t)(end - p) >= 4 && !memcmp(p, "true", 4)) return p + 4;
    if ((size_t)(end - p) >= 5 && !memcmp(p, "false", 5)) return p + 5;
    if ((size_t)(end - p) >= 4 && !memcmp(p, "null", 4)) return p + 4;
    return skip_json_number(p, end);
}

static bool parse_commit_response(const char *json, size_t len,
                                  char commit[DS4_HF_COMMIT_SHA_LEN + 1]) {
    const char *p = skip_ws(json, json + len);
    const char *end = json + len;
    if (p == end || *p++ != '{') return false;
    p = skip_ws(p, end);
    bool found = false;
    if (p < end && *p == '}') p++;
    else for (;;) {
        const char *key = p;
        const char *key_end = skip_json_string(p, end);
        if (!key_end) return false;
        bool is_sha = key_end - key == 5 && !memcmp(key, "\"sha\"", 5);
        p = skip_ws(key_end, end);
        if (p == end || *p++ != ':') return false;
        p = skip_ws(p, end);
        if (is_sha) {
            const char *value_end = skip_json_string(p, end);
            if (!value_end || value_end - p != DS4_HF_COMMIT_SHA_LEN + 2) return false;
            memcpy(commit, p + 1, DS4_HF_COMMIT_SHA_LEN);
            commit[DS4_HF_COMMIT_SHA_LEN] = '\0';
            if (!valid_commit(commit)) return false;
            found = true;
            p = value_end;
        } else {
            p = skip_json_value(p, end, 0);
            if (!p) return false;
        }
        p = skip_ws(p, end);
        if (p == end) return false;
        if (*p == '}') { p++; break; }
        if (*p++ != ',') return false;
        p = skip_ws(p, end);
    }
    return found && skip_ws(p, end) == end;
}

static ds4_hf_resolve_status map_http_error(long http_status,
                                             const char *error_code,
                                             bool revision_request) {
    if (!strcmp(error_code, "GatedRepo") || !strcmp(error_code, "PrivateRepo") ||
        http_status == 403) return DS4_HF_RESOLVE_PRIVATE_OR_GATED;
    if (!strcmp(error_code, "RepoNotFound")) return DS4_HF_RESOLVE_REPOSITORY_NOT_FOUND;
    if (!strcmp(error_code, "RevisionNotFound")) return DS4_HF_RESOLVE_REVISION_NOT_FOUND;
    if (http_status == 401) return DS4_HF_RESOLVE_AUTHENTICATION_FAILED;
    if (http_status == 404) return revision_request ? DS4_HF_RESOLVE_REVISION_NOT_FOUND :
                                                     DS4_HF_RESOLVE_REPOSITORY_NOT_FOUND;
    return DS4_HF_RESOLVE_NETWORK_FAILED;
}

static ds4_hf_resolve_status request_commit(const char *url, const char *repo,
                                             const char *revision,
                                             const char *token, long timeout_ms,
                                             char commit[DS4_HF_COMMIT_SHA_LEN + 1],
                                             char *err, size_t errlen) {
    hf_http_response response = {0};
    response.data = malloc(HF_RESPONSE_MAX + 1);
    if (!response.data) {
        fail(err, errlen, "HF revision resolution ran out of memory");
        return DS4_HF_RESOLVE_NETWORK_FAILED;
    }
    response.data[0] = '\0';
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(response.data);
        fail(err, errlen, "HF transport initialization failed");
        return DS4_HF_RESOLVE_NETWORK_FAILED;
    }
    struct curl_slist *headers = NULL;
    char *authorization = NULL;
    if (token && token[0]) {
        size_t token_len = strlen(token);
        authorization = malloc(token_len + sizeof("Authorization: Bearer "));
        if (!authorization) {
            curl_easy_cleanup(curl);
            free(response.data);
            fail(err, errlen, "HF authentication setup ran out of memory");
            return DS4_HF_RESOLVE_NETWORK_FAILED;
        }
        snprintf(authorization, token_len + sizeof("Authorization: Bearer "),
                 "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
        secure_clear(authorization, token_len + sizeof("Authorization: Bearer "));
        free(authorization);
        if (!headers) {
            curl_easy_cleanup(curl);
            free(response.data);
            fail(err, errlen, "HF authentication setup ran out of memory");
            return DS4_HF_RESOLVE_NETWORK_FAILED;
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ds4-hf-resolver/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode curl_status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    ds4_hf_resolve_status status = DS4_HF_RESOLVE_OK;
    if (curl_status == CURLE_OPERATION_TIMEDOUT) {
        status = DS4_HF_RESOLVE_TIMEOUT;
        fail(err, errlen, "HF revision resolution timed out for repository '%s'", repo);
    } else if (curl_status != CURLE_OK || response.too_large) {
        status = response.too_large ? DS4_HF_RESOLVE_MALFORMED_RESPONSE :
                                      DS4_HF_RESOLVE_NETWORK_FAILED;
        fail(err, errlen, response.too_large ?
             "malformed HF API response for repository '%s': response is too large" :
             "HF network failure while resolving repository '%s'", repo);
    } else if (http_status != 200) {
        status = map_http_error(http_status, response.error_code, revision != NULL);
        switch (status) {
            case DS4_HF_RESOLVE_PRIVATE_OR_GATED:
                fail(err, errlen, "HF repository '%s' is private or gated", repo); break;
            case DS4_HF_RESOLVE_REPOSITORY_NOT_FOUND:
                fail(err, errlen, "HF repository '%s' was not found", repo); break;
            case DS4_HF_RESOLVE_REVISION_NOT_FOUND:
                fail(err, errlen, "HF revision '%s' was not found for repository '%s'",
                     revision ? revision : "default", repo); break;
            case DS4_HF_RESOLVE_AUTHENTICATION_FAILED:
                fail(err, errlen, "HF authentication failed for repository '%s'", repo); break;
            default:
                fail(err, errlen, "HF network failure while resolving repository '%s' (HTTP %ld)",
                     repo, http_status); break;
        }
    } else if (!parse_commit_response(response.data, response.len, commit)) {
        status = DS4_HF_RESOLVE_MALFORMED_RESPONSE;
        fail(err, errlen, "malformed HF API response for repository '%s': missing immutable commit SHA",
             repo);
    }
    free(response.data);
    return status;
}

static bool build_model_api_url(const char *endpoint, const char *repo,
                                const char *revision, char *url, size_t url_len) {
    size_t used = 0;
    url[0] = '\0';
    if (!url_append(url, url_len, &used, endpoint, strlen(endpoint)) ||
        !url_append(url, url_len, &used, "/api/models/", 12) ||
        !url_append_encoded(url, url_len, &used, repo, true)) return false;
    return !revision ||
           (url_append(url, url_len, &used, "/revision/", 10) &&
            url_append_encoded(url, url_len, &used, revision, false));
}

ds4_hf_resolve_status ds4_hf_resolve_repository(
    const ds4_hf_cli_config *cfg,
    long timeout_ms,
    ds4_hf_resolved_repo *resolved,
    char *err,
    size_t errlen) {
    if (resolved) memset(resolved, 0, sizeof(*resolved));
    if (!cfg || !resolved || !safe_repo_id(cfg->repo) ||
        (cfg->revision && (!visible_nonspace(cfg->revision) ||
                          !strcmp(cfg->revision, ".") ||
                          !strcmp(cfg->revision, "..")))) {
        fail(err, errlen, "invalid HF repository resolution request");
        return DS4_HF_RESOLVE_INVALID_ARGUMENT;
    }
    if (!normalize_endpoint(cfg->endpoint, resolved->endpoint,
                            sizeof(resolved->endpoint)) ||
        !copy_string(resolved->repo, sizeof(resolved->repo), cfg->repo)) {
        fail(err, errlen, "invalid HF endpoint or repository identifier");
        memset(resolved, 0, sizeof(*resolved));
        return DS4_HF_RESOLVE_INVALID_ARGUMENT;
    }
    if (timeout_ms <= 0) timeout_ms = 30000;

    char token[HF_TOKEN_MAX];
    bool have_token = discover_token(cfg, token);
    if (cfg->token && cfg->token[0] && !have_token) {
        fail(err, errlen, "HF credential is too long");
        memset(resolved, 0, sizeof(*resolved));
        return DS4_HF_RESOLVE_INVALID_ARGUMENT;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        secure_clear(token, sizeof(token));
        fail(err, errlen, "HF transport initialization failed");
        memset(resolved, 0, sizeof(*resolved));
        return DS4_HF_RESOLVE_NETWORK_FAILED;
    }

    char url[DS4_HF_URL_MAX];
    char ignored_commit[DS4_HF_COMMIT_SHA_LEN + 1];
    ds4_hf_resolve_status status = DS4_HF_RESOLVE_OK;
    if (cfg->revision && cfg->revision[0]) {
        if (!build_model_api_url(resolved->endpoint, resolved->repo, NULL,
                                 url, sizeof(url))) {
            status = DS4_HF_RESOLVE_INVALID_ARGUMENT;
            fail(err, errlen, "HF repository API URL is too long");
        } else {
            status = request_commit(url, resolved->repo, NULL,
                                    have_token ? token : NULL, timeout_ms,
                                    ignored_commit, err, errlen);
        }
    }
    if (status == DS4_HF_RESOLVE_OK) {
        if (!build_model_api_url(resolved->endpoint, resolved->repo,
                                 cfg->revision, url, sizeof(url))) {
            status = DS4_HF_RESOLVE_INVALID_ARGUMENT;
            fail(err, errlen, "HF revision API URL is too long");
        } else {
            status = request_commit(url, resolved->repo, cfg->revision,
                                    have_token ? token : NULL, timeout_ms,
                                    resolved->commit, err, errlen);
        }
    }
    secure_clear(token, sizeof(token));
    if (status != DS4_HF_RESOLVE_OK) memset(resolved, 0, sizeof(*resolved));
    return status;
}

const char *ds4_hf_resolve_status_name(ds4_hf_resolve_status status) {
    switch (status) {
        case DS4_HF_RESOLVE_OK: return "ok";
        case DS4_HF_RESOLVE_PRIVATE_OR_GATED: return "private_or_gated";
        case DS4_HF_RESOLVE_REPOSITORY_NOT_FOUND: return "repository_not_found";
        case DS4_HF_RESOLVE_REVISION_NOT_FOUND: return "revision_not_found";
        case DS4_HF_RESOLVE_AUTHENTICATION_FAILED: return "authentication_failed";
        case DS4_HF_RESOLVE_NETWORK_FAILED: return "network_failed";
        case DS4_HF_RESOLVE_TIMEOUT: return "timeout";
        case DS4_HF_RESOLVE_MALFORMED_RESPONSE: return "malformed_response";
        case DS4_HF_RESOLVE_INVALID_ARGUMENT: return "invalid_argument";
    }
    return "unknown";
}

const char *ds4_hf_artifact_role_name(ds4_hf_artifact_role role) {
    switch (role) {
        case DS4_HF_ROLE_RECEIVER: return "receiver";
        case DS4_HF_ROLE_VISION_TOWER: return "ds4_vision.tower";
        case DS4_HF_ROLE_VISION_PROJECTOR: return "ds4_vision.projector";
        case DS4_HF_ROLE_VISION_CONFIG: return "ds4_vision.config";
        case DS4_HF_ROLE_LLAMA_CPP_MMPROJ: return "llama_cpp_mmproj";
        case DS4_HF_ROLE_DSPARK: return "dspark";
    }
    return "unknown";
}

static bool manifest_download(const ds4_hf_cli_config *cfg,
                              const ds4_hf_resolved_repo *resolved,
                              char **json_out,
                              size_t *json_len_out,
                              char *err,
                              size_t errlen) {
    *json_out = NULL;
    *json_len_out = 0;
    if (cfg->offline) {
        return fail(err, errlen,
                    "HF offline manifest reuse is not available yet; provide network access or use --model with the verified cached receiver");
    }

    char url[DS4_HF_URL_MAX];
    if (!ds4_hf_resolved_file_url(resolved, "variants.json", url,
                                  sizeof(url), err, errlen)) return false;

    char token[HF_TOKEN_MAX];
    bool have_token = discover_token(cfg, token);
    if (cfg->token && cfg->token[0] && !have_token) {
        return fail(err, errlen, "HF credential is too long");
    }

    hf_http_response response = {0};
    response.data = malloc(HF_RESPONSE_MAX + 1u);
    if (!response.data) {
        secure_clear(token, sizeof(token));
        return fail(err, errlen, "HF manifest download ran out of memory");
    }
    response.data[0] = '\0';

    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    if (!curl) {
        free(response.data);
        secure_clear(token, sizeof(token));
        return fail(err, errlen, "HF manifest transport initialization failed");
    }
    if (have_token) {
        size_t token_len = strlen(token);
        char *authorization = malloc(token_len + sizeof("Authorization: Bearer "));
        if (!authorization) {
            curl_easy_cleanup(curl);
            free(response.data);
            secure_clear(token, sizeof(token));
            return fail(err, errlen, "HF manifest authentication setup ran out of memory");
        }
        snprintf(authorization, token_len + sizeof("Authorization: Bearer "),
                 "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
        secure_clear(authorization,
                     token_len + sizeof("Authorization: Bearer "));
        free(authorization);
        if (!headers) {
            curl_easy_cleanup(curl);
            free(response.data);
            secure_clear(token, sizeof(token));
            return fail(err, errlen, "HF manifest authentication setup ran out of memory");
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_body_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ds4-hf-resolver/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    CURLcode curl_status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    secure_clear(token, sizeof(token));

    if (curl_status != CURLE_OK || response.too_large ||
        response.len > DS4_HF_MANIFEST_MAX_BYTES || http_status != 200) {
        bool too_large = response.too_large ||
                         response.len > DS4_HF_MANIFEST_MAX_BYTES;
        free(response.data);
        if (too_large) {
            return fail(err, errlen,
                        "HF manifest exceeds the %u-byte bounded parser limit for repository '%s' revision '%s'",
                        DS4_HF_MANIFEST_MAX_BYTES, resolved->repo,
                        resolved->commit);
        }
        return fail(err, errlen,
                    "HF manifest download failed for repository '%s' revision '%s' (curl=%s, HTTP=%ld)",
                    resolved->repo, resolved->commit,
                    curl_easy_strerror(curl_status), http_status);
    }
    *json_out = response.data;
    *json_len_out = response.len;
    return true;
}

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t used;
} hf_sha256;

static uint32_t sha256_rotr(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static void sha256_transform(hf_sha256 *hash, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t words[64];
    for (unsigned i = 0; i < 16; i++) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(words[i - 15], 7) ^
                      sha256_rotr(words[i - 15], 18) ^
                      (words[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(words[i - 2], 17) ^
                      sha256_rotr(words[i - 2], 19) ^
                      (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = hash->state[0], b = hash->state[1];
    uint32_t c = hash->state[2], d = hash->state[3];
    uint32_t e = hash->state[4], f = hash->state[5];
    uint32_t g = hash->state[6], h = hash->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t sum1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^
                        sha256_rotr(e, 25);
        uint32_t choose = (e & f) ^ (~e & g);
        uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
        uint32_t sum0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^
                        sha256_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    hash->state[0] += a; hash->state[1] += b;
    hash->state[2] += c; hash->state[3] += d;
    hash->state[4] += e; hash->state[5] += f;
    hash->state[6] += g; hash->state[7] += h;
}

static void sha256_init(hf_sha256 *hash) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    memset(hash, 0, sizeof(*hash));
    memcpy(hash->state, initial, sizeof(initial));
}

static void sha256_update(hf_sha256 *hash, const void *data, size_t len) {
    const unsigned char *bytes = data;
    hash->bytes += len;
    while (len) {
        size_t space = sizeof(hash->block) - hash->used;
        size_t take = len < space ? len : space;
        memcpy(hash->block + hash->used, bytes, take);
        hash->used += take;
        bytes += take;
        len -= take;
        if (hash->used == sizeof(hash->block)) {
            sha256_transform(hash, hash->block);
            hash->used = 0;
        }
    }
}

static void sha256_final(hf_sha256 *hash, unsigned char digest[32]) {
    uint64_t bits = hash->bytes * UINT64_C(8);
    hash->block[hash->used++] = 0x80;
    if (hash->used > 56) {
        memset(hash->block + hash->used, 0, 64 - hash->used);
        sha256_transform(hash, hash->block);
        hash->used = 0;
    }
    memset(hash->block + hash->used, 0, 56 - hash->used);
    for (unsigned i = 0; i < 8; i++) {
        hash->block[63 - i] = (unsigned char)(bits >> (i * 8));
    }
    sha256_transform(hash, hash->block);
    for (unsigned i = 0; i < 8; i++) {
        digest[i * 4] = (unsigned char)(hash->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(hash->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(hash->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)hash->state[i];
    }
    secure_clear(hash, sizeof(*hash));
}

static void sha256_hex(const unsigned char digest[32],
                       char output[DS4_HF_SHA256_HEX_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 15];
    }
    output[64] = '\0';
}

static bool sha256_bounded_field(hf_sha256 *hash, const char *value,
                                 size_t capacity) {
    const char *end = memchr(value, '\0', capacity);
    if (!end) return false;
    size_t len = (size_t)(end - value);
    unsigned char encoded_len[8];
    for (unsigned i = 0; i < 8; i++) {
        encoded_len[7 - i] = (unsigned char)((uint64_t)len >> (i * 8));
    }
    sha256_update(hash, encoded_len, sizeof(encoded_len));
    sha256_update(hash, value, len);
    return true;
}

static void sha256_u64(hf_sha256 *hash, uint64_t value) {
    unsigned char encoded[8];
    for (unsigned i = 0; i < 8; i++) {
        encoded[7 - i] = (unsigned char)(value >> (i * 8));
    }
    sha256_update(hash, encoded, sizeof(encoded));
}

static bool acquisition_plan_seal(const ds4_hf_acquisition_plan *plan,
                                  char seal[DS4_HF_SHA256_HEX_SIZE]) {
    if (!plan || plan->artifact_count == 0 ||
        plan->artifact_count > DS4_HF_ACQUISITION_MAX_ARTIFACTS) return false;
    hf_sha256 hash;
    sha256_init(&hash);
    static const char domain[] = "ds4-hf-acquisition-plan-v1";
    sha256_update(&hash, domain, sizeof(domain));
    if (!sha256_bounded_field(&hash, plan->endpoint, sizeof(plan->endpoint)) ||
        !sha256_bounded_field(&hash, plan->repository,
                              sizeof(plan->repository)) ||
        !sha256_bounded_field(&hash, plan->revision,
                              sizeof(plan->revision)) ||
        !sha256_bounded_field(&hash, plan->selector,
                              sizeof(plan->selector)) ||
        !sha256_bounded_field(&hash, plan->cache_root,
                              sizeof(plan->cache_root))) return false;
    sha256_u64(&hash, (uint64_t)plan->artifact_count);
    for (size_t i = 0; i < plan->artifact_count; i++) {
        const ds4_hf_acquisition_artifact *artifact = &plan->artifacts[i];
        if (artifact->role < DS4_HF_ROLE_RECEIVER ||
            artifact->role > DS4_HF_ROLE_DSPARK || !artifact->bytes ||
            !memchr(artifact->sha256, '\0', sizeof(artifact->sha256)) ||
            !memchr(artifact->repo_path, '\0', sizeof(artifact->repo_path)) ||
            !manifest_valid_sha256(artifact->sha256) ||
            !safe_repo_path(artifact->repo_path)) return false;
        sha256_u64(&hash, (uint64_t)artifact->role);
        sha256_u64(&hash, artifact->requested ? 1 : 0);
        sha256_u64(&hash, artifact->bytes);
        if (!sha256_bounded_field(&hash, artifact->repo_path,
                                  sizeof(artifact->repo_path)) ||
            !sha256_bounded_field(&hash, artifact->sha256,
                                  sizeof(artifact->sha256)) ||
            !sha256_bounded_field(&hash, artifact->destination,
                                  sizeof(artifact->destination))) return false;
    }
    unsigned char digest[32];
    sha256_final(&hash, digest);
    sha256_hex(digest, seal);
    secure_clear(digest, sizeof(digest));
    return true;
}

static bool cache_component_encode(const char *value, char *encoded,
                                   size_t encoded_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '/') {
            if (used + 3 >= encoded_len) return false;
            encoded[used++] = '%';
            encoded[used++] = hex[*p >> 4];
            encoded[used++] = hex[*p & 15];
        } else {
            if (used + 1 >= encoded_len) return false;
            encoded[used++] = (char)*p;
        }
    }
    if (!used || used >= encoded_len) return false;
    encoded[used] = '\0';
    return true;
}

static bool cache_path(const char *base, const char *suffix,
                       char *out, size_t out_len) {
    if (!base || !base[0] || !suffix || !suffix[0]) return false;
    int written = snprintf(out, out_len, "%s%s%s", base,
                           base[strlen(base) - 1] == '/' ? "" : "/", suffix);
    return written > 0 && (size_t)written < out_len;
}

static bool cache_root_resolve(const ds4_hf_cli_config *cfg,
                               char root[DS4_HF_CACHE_PATH_MAX]) {
    const char *base = NULL;
    const char *suffix = NULL;
    if (cfg->cache_dir && cfg->cache_dir[0]) {
        base = cfg->cache_dir;
        suffix = NULL;
    } else if ((base = getenv("HF_HOME")) && base[0]) {
        suffix = "ds4";
    } else if ((base = getenv("XDG_CACHE_HOME")) && base[0]) {
        suffix = "huggingface/ds4";
    } else if ((base = getenv("HOME")) && base[0]) {
        suffix = ".cache/huggingface/ds4";
    } else {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)base; *p; p++) {
        if (iscntrl(*p)) return false;
    }
    bool ok = suffix ? cache_path(base, suffix, root, DS4_HF_CACHE_PATH_MAX) :
                       copy_string(root, DS4_HF_CACHE_PATH_MAX, base);
    if (!ok) return false;
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') root[--len] = '\0';
    return len != 0;
}

static bool plan_add_artifact(ds4_hf_acquisition_plan *plan,
                              ds4_hf_artifact_role role,
                              const ds4_hf_manifest_artifact *source,
                              bool requested, const char *repo_component) {
    if (plan->artifact_count >= DS4_HF_ACQUISITION_MAX_ARTIFACTS) return false;
    ds4_hf_acquisition_artifact *artifact =
        &plan->artifacts[plan->artifact_count++];
    memset(artifact, 0, sizeof(*artifact));
    artifact->role = role;
    artifact->requested = requested;
    artifact->bytes = source->bytes;
    if (!copy_string(artifact->repo_path, sizeof(artifact->repo_path),
                     source->path) ||
        !copy_string(artifact->sha256, sizeof(artifact->sha256),
                     source->sha256)) return false;
    sha256_text_lower(artifact->sha256);

    char snapshot_suffix[DS4_HF_CACHE_PATH_MAX];
    char snapshot[DS4_HF_CACHE_PATH_MAX];
    int written = snprintf(snapshot_suffix, sizeof(snapshot_suffix),
                           "repos/%s/snapshots/%s", repo_component,
                           plan->revision);
    return written > 0 && (size_t)written < sizeof(snapshot_suffix) &&
           cache_path(plan->cache_root, snapshot_suffix, snapshot,
                      sizeof(snapshot)) &&
           cache_path(snapshot, artifact->repo_path, artifact->destination,
                      sizeof(artifact->destination));
}

static bool acquisition_context_fail(
    char *err, size_t errlen, const ds4_hf_acquisition_plan *plan,
    const ds4_hf_acquisition_artifact *artifact, const char *fmt, ...) {
    char reason[DS4_HF_CACHE_PATH_MAX * 8 + 1024] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    return fail(err, errlen,
                "HF acquisition failed: repository='%s' revision='%s' "
                "selector='%s' role='%s' expected_size=%" PRIu64
                " destination='%s': %s",
                plan && plan->repository[0] ? plan->repository : "<unknown>",
                plan && plan->revision[0] ? plan->revision : "<unknown>",
                plan && plan->selector[0] ? plan->selector : "<unknown>",
                artifact ? ds4_hf_artifact_role_name(artifact->role) : "<unknown>",
                artifact ? artifact->bytes : UINT64_C(0),
                artifact && artifact->destination[0] ? artifact->destination :
                                                       "<unresolved>",
                reason);
}

bool ds4_hf_acquisition_plan_build(
    const ds4_hf_cli_config *cfg,
    const ds4_hf_resolved_repo *resolved,
    const ds4_hf_manifest *manifest,
    bool materialize_llama_cpp_mmproj,
    ds4_hf_acquisition_plan *plan,
    char *err,
    size_t errlen) {
    if (plan) memset(plan, 0, sizeof(*plan));
    if (!cfg || !resolved || !manifest || !plan ||
        !safe_repo_id(resolved->repo) || !valid_commit(resolved->commit) ||
        strcmp(manifest->repository, resolved->repo) ||
        !copy_string(plan->endpoint, sizeof(plan->endpoint), resolved->endpoint) ||
        !copy_string(plan->repository, sizeof(plan->repository), resolved->repo) ||
        !copy_string(plan->revision, sizeof(plan->revision), resolved->commit)) {
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        "invalid or mismatched plan inputs");
    }

    const ds4_hf_manifest_variant *variant = NULL;
    const char *selector = cfg->selector_set ? cfg->selector :
                                               manifest->default_selector;
    if (cfg->file) {
        for (size_t i = 0; i < manifest->variant_count; i++) {
            if (!strcmp(manifest->variants[i].receiver.path, cfg->file)) {
                variant = &manifest->variants[i];
                break;
            }
        }
        if (variant && cfg->selector_set &&
            !ds4_hf_selector_equal(variant->selector, selector)) variant = NULL;
    } else {
        variant = ds4_hf_manifest_find_variant(manifest, selector);
    }
    if (!variant || !copy_string(plan->selector, sizeof(plan->selector),
                                 variant->selector)) {
        copy_string(plan->selector, sizeof(plan->selector),
                    selector ? selector : "<unknown>");
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        cfg->file ?
                                        "exact file is not the selected catalog receiver" :
                                        "selector is not present in the catalog");
    }
    if (!cache_root_resolve(cfg, plan->cache_root)) {
        return acquisition_context_fail(
            err, errlen, plan, NULL,
            "cache root is unavailable; set --hf-cache-dir, HF_HOME, "
            "XDG_CACHE_HOME, or HOME");
    }

    char repo_component[DS4_HF_REPO_MAX + 4];
    if (!cache_component_encode(plan->repository, repo_component,
                                sizeof(repo_component)) ||
        !plan_add_artifact(plan, DS4_HF_ROLE_RECEIVER, &variant->receiver,
                           true, repo_component) ||
        !plan_add_artifact(plan, DS4_HF_ROLE_VISION_TOWER,
                           &variant->ds4_vision.tower,
                           cfg->vision_source == DS4_HF_VISION_CATALOG,
                           repo_component) ||
        !plan_add_artifact(plan, DS4_HF_ROLE_VISION_PROJECTOR,
                           &variant->ds4_vision.projector,
                           cfg->vision_source == DS4_HF_VISION_CATALOG,
                           repo_component) ||
        !plan_add_artifact(plan, DS4_HF_ROLE_VISION_CONFIG,
                           &variant->ds4_vision.config,
                           cfg->vision_source == DS4_HF_VISION_CATALOG,
                           repo_component) ||
        !plan_add_artifact(plan, DS4_HF_ROLE_LLAMA_CPP_MMPROJ,
                           &variant->llama_cpp_mmproj,
                           materialize_llama_cpp_mmproj, repo_component)) {
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        "cache destination is too long");
    }
    if (cfg->dspark_source == DS4_HF_DSPARK_CATALOG && !variant->has_dspark) {
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        "selected variant has no DSpark role");
    }
    if (variant->has_dspark &&
        !plan_add_artifact(plan, DS4_HF_ROLE_DSPARK, &variant->dspark,
                           cfg->dspark_source == DS4_HF_DSPARK_CATALOG,
                           repo_component)) {
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        "cache destination is too long");
    }
    if (!acquisition_plan_seal(plan, plan->integrity_seal)) {
        return acquisition_context_fail(err, errlen, plan, NULL,
                                        "cannot seal manifest-selected plan");
    }
    return true;
}

static bool mkdir_tree(const char *path) {
    char copy[DS4_HF_CACHE_PATH_MAX];
    if (!copy_string(copy, sizeof(copy), path)) return false;
    for (char *p = copy + (copy[0] == '/');; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        if (copy[0] && mkdir(copy, 0755) && errno != EEXIST) return false;
        struct stat st;
        if (copy[0] && (stat(copy, &st) || !S_ISDIR(st.st_mode))) return false;
        *p = saved;
        if (!saved) break;
    }
    return true;
}

static bool artifact_metadata(const ds4_hf_acquisition_plan *plan,
                              const ds4_hf_acquisition_artifact *artifact,
                              char *metadata, size_t metadata_len,
                              size_t *written_out) {
    int written = snprintf(
        metadata, metadata_len,
        "version=1\nrepository=%s\nrevision=%s\nselector=%s\nrole=%s\n"
        "expected_size=%" PRIu64 "\nsha256=%s\npath=%s\n",
        plan->repository, plan->revision, plan->selector,
        ds4_hf_artifact_role_name(artifact->role), artifact->bytes,
        artifact->sha256, artifact->repo_path);
    if (written < 0 || (size_t)written >= metadata_len) return false;
    *written_out = (size_t)written;
    return true;
}

static int cache_root_open(const ds4_hf_acquisition_plan *plan, bool create) {
    if (create && !mkdir_tree(plan->cache_root)) return -1;
    int fd = open(plan->cache_root,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) {
        int saved_errno = errno ? errno : ENOTDIR;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static bool safe_relative_component(const char *component) {
    return component[0] && strcmp(component, ".") && strcmp(component, "..");
}

static int directory_chain_open(int root_fd, const char *path, bool create) {
    char copy[DS4_HF_CACHE_PATH_MAX];
    if (!copy_string(copy, sizeof(copy), path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int current = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (current < 0) return -1;
    char *component = copy;
    while (*component) {
        char *slash = strchr(component, '/');
        if (slash) *slash = '\0';
        if (!safe_relative_component(component)) {
            close(current);
            errno = EINVAL;
            return -1;
        }
        if (create && mkdirat(current, component, 0700) && errno != EEXIST) {
            int saved_errno = errno;
            close(current);
            errno = saved_errno;
            return -1;
        }
        int next = openat(current, component,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            int saved_errno = errno;
            close(current);
            errno = saved_errno;
            return -1;
        }
        close(current);
        current = next;
        if (!slash) break;
        component = slash + 1;
    }
    return current;
}

static int artifact_parent_open(
    const ds4_hf_acquisition_plan *plan,
    const ds4_hf_acquisition_artifact *artifact, bool create,
    char leaf[DS4_HF_PATH_MAX]) {
    size_t root_len = strlen(plan->cache_root);
    if (strncmp(artifact->destination, plan->cache_root, root_len) ||
        artifact->destination[root_len] != '/' ||
        !artifact->destination[root_len + 1]) {
        errno = EINVAL;
        return -1;
    }
    const char *relative = artifact->destination + root_len + 1;
    const char *slash = strrchr(relative, '/');
    if (!slash || slash == relative || !safe_relative_component(slash + 1) ||
        !copy_string(leaf, DS4_HF_PATH_MAX, slash + 1)) {
        errno = EINVAL;
        return -1;
    }
    char parent[DS4_HF_CACHE_PATH_MAX];
    size_t parent_len = (size_t)(slash - relative);
    if (parent_len >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, relative, parent_len);
    parent[parent_len] = '\0';
    int root_fd = cache_root_open(plan, create);
    if (root_fd < 0) return -1;
    int parent_fd = directory_chain_open(root_fd, parent, create);
    int saved_errno = errno;
    close(root_fd);
    errno = saved_errno;
    return parent_fd;
}

static bool leaf_with_suffix(const char *leaf, const char *suffix,
                             char output[DS4_HF_PATH_MAX]) {
    int written = snprintf(output, DS4_HF_PATH_MAX, "%s%s", leaf, suffix);
    return written > 0 && written < DS4_HF_PATH_MAX;
}

static bool regular_entry_stat(int parent_fd, const char *leaf,
                               struct stat *st) {
    return !fstatat(parent_fd, leaf, st, AT_SYMLINK_NOFOLLOW) &&
           S_ISREG(st->st_mode) && st->st_nlink == 1 && st->st_size >= 0;
}

static int regular_entry_open(int parent_fd, const char *leaf, int flags,
                              mode_t mode) {
    int fd = openat(parent_fd, leaf, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static bool hash_regular_fd_stable(int parent_fd, const char *leaf, int fd,
                                   uint64_t expected_size,
                                   const char *expected_sha256,
                                   char actual_sha256[DS4_HF_SHA256_HEX_SIZE]) {
    struct stat before, after, path_stat;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) ||
        before.st_nlink != 1 || before.st_size < 0 ||
        (uint64_t)before.st_size != expected_size) return false;
    hf_sha256 hash;
    sha256_init(&hash);
    unsigned char buffer[64 * 1024];
    uint64_t offset = 0;
    while (offset < expected_size) {
        size_t wanted = expected_size - offset < sizeof(buffer) ?
                        (size_t)(expected_size - offset) : sizeof(buffer);
        ssize_t got = pread(fd, buffer, wanted, (off_t)offset);
        if (got < 0) {
            if (errno == EINTR) continue;
            secure_clear(&hash, sizeof(hash));
            return false;
        }
        if (!got) {
            secure_clear(&hash, sizeof(hash));
            return false;
        }
        sha256_update(&hash, buffer, (size_t)got);
        offset += (uint64_t)got;
    }
    unsigned char digest[32];
    sha256_final(&hash, digest);
    sha256_hex(digest, actual_sha256);
    secure_clear(digest, sizeof(digest));
    if (fstat(fd, &after) ||
        fstatat(parent_fd, leaf, &path_stat, AT_SYMLINK_NOFOLLOW) ||
        !S_ISREG(path_stat.st_mode) || path_stat.st_nlink != 1 ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        after.st_dev != path_stat.st_dev || after.st_ino != path_stat.st_ino ||
        after.st_size != path_stat.st_size) return false;
    return !strcmp(actual_sha256, expected_sha256);
}

static bool metadata_fd_matches(const ds4_hf_acquisition_plan *plan,
                                const ds4_hf_acquisition_artifact *artifact,
                                int fd) {
    char expected[1024], actual[1025];
    size_t expected_len = 0;
    if (!artifact_metadata(plan, artifact, expected, sizeof(expected),
                           &expected_len)) return false;
    struct stat before, after;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) ||
        before.st_nlink != 1 || before.st_size < 0 ||
        (uint64_t)before.st_size != expected_len) return false;
    ssize_t got;
    do {
        got = pread(fd, actual, sizeof(actual), 0);
    } while (got < 0 && errno == EINTR);
    return got >= 0 && (size_t)got == expected_len &&
           !memcmp(actual, expected, expected_len) && !fstat(fd, &after) &&
           before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size;
}

typedef enum {
    ARTIFACT_CACHE_MISSING,
    ARTIFACT_CACHE_VALID,
    ARTIFACT_CACHE_INVALID,
} artifact_cache_state;

static artifact_cache_state artifact_cache_open(
    const ds4_hf_acquisition_plan *plan,
    const ds4_hf_acquisition_artifact *artifact, int *artifact_fd_out) {
    *artifact_fd_out = -1;
    char leaf[DS4_HF_PATH_MAX], metadata_leaf[DS4_HF_PATH_MAX];
    int parent_fd = artifact_parent_open(plan, artifact, false, leaf);
    if (parent_fd < 0) return errno == ENOENT ? ARTIFACT_CACHE_MISSING :
                                               ARTIFACT_CACHE_INVALID;
    if (!leaf_with_suffix(leaf, ".ds4-meta", metadata_leaf)) {
        close(parent_fd);
        return ARTIFACT_CACHE_INVALID;
    }
    struct stat artifact_stat, metadata_stat;
    bool artifact_exists = !fstatat(parent_fd, leaf, &artifact_stat,
                                    AT_SYMLINK_NOFOLLOW);
    int artifact_errno = errno;
    bool metadata_exists = !fstatat(parent_fd, metadata_leaf, &metadata_stat,
                                    AT_SYMLINK_NOFOLLOW);
    int metadata_errno = errno;
    if (!artifact_exists && !metadata_exists && artifact_errno == ENOENT &&
        metadata_errno == ENOENT) {
        close(parent_fd);
        return ARTIFACT_CACHE_MISSING;
    }
    if (!artifact_exists || !metadata_exists ||
        !S_ISREG(artifact_stat.st_mode) || artifact_stat.st_nlink != 1 ||
        !S_ISREG(metadata_stat.st_mode) || metadata_stat.st_nlink != 1) {
        close(parent_fd);
        return ARTIFACT_CACHE_INVALID;
    }
    int metadata_fd = regular_entry_open(parent_fd, metadata_leaf, O_RDONLY, 0);
    int artifact_fd = regular_entry_open(parent_fd, leaf, O_RDONLY, 0);
    char actual_sha256[DS4_HF_SHA256_HEX_SIZE] = {0};
    bool ok = metadata_fd >= 0 && artifact_fd >= 0 &&
              metadata_fd_matches(plan, artifact, metadata_fd) &&
              hash_regular_fd_stable(parent_fd, leaf, artifact_fd,
                                     artifact->bytes, artifact->sha256,
                                     actual_sha256);
    if (metadata_fd >= 0) close(metadata_fd);
    close(parent_fd);
    if (!ok) {
        if (artifact_fd >= 0) close(artifact_fd);
        return ARTIFACT_CACHE_INVALID;
    }
    *artifact_fd_out = artifact_fd;
    return ARTIFACT_CACHE_VALID;
}

static bool write_all(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t wrote = write(fd, data, len);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += (size_t)wrote;
        len -= (size_t)wrote;
    }
    return true;
}

typedef struct {
    FILE *file;
    uint64_t remaining;
} hf_artifact_writer;

static size_t artifact_write_cb(char *data, size_t size, size_t count,
                                void *userdata) {
    hf_artifact_writer *writer = userdata;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if ((uint64_t)bytes > writer->remaining) return 0;
    size_t wrote = fwrite(data, 1, bytes, writer->file);
    writer->remaining -= wrote;
    return wrote;
}

static CURLcode artifact_download_once(
    const ds4_hf_acquisition_plan *plan,
    const ds4_hf_acquisition_artifact *artifact, int parent_fd,
    const char *part_leaf,
    const char *token, uint64_t offset, long timeout_ms, long *http_status) {
    int flags = O_WRONLY | O_CREAT | (offset ? O_APPEND : O_TRUNC);
    int fd = regular_entry_open(parent_fd, part_leaf, flags, 0600);
    if (fd < 0) return CURLE_WRITE_ERROR;
    FILE *file = fdopen(fd, offset ? "ab" : "wb");
    if (!file) {
        close(fd);
        return CURLE_WRITE_ERROR;
    }

    ds4_hf_resolved_repo resolved = {0};
    copy_string(resolved.endpoint, sizeof(resolved.endpoint), plan->endpoint);
    copy_string(resolved.repo, sizeof(resolved.repo), plan->repository);
    copy_string(resolved.commit, sizeof(resolved.commit), plan->revision);
    char url[DS4_HF_URL_MAX];
    char ignored[1];
    if (!ds4_hf_resolved_file_url(&resolved, artifact->repo_path, url,
                                  sizeof(url), ignored, sizeof(ignored))) {
        fclose(file);
        return CURLE_URL_MALFORMAT;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        return CURLE_FAILED_INIT;
    }
    struct curl_slist *headers = NULL;
    char *authorization = NULL;
    if (token && token[0]) {
        size_t token_len = strlen(token);
        authorization = malloc(token_len + sizeof("Authorization: Bearer "));
        if (!authorization) {
            curl_easy_cleanup(curl);
            fclose(file);
            return CURLE_OUT_OF_MEMORY;
        }
        snprintf(authorization, token_len + sizeof("Authorization: Bearer "),
                 "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
        secure_clear(authorization, token_len + sizeof("Authorization: Bearer "));
        free(authorization);
        if (!headers) {
            curl_easy_cleanup(curl);
            fclose(file);
            return CURLE_OUT_OF_MEMORY;
        }
    }
    hf_artifact_writer writer = {file, artifact->bytes - offset};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, artifact_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ds4-hf-resolver/1");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
    if (timeout_ms > 0) curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    if (offset) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                                 (curl_off_t)offset);
    CURLcode status = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    if (fflush(file) || fsync(fd)) status = CURLE_WRITE_ERROR;
    if (fclose(file) && status == CURLE_OK) status = CURLE_WRITE_ERROR;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

static bool shell_quote(const char *value, char *output, size_t output_len) {
    size_t used = 0;
    if (output_len < 3) return false;
    output[used++] = '\'';
    for (const char *p = value; *p; p++) {
        static const char escaped_quote[] = "'\\''";
        if (*p == '\'') {
            if (sizeof(escaped_quote) - 1 >= output_len - used) return false;
            memcpy(output + used, escaped_quote, sizeof(escaped_quote) - 1);
            used += sizeof(escaped_quote) - 1;
        } else {
            if (used + 1 >= output_len) return false;
            output[used++] = *p;
        }
    }
    if (used + 2 > output_len) return false;
    output[used++] = '\'';
    output[used] = '\0';
    return true;
}

static bool untrusted_entry_fail(
    char *err, size_t errlen, const ds4_hf_acquisition_plan *plan,
    const ds4_hf_acquisition_artifact *artifact, const char *path,
    bool complete_cache_entry, const char *reason) {
    char quoted[DS4_HF_CACHE_PATH_MAX * 4 + 3];
    char quarantine_template[DS4_HF_CACHE_PATH_MAX + 32];
    char quoted_template[(DS4_HF_CACHE_PATH_MAX + 32) * 4 + 3];
    int template_written = snprintf(quarantine_template,
                                    sizeof(quarantine_template),
                                    "%s.untrusted.XXXXXX", path);
    if (template_written <= 0 ||
        (size_t)template_written >= sizeof(quarantine_template) ||
        !shell_quote(path, quoted, sizeof(quoted)) ||
        !shell_quote(quarantine_template, quoted_template,
                     sizeof(quoted_template))) {
        return acquisition_context_fail(
            err, errlen, plan, artifact,
            "%s; move the untrusted entry aside without deleting it and retry",
            reason);
    }
    if (!complete_cache_entry) {
        return acquisition_context_fail(
            err, errlen, plan, artifact,
            "%s; non-destructive recovery command: "
            "q=$(mktemp -d %s) && mv -- %s \"$q/\"",
            reason, quoted_template, quoted);
    }
    return acquisition_context_fail(
        err, errlen, plan, artifact,
        "%s; non-destructive recovery command: "
        "p=%s; q=$(mktemp -d %s) && "
        "for s in '' .ds4-meta .part .ds4-meta.part; do "
        "if [ -e \"$p$s\" ] || [ -L \"$p$s\" ]; then "
        "mv -- \"$p$s\" \"$q/\" || exit; fi; done",
        reason, quoted, quoted_template);
}

static bool acquisition_plan_valid(const ds4_hf_acquisition_plan *plan) {
    char actual[DS4_HF_SHA256_HEX_SIZE];
    return acquisition_plan_seal(plan, actual) &&
           memchr(plan->integrity_seal, '\0',
                  sizeof(plan->integrity_seal)) != NULL &&
           manifest_valid_sha256(plan->integrity_seal) &&
           !strcmp(actual, plan->integrity_seal);
}

static bool acquire_one(const ds4_hf_cli_config *cfg,
                        ds4_hf_acquisition_plan *plan,
                        ds4_hf_acquisition_artifact *artifact,
                        const char *token, long timeout_ms,
                        char *err, size_t errlen) {
    char leaf[DS4_HF_PATH_MAX], lock_leaf[DS4_HF_PATH_MAX];
    char part_leaf[DS4_HF_PATH_MAX], metadata_leaf[DS4_HF_PATH_MAX];
    char metadata_tmp_leaf[DS4_HF_PATH_MAX];
    int parent_fd = artifact_parent_open(plan, artifact, true, leaf);
    if (parent_fd < 0 ||
        !leaf_with_suffix(leaf, ".lock", lock_leaf) ||
        !leaf_with_suffix(leaf, ".part", part_leaf) ||
        !leaf_with_suffix(leaf, ".ds4-meta", metadata_leaf) ||
        !leaf_with_suffix(leaf, ".ds4-meta.part", metadata_tmp_leaf)) {
        if (parent_fd >= 0) close(parent_fd);
        return acquisition_context_fail(
            err, errlen, plan, artifact,
            "cannot prepare descriptor-anchored cache directories without symlinks: %s",
            strerror(errno));
    }
    int lock_fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        lock_fd = regular_entry_open(parent_fd, lock_leaf,
                                     O_RDWR | O_CREAT, 0600);
        if (lock_fd >= 0 || errno != ENOENT) break;
        sched_yield();
    }
    if (lock_fd < 0) {
        int saved_errno = errno;
        close(parent_fd);
        return acquisition_context_fail(err, errlen, plan, artifact,
                                        "cannot open regular no-follow cache lock: %s",
                                        strerror(saved_errno));
    }
    if (flock(lock_fd, LOCK_EX)) {
        int saved_errno = errno;
        close(lock_fd);
        close(parent_fd);
        return acquisition_context_fail(err, errlen, plan, artifact,
                                        "cannot acquire cache lock: %s",
                                        strerror(saved_errno));
    }
    bool ok = false;
    int cached_fd = -1;
    artifact_cache_state cache_state = artifact_cache_open(plan, artifact,
                                                           &cached_fd);
    if (cache_state == ARTIFACT_CACHE_VALID) {
        close(cached_fd);
        artifact->cache_hit = true;
        ok = true;
        goto done;
    }
    if (cache_state == ARTIFACT_CACHE_INVALID) {
        untrusted_entry_fail(
            err, errlen, plan, artifact, artifact->destination, true,
            "cache entry fails manifest role, byte-count, SHA-256, sidecar, or stable-path verification; no same-directory fallback is permitted");
        goto done;
    }
    struct stat transaction_stat;
    if (!fstatat(parent_fd, metadata_tmp_leaf, &transaction_stat,
                 AT_SYMLINK_NOFOLLOW)) {
        untrusted_entry_fail(
            err, errlen, plan, artifact, artifact->destination, true,
            "interrupted cache publication left transaction members that must be quarantined before retry");
        goto done;
    }
    if (errno != ENOENT) {
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "cannot inspect interrupted cache publication metadata: %s",
            strerror(errno));
        goto done;
    }
    if (cfg->offline) {
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "offline mode forbids the required network transfer");
        goto done;
    }
    if (artifact->bytes > (uint64_t)INT64_MAX) {
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "expected size exceeds the signed 64-bit range supported by libcurl");
        goto done;
    }

    uint64_t offset = 0;
    struct stat part_stat;
    if (!fstatat(parent_fd, part_leaf, &part_stat, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(part_stat.st_mode) || part_stat.st_nlink != 1 ||
            part_stat.st_size < 0 ||
            (uint64_t)part_stat.st_size > artifact->bytes) {
            char part_path[DS4_HF_CACHE_PATH_MAX + 8];
            snprintf(part_path, sizeof(part_path), "%s.part",
                     artifact->destination);
            untrusted_entry_fail(
                err, errlen, plan, artifact, part_path, false,
                "resume entry is a symlink, special/hard-linked file, or has an impossible size");
            goto done;
        }
        int part_fd = regular_entry_open(parent_fd, part_leaf, O_RDONLY, 0);
        if (part_fd < 0 || fstat(part_fd, &part_stat)) {
            if (part_fd >= 0) close(part_fd);
            acquisition_context_fail(err, errlen, plan, artifact,
                                     "resume entry changed during inspection");
            goto done;
        }
        close(part_fd);
        offset = (uint64_t)part_stat.st_size;
    } else if (errno != ENOENT) {
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "cannot inspect resume entry: %s",
                                 strerror(errno));
        goto done;
    }

    CURLcode curl_status = CURLE_OK;
    long http_status = 0;
    if (offset < artifact->bytes) {
        curl_status = artifact_download_once(plan, artifact, parent_fd,
                                             part_leaf, token, offset,
                                             timeout_ms, &http_status);
        if (offset && (http_status == 200 ||
                       curl_status == CURLE_RANGE_ERROR)) {
            http_status = 0;
            curl_status = artifact_download_once(plan, artifact, parent_fd,
                                                 part_leaf, token, 0,
                                                 timeout_ms, &http_status);
        }
    }
    struct stat downloaded_stat;
    uint64_t downloaded = 0;
    if (regular_entry_stat(parent_fd, part_leaf, &downloaded_stat)) {
        downloaded = (uint64_t)downloaded_stat.st_size;
    }
    if (curl_status != CURLE_OK || downloaded != artifact->bytes) {
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "libcurl HTTP(S) redirect/range transfer failed (curl=%s, HTTP=%ld, "
            "partial_size=%" PRIu64 "); HF LFS/Xet requires libcurl with "
            "HTTP(S), redirects, and byte-range support",
            curl_easy_strerror(curl_status), http_status, downloaded);
        goto done;
    }

    int part_fd = regular_entry_open(parent_fd, part_leaf, O_RDONLY, 0);
    char actual_sha256[DS4_HF_SHA256_HEX_SIZE] = {0};
    if (part_fd < 0 ||
        !hash_regular_fd_stable(parent_fd, part_leaf, part_fd,
                                artifact->bytes, artifact->sha256,
                                actual_sha256)) {
        if (part_fd >= 0) close(part_fd);
        char part_path[DS4_HF_CACHE_PATH_MAX + 8];
        snprintf(part_path, sizeof(part_path), "%s.part",
                 artifact->destination);
        untrusted_entry_fail(
            err, errlen, plan, artifact, part_path, false,
            "downloaded bytes fail manifest SHA-256 or stable-path verification");
        goto done;
    }
    if (fchmod(part_fd, 0400)) {
        int saved_errno = errno;
        close(part_fd);
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "cannot make verified partial read-only: %s",
                                 strerror(saved_errno));
        goto done;
    }
    close(part_fd);

    char metadata[1024];
    size_t metadata_len = 0;
    if (!artifact_metadata(plan, artifact, metadata, sizeof(metadata),
                           &metadata_len)) {
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "immutable cache metadata is too long");
        goto done;
    }
    int metadata_fd = regular_entry_open(
        parent_fd, metadata_tmp_leaf,
        O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (metadata_fd < 0) {
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "cannot create exclusive no-follow cache metadata: %s",
            strerror(errno));
        goto done;
    }
    bool metadata_written = write_all(metadata_fd, metadata, metadata_len) &&
                            !fchmod(metadata_fd, 0400) &&
                            !fsync(metadata_fd);
    int metadata_errno = errno;
    if (close(metadata_fd) && metadata_written) {
        metadata_written = false;
        metadata_errno = errno;
    }
    if (!metadata_written) {
        unlinkat(parent_fd, metadata_tmp_leaf, 0);
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "cannot write immutable cache metadata: %s",
                                 strerror(metadata_errno));
        goto done;
    }
    if (linkat(parent_fd, metadata_tmp_leaf, parent_fd, metadata_leaf, 0)) {
        int saved_errno = errno;
        unlinkat(parent_fd, metadata_tmp_leaf, 0);
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "cannot exclusively publish immutable cache metadata: %s",
            strerror(saved_errno));
        goto done;
    }
    if (linkat(parent_fd, part_leaf, parent_fd, leaf, 0)) {
        int saved_errno = errno;
        unlinkat(parent_fd, metadata_leaf, 0);
        unlinkat(parent_fd, metadata_tmp_leaf, 0);
        acquisition_context_fail(
            err, errlen, plan, artifact,
            "cannot exclusively publish verified cache entry: %s",
            strerror(saved_errno));
        goto done;
    }
    unlinkat(parent_fd, part_leaf, 0);
    unlinkat(parent_fd, metadata_tmp_leaf, 0);
    if (fsync(parent_fd)) {
        acquisition_context_fail(err, errlen, plan, artifact,
                                 "cannot durably publish verified cache entry: %s",
                                 strerror(errno));
        goto done;
    }
    cached_fd = -1;
    if (artifact_cache_open(plan, artifact, &cached_fd) !=
        ARTIFACT_CACHE_VALID) {
        if (cached_fd >= 0) close(cached_fd);
        untrusted_entry_fail(
            err, errlen, plan, artifact, artifact->destination, true,
            "published entry changed before final integrity handoff");
        goto done;
    }
    close(cached_fd);
    artifact->cache_hit = false;
    ok = true;

done:
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(parent_fd);
    return ok;
}

bool ds4_hf_acquisition_open_verified(
    const ds4_hf_acquisition_plan *plan,
    size_t artifact_index,
    int *fd_out,
    char *err,
    size_t errlen) {
    if (fd_out) *fd_out = -1;
    if (!plan || !fd_out || !acquisition_plan_valid(plan)) {
        return fail(err, errlen,
                    "HF verified open rejected an invalid or manifest-mutated sealed plan");
    }
    if (artifact_index >= plan->artifact_count ||
        !plan->artifacts[artifact_index].requested) {
        return fail(err, errlen,
                    "HF verified open requires a requested artifact role from the sealed plan");
    }
    const ds4_hf_acquisition_artifact *artifact =
        &plan->artifacts[artifact_index];
    artifact_cache_state state = artifact_cache_open(plan, artifact, fd_out);
    if (state == ARTIFACT_CACHE_VALID) return true;
    if (state == ARTIFACT_CACHE_MISSING) {
        return acquisition_context_fail(
            err, errlen, plan, artifact,
            "verified cache artifact is missing; no same-directory fallback is permitted");
    }
    return untrusted_entry_fail(
        err, errlen, plan, artifact, artifact->destination, true,
        "cache entry fails manifest role, byte-count, SHA-256, sidecar, or stable-path verification; no same-directory fallback is permitted");
}

bool ds4_hf_acquisition_execute(const ds4_hf_cli_config *cfg,
                                ds4_hf_acquisition_plan *plan,
                                long timeout_ms,
                                char *err,
                                size_t errlen) {
    if (!cfg || !plan || !acquisition_plan_valid(plan)) {
        return fail(err, errlen,
                    "HF acquisition rejected an invalid or manifest-mutated sealed plan");
    }
    ds4_hf_acquisition_artifact *first = NULL;
    for (size_t i = 0; i < plan->artifact_count; i++) {
        if (plan->artifacts[i].requested) {
            first = &plan->artifacts[i];
            break;
        }
    }
    if (!first) return true;

    char token[HF_TOKEN_MAX];
    bool have_token = discover_token(cfg, token);
    if (cfg->token && cfg->token[0] && !have_token) {
        return acquisition_context_fail(err, errlen, plan, first,
                                        "HF credential exceeds the supported in-memory limit");
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        secure_clear(token, sizeof(token));
        return acquisition_context_fail(
            err, errlen, plan, first,
            "libcurl initialization failed; HF LFS/Xet requires libcurl with "
            "HTTP(S), redirects, and byte-range support");
    }

    bool ok = true;
    for (size_t i = 0; i < plan->artifact_count; i++) {
        if (!plan->artifacts[i].requested) continue;
        if (!acquire_one(cfg, plan, &plan->artifacts[i],
                         have_token ? token : NULL, timeout_ms,
                         err, errlen)) {
            ok = false;
            break;
        }
    }
    secure_clear(token, sizeof(token));
    return ok;
}

bool ds4_hf_acquisition_probe_cache(ds4_hf_acquisition_plan *plan,
                                    bool require_requested,
                                    char *err,
                                    size_t errlen) {
    if (!plan || !acquisition_plan_valid(plan)) {
        return fail(err, errlen,
                    "HF cache probe rejected an invalid or manifest-mutated sealed plan");
    }
    for (size_t i = 0; i < plan->artifact_count; i++) {
        ds4_hf_acquisition_artifact *artifact = &plan->artifacts[i];
        int fd = -1;
        artifact_cache_state state = artifact_cache_open(plan, artifact, &fd);
        if (fd >= 0) close(fd);
        artifact->cache_hit = state == ARTIFACT_CACHE_VALID;
        if (state == ARTIFACT_CACHE_INVALID && artifact->requested) {
            return acquisition_context_fail(
                err, errlen, plan, artifact,
                "cache entry is present but is not a complete verified immutable role snapshot");
        }
        if (require_requested && artifact->requested &&
            state != ARTIFACT_CACHE_VALID) {
            return acquisition_context_fail(
                err, errlen, plan, artifact,
                "offline mode requires a complete verified snapshot for every requested role");
        }
    }
    return true;
}

static void sha256_memory_hex(const void *data, size_t len,
                              char output[DS4_HF_SHA256_HEX_SIZE]) {
    hf_sha256 hash;
    unsigned char digest[32];
    sha256_init(&hash);
    sha256_update(&hash, data, len);
    sha256_final(&hash, digest);
    sha256_hex(digest, output);
    secure_clear(digest, sizeof(digest));
}

static bool metadata_cache_parts(const ds4_hf_cli_config *cfg,
                                 const char *repo,
                                 const char *revision,
                                 char cache_root[DS4_HF_CACHE_PATH_MAX],
                                 char repo_component[DS4_HF_REPO_MAX + 4],
                                 char snapshot_parent[DS4_HF_CACHE_PATH_MAX]) {
    int written;
    return cache_root_resolve(cfg, cache_root) &&
           cache_component_encode(repo, repo_component,
                                  DS4_HF_REPO_MAX + 4) &&
           (written = snprintf(snapshot_parent, DS4_HF_CACHE_PATH_MAX,
                               "repos/%s/snapshots/%s",
                               repo_component, revision)) > 0 &&
           written < DS4_HF_CACHE_PATH_MAX;
}

static int metadata_parent_open(const char *cache_root,
                                const char *relative,
                                bool create) {
    ds4_hf_acquisition_plan root = {0};
    if (!copy_string(root.cache_root, sizeof(root.cache_root), cache_root)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int root_fd = cache_root_open(&root, create);
    if (root_fd < 0) return -1;
    int parent_fd = directory_chain_open(root_fd, relative, create);
    int saved_errno = errno;
    close(root_fd);
    errno = saved_errno;
    return parent_fd;
}

static bool read_regular_small(int parent_fd, const char *leaf,
                               char *buffer, size_t capacity,
                               size_t *length_out) {
    int fd = regular_entry_open(parent_fd, leaf, O_RDONLY, 0);
    if (fd < 0) return false;
    struct stat before, after, path_stat;
    bool ok = !fstat(fd, &before) && before.st_size >= 0 &&
              (uint64_t)before.st_size < capacity;
    size_t length = ok ? (size_t)before.st_size : 0;
    size_t used = 0;
    while (ok && used < length) {
        ssize_t got = pread(fd, buffer + used, length - used, (off_t)used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = false;
            break;
        }
        used += (size_t)got;
    }
    ok = ok && !fstat(fd, &after) &&
         !fstatat(parent_fd, leaf, &path_stat, AT_SYMLINK_NOFOLLOW) &&
         S_ISREG(path_stat.st_mode) && path_stat.st_nlink == 1 &&
         before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_size == after.st_size &&
         after.st_dev == path_stat.st_dev && after.st_ino == path_stat.st_ino &&
         after.st_size == path_stat.st_size;
    close(fd);
    if (!ok) return false;
    buffer[length] = '\0';
    *length_out = length;
    return true;
}

static bool publish_regular_exclusive(int parent_fd, const char *leaf,
                                      const char *data, size_t length,
                                      char *err, size_t errlen) {
    char temporary[DS4_HF_PATH_MAX];
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        int written = snprintf(temporary, sizeof(temporary),
                               ".%s.ds4-tmp.%ld.%u", leaf,
                               (long)getpid(), attempt);
        if (written <= 0 || (size_t)written >= sizeof(temporary)) {
            return fail(err, errlen,
                        "HF metadata cache temporary path is too long");
        }
        fd = regular_entry_open(parent_fd, temporary,
                                O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) {
        return fail(err, errlen, "cannot create HF metadata cache entry: %s",
                    strerror(errno));
    }
    bool ok = write_all(fd, data, length) && !fchmod(fd, 0400) && !fsync(fd);
    int saved_errno = errno;
    if (close(fd) && ok) {
        ok = false;
        saved_errno = errno;
    }
    if (ok && linkat(parent_fd, temporary, parent_fd, leaf, 0)) {
        ok = false;
        saved_errno = errno;
    }
    unlinkat(parent_fd, temporary, 0);
    if (!ok) {
        return fail(err, errlen, "cannot publish HF metadata cache entry: %s",
                    strerror(saved_errno));
    }
    return true;
}

static bool manifest_cache_load(const ds4_hf_cli_config *cfg,
                                const char *repo,
                                const char *revision,
                                char **json_out,
                                size_t *json_len_out,
                                char *err,
                                size_t errlen) {
    *json_out = NULL;
    *json_len_out = 0;
    char cache_root[DS4_HF_CACHE_PATH_MAX];
    char repo_component[DS4_HF_REPO_MAX + 4];
    char parent[DS4_HF_CACHE_PATH_MAX];
    if (!metadata_cache_parts(cfg, repo, revision, cache_root,
                              repo_component, parent)) {
        return fail(err, errlen, "HF offline metadata cache path is unavailable");
    }
    int parent_fd = metadata_parent_open(cache_root, parent, false);
    if (parent_fd < 0) {
        return fail(err, errlen,
                    "HF offline snapshot metadata is missing for repository='%s' revision='%s'",
                    repo, revision);
    }
    char metadata[1024];
    size_t metadata_len = 0;
    if (!read_regular_small(parent_fd, "variants.json.ds4-meta",
                            metadata, sizeof(metadata), &metadata_len)) {
        close(parent_fd);
        return fail(err, errlen,
                    "HF offline snapshot manifest metadata is missing or untrusted for repository='%s' revision='%s'",
                    repo, revision);
    }
    char meta_repo[DS4_HF_REPO_MAX] = {0};
    char meta_revision[DS4_HF_COMMIT_SHA_LEN + 1] = {0};
    char digest[DS4_HF_SHA256_HEX_SIZE] = {0};
    uint64_t bytes = 0;
    char trailing = '\0';
    int fields = sscanf(metadata,
                        "version=1\nrepository=%255[^\n]\nrevision=%40[^\n]\nbytes=%" SCNu64 "\nsha256=%64[^\n]\n%c",
                        meta_repo, meta_revision, &bytes, digest, &trailing);
    if (fields != 4 || strcmp(meta_repo, repo) ||
        strcmp(meta_revision, revision) ||
        !manifest_valid_sha256(digest) ||
        bytes == 0 || bytes > DS4_HF_MANIFEST_MAX_BYTES) {
        close(parent_fd);
        return fail(err, errlen,
                    "HF offline snapshot manifest metadata is invalid for repository='%s' revision='%s'",
                    repo, revision);
    }
    int manifest_fd = regular_entry_open(parent_fd, "variants.json", O_RDONLY, 0);
    char actual[DS4_HF_SHA256_HEX_SIZE] = {0};
    if (manifest_fd < 0 ||
        !hash_regular_fd_stable(parent_fd, "variants.json", manifest_fd,
                                bytes, digest, actual)) {
        if (manifest_fd >= 0) close(manifest_fd);
        close(parent_fd);
        return fail(err, errlen,
                    "HF offline snapshot manifest is missing or fails verification for repository='%s' revision='%s'",
                    repo, revision);
    }
    char *json = malloc((size_t)bytes + 1u);
    if (!json) {
        close(manifest_fd);
        close(parent_fd);
        return fail(err, errlen, "HF offline manifest allocation failed");
    }
    size_t used = 0;
    while (used < (size_t)bytes) {
        ssize_t got = pread(manifest_fd, json + used,
                            (size_t)bytes - used, (off_t)used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(manifest_fd);
    close(parent_fd);
    if (used != (size_t)bytes) {
        free(json);
        return fail(err, errlen, "HF offline snapshot manifest changed while reading");
    }
    char read_digest[DS4_HF_SHA256_HEX_SIZE];
    sha256_memory_hex(json, used, read_digest);
    if (strcmp(read_digest, digest)) {
        free(json);
        return fail(err, errlen,
                    "HF offline snapshot manifest changed after verification");
    }
    json[used] = '\0';
    *json_out = json;
    *json_len_out = used;
    return true;
}

static bool manifest_cache_publish(const ds4_hf_cli_config *cfg,
                                   const ds4_hf_resolved_repo *resolved,
                                   const char *json,
                                   size_t json_len,
                                   char *err,
                                   size_t errlen) {
    char cache_root[DS4_HF_CACHE_PATH_MAX];
    char repo_component[DS4_HF_REPO_MAX + 4];
    char parent[DS4_HF_CACHE_PATH_MAX];
    if (!metadata_cache_parts(cfg, resolved->repo, resolved->commit,
                              cache_root, repo_component, parent)) {
        return fail(err, errlen, "HF metadata cache path is unavailable");
    }
    char digest[DS4_HF_SHA256_HEX_SIZE];
    sha256_memory_hex(json, json_len, digest);
    char metadata[1024];
    int metadata_len = snprintf(
        metadata, sizeof(metadata),
        "version=1\nrepository=%s\nrevision=%s\nbytes=%zu\nsha256=%s\n",
        resolved->repo, resolved->commit, json_len, digest);
    if (metadata_len <= 0 || (size_t)metadata_len >= sizeof(metadata)) {
        return fail(err, errlen, "HF manifest metadata is too long");
    }
    int parent_fd = metadata_parent_open(cache_root, parent, true);
    if (parent_fd < 0) {
        return fail(err, errlen, "cannot create HF metadata snapshot: %s",
                    strerror(errno));
    }
    int lock_fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        lock_fd = regular_entry_open(parent_fd, ".variants.lock",
                                     O_RDWR | O_CREAT, 0600);
        if (lock_fd >= 0 || (errno != ENOENT && errno != EINTR)) break;
        sched_yield();
    }
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX)) {
        int saved_errno = errno;
        if (lock_fd >= 0) close(lock_fd);
        close(parent_fd);
        return fail(err, errlen, "cannot lock HF metadata snapshot: %s",
                    strerror(saved_errno));
    }
    struct stat manifest_st, metadata_st;
    bool manifest_exists = !fstatat(parent_fd, "variants.json", &manifest_st,
                                    AT_SYMLINK_NOFOLLOW);
    int manifest_errno = errno;
    bool metadata_exists = !fstatat(parent_fd, "variants.json.ds4-meta",
                                    &metadata_st, AT_SYMLINK_NOFOLLOW);
    int metadata_errno = errno;
    if (manifest_exists || metadata_exists) {
        if (!manifest_exists || !metadata_exists ||
            manifest_errno == ELOOP || metadata_errno == ELOOP) {
            fail(err, errlen,
                 "HF immutable metadata snapshot is incomplete or untrusted for revision='%s'",
                 resolved->commit);
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            close(parent_fd);
            return false;
        }
        char *cached = NULL;
        size_t cached_len = 0;
        bool ok = manifest_cache_load(cfg, resolved->repo, resolved->commit,
                                      &cached, &cached_len, err, errlen) &&
                  cached_len == json_len && !memcmp(cached, json, json_len);
        free(cached);
        if (!ok && (!err || !err[0])) {
            fail(err, errlen,
                 "HF immutable metadata snapshot differs for revision='%s'",
                 resolved->commit);
        }
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        close(parent_fd);
        return ok;
    }
    bool ok = publish_regular_exclusive(parent_fd,
                                        "variants.json.ds4-meta",
                                        metadata, (size_t)metadata_len,
                                        err, errlen) &&
              publish_regular_exclusive(parent_fd, "variants.json",
                                        json, json_len, err, errlen) &&
              !fsync(parent_fd);
    if (!ok) unlinkat(parent_fd, "variants.json.ds4-meta", 0);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    close(parent_fd);
    return ok;
}

static bool reference_cache_path(const ds4_hf_cli_config *cfg,
                                 const char *repo,
                                 char cache_root[DS4_HF_CACHE_PATH_MAX],
                                 char parent[DS4_HF_CACHE_PATH_MAX],
                                 char leaf[DS4_HF_PATH_MAX]) {
    char repo_component[DS4_HF_REPO_MAX + 4];
    char reference_component[DS4_HF_METADATA_MAX * 3 + 4];
    const char *reference = cfg->revision && cfg->revision[0] ?
                            cfg->revision : "default";
    int written;
    return cache_root_resolve(cfg, cache_root) &&
           cache_component_encode(repo, repo_component,
                                  sizeof(repo_component)) &&
           cache_component_encode(reference, reference_component,
                                  sizeof(reference_component)) &&
           (written = snprintf(parent, DS4_HF_CACHE_PATH_MAX,
                               "repos/%s/refs", repo_component)) > 0 &&
           written < DS4_HF_CACHE_PATH_MAX &&
           copy_string(leaf, DS4_HF_PATH_MAX, reference_component);
}

static bool reference_cache_publish(const ds4_hf_cli_config *cfg,
                                    const ds4_hf_resolved_repo *resolved,
                                    char *err,
                                    size_t errlen) {
    char cache_root[DS4_HF_CACHE_PATH_MAX], parent[DS4_HF_CACHE_PATH_MAX];
    char leaf[DS4_HF_PATH_MAX], temporary[DS4_HF_PATH_MAX];
    if (!reference_cache_path(cfg, resolved->repo, cache_root, parent, leaf)) {
        return fail(err, errlen, "HF reference cache path is unavailable");
    }
    int parent_fd = metadata_parent_open(cache_root, parent, true);
    if (parent_fd < 0) return fail(err, errlen, "cannot create HF reference cache");
    char content[DS4_HF_COMMIT_SHA_LEN + 2];
    int content_len = snprintf(content, sizeof(content), "%s\n",
                               resolved->commit);
    if (content_len != DS4_HF_COMMIT_SHA_LEN + 1) {
        close(parent_fd);
        return fail(err, errlen, "HF reference cache record is invalid");
    }
    int fd = -1;
    for (unsigned attempt = 0; attempt < 64; attempt++) {
        int written = snprintf(temporary, sizeof(temporary),
                               ".%s.tmp.%ld.%u", leaf,
                               (long)getpid(), attempt);
        if (written <= 0 || (size_t)written >= sizeof(temporary)) break;
        fd = regular_entry_open(parent_fd, temporary,
                                O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    bool ok = fd >= 0 && write_all(fd, content, (size_t)content_len) &&
              !fchmod(fd, 0400) && !fsync(fd);
    if (fd >= 0 && close(fd) && ok) ok = false;
    if (ok && renameat(parent_fd, temporary, parent_fd, leaf)) ok = false;
    unlinkat(parent_fd, temporary, 0);
    if (ok) ok = !fsync(parent_fd);
    close(parent_fd);
    return ok ? true : fail(err, errlen, "cannot publish HF reference cache");
}

static bool reference_cache_load(const ds4_hf_cli_config *cfg,
                                 const char *repo,
                                 char commit[DS4_HF_COMMIT_SHA_LEN + 1],
                                 char *err,
                                 size_t errlen) {
    if (cfg->revision && valid_commit(cfg->revision)) {
        return copy_string(commit, DS4_HF_COMMIT_SHA_LEN + 1, cfg->revision);
    }
    char cache_root[DS4_HF_CACHE_PATH_MAX], parent[DS4_HF_CACHE_PATH_MAX];
    char leaf[DS4_HF_PATH_MAX], content[DS4_HF_COMMIT_SHA_LEN + 2];
    if (!reference_cache_path(cfg, repo, cache_root, parent, leaf)) {
        return fail(err, errlen, "HF offline reference cache path is unavailable");
    }
    int parent_fd = metadata_parent_open(cache_root, parent, false);
    size_t content_len = 0;
    bool ok = parent_fd >= 0 &&
              read_regular_small(parent_fd, leaf, content, sizeof(content),
                                 &content_len);
    if (parent_fd >= 0) close(parent_fd);
    if (!ok || content_len != DS4_HF_COMMIT_SHA_LEN + 1 ||
        content[DS4_HF_COMMIT_SHA_LEN] != '\n') {
        return fail(err, errlen,
                    "HF offline reference is not cached for repository='%s' reference='%s'",
                    repo, cfg->revision ? cfg->revision : "default");
    }
    content[DS4_HF_COMMIT_SHA_LEN] = '\0';
    if (!valid_commit(content)) {
        return fail(err, errlen, "HF offline cached reference is invalid");
    }
    return copy_string(commit, DS4_HF_COMMIT_SHA_LEN + 1, content);
}

static bool repository_metadata_prepare(const ds4_hf_cli_config *cfg,
                                        ds4_hf_resolved_repo *resolved,
                                        ds4_hf_manifest *manifest,
                                        bool *from_cache,
                                        char *err,
                                        size_t errlen) {
    char *json = NULL;
    size_t json_len = 0;
    *from_cache = cfg->offline;
    if (cfg->offline) {
        if (!safe_repo_id(cfg->repo) ||
            !normalize_endpoint(cfg->endpoint, resolved->endpoint,
                                sizeof(resolved->endpoint)) ||
            !copy_string(resolved->repo, sizeof(resolved->repo), cfg->repo) ||
            !reference_cache_load(cfg, cfg->repo, resolved->commit,
                                  err, errlen) ||
            !manifest_cache_load(cfg, cfg->repo, resolved->commit,
                                 &json, &json_len, err, errlen)) return false;
    } else {
        ds4_hf_resolve_status status = ds4_hf_resolve_repository(
            cfg, 30000L, resolved, err, errlen);
        if (status != DS4_HF_RESOLVE_OK ||
            !manifest_download(cfg, resolved, &json, &json_len,
                               err, errlen)) return false;
    }
    bool parsed = ds4_hf_manifest_parse(json, json_len, manifest,
                                        err, errlen) &&
                  !strcmp(manifest->repository, resolved->repo);
    if (!parsed && err && errlen && !err[0]) {
        fail(err, errlen, "HF manifest repository does not match the resolved repository");
    }
    if (parsed && !cfg->offline) {
        parsed = manifest_cache_publish(cfg, resolved, json, json_len,
                                        err, errlen) &&
                 reference_cache_publish(cfg, resolved, err, errlen);
    }
    free(json);
    return parsed;
}

bool ds4_hf_diagnostics_prepare(const ds4_hf_cli_config *cfg,
                                ds4_hf_diagnostics *diagnostics,
                                char *err,
                                size_t errlen) {
    if (!cfg || !diagnostics ||
        cfg->receiver_source != DS4_HF_RECEIVER_REPOSITORY ||
        (!cfg->list_variants && !cfg->dry_run)) {
        return fail(err, errlen, "HF diagnostics require a repository diagnostic mode");
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    if (!repository_metadata_prepare(cfg, &diagnostics->resolved,
                                     &diagnostics->manifest,
                                     &diagnostics->metadata_from_cache,
                                     err, errlen)) return false;
    if (cfg->list_variants) return true;
    if (!ds4_hf_acquisition_plan_build(
            cfg, &diagnostics->resolved, &diagnostics->manifest, false,
            &diagnostics->plan, err, errlen)) return false;
    diagnostics->selected_variant = ds4_hf_manifest_find_variant(
        &diagnostics->manifest, diagnostics->plan.selector);
    if (!diagnostics->selected_variant) {
        return fail(err, errlen,
                    "HF diagnostics lost the manifest-selected variant");
    }
    return ds4_hf_acquisition_probe_cache(&diagnostics->plan, cfg->offline,
                                          err, errlen);
}

static void hf_json_string(FILE *fp, const char *value) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(value ? value : "");
         *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\b': fputs("\\b", fp); break;
            case '\f': fputs("\\f", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", (unsigned)*p);
                else fputc(*p, fp);
        }
    }
    fputc('"', fp);
}

typedef struct {
    uint32_t bit;
    const char *name;
} hf_capability_name;

static const hf_capability_name hf_capability_names[] = {
    {DS4_HF_CAP_DEEPSEEK4, "deepseek4"},
    {DS4_HF_CAP_TEXT_GENERATION, "text_generation"},
    {DS4_HF_CAP_DS4_VISION, "ds4_vision"},
    {DS4_HF_CAP_LLAMA_CPP_MMPROJ, "llama_cpp_mmproj"},
    {DS4_HF_CAP_DSPARK, "dspark"},
    {DS4_HF_CAP_ROUTE_TOKEN_ID, "route_token_id"},
    {DS4_HF_CAP_SSD_STREAMING, "ssd_streaming"},
};

static void hf_json_capabilities(FILE *fp, uint32_t capabilities,
                                 const char *const *unknown,
                                 size_t unknown_count) {
    fputc('[', fp);
    bool first = true;
    for (size_t i = 0; i < sizeof(hf_capability_names) /
                            sizeof(hf_capability_names[0]); i++) {
        if (!(capabilities & hf_capability_names[i].bit)) continue;
        if (!first) fputc(',', fp);
        hf_json_string(fp, hf_capability_names[i].name);
        first = false;
    }
    for (size_t i = 0; i < unknown_count; i++) {
        if (!first) fputc(',', fp);
        hf_json_string(fp, unknown[i]);
        first = false;
    }
    fputc(']', fp);
}

static void hf_human_capabilities(FILE *fp, uint32_t capabilities,
                                  const char *const *unknown,
                                  size_t unknown_count) {
    bool first = true;
    for (size_t i = 0; i < sizeof(hf_capability_names) /
                            sizeof(hf_capability_names[0]); i++) {
        if (!(capabilities & hf_capability_names[i].bit)) continue;
        fprintf(fp, "%s%s", first ? "" : ",",
                hf_capability_names[i].name);
        first = false;
    }
    for (size_t i = 0; i < unknown_count; i++) {
        fprintf(fp, "%s%s", first ? "" : ",", unknown[i]);
        first = false;
    }
    if (first) fputs("none", fp);
}

#define DS4_HF_VARIANT_MAX_UNKNOWN_OPTIONAL \
    (DS4_HF_ACQUISITION_MAX_ARTIFACTS * DS4_HF_MANIFEST_MAX_CAPABILITIES)

typedef struct {
    uint32_t required;
    uint32_t optional;
    const char *unknown_optional[DS4_HF_VARIANT_MAX_UNKNOWN_OPTIONAL];
    size_t unknown_optional_count;
} variant_capability_summary;

static void variant_capabilities(const ds4_hf_manifest_variant *variant,
                                 variant_capability_summary *summary) {
    const ds4_hf_manifest_artifact *artifacts[] = {
        &variant->receiver,
        &variant->ds4_vision.tower,
        &variant->ds4_vision.projector,
        &variant->ds4_vision.config,
        &variant->llama_cpp_mmproj,
        variant->has_dspark ? &variant->dspark : NULL,
    };
    memset(summary, 0, sizeof(*summary));
    for (size_t i = 0; i < sizeof(artifacts) / sizeof(artifacts[0]); i++) {
        if (!artifacts[i]) continue;
        summary->required |= artifacts[i]->required_capabilities;
        summary->optional |= artifacts[i]->optional_capabilities;
        for (size_t j = 0;
             j < artifacts[i]->unknown_optional_capability_count; j++) {
            const char *name = artifacts[i]->unknown_optional_capabilities[j];
            bool duplicate = false;
            for (size_t k = 0; k < summary->unknown_optional_count; k++) {
                if (!strcmp(summary->unknown_optional[k], name)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && summary->unknown_optional_count <
                                  DS4_HF_VARIANT_MAX_UNKNOWN_OPTIONAL) {
                summary->unknown_optional[summary->unknown_optional_count++] =
                    name;
            }
        }
    }
}

static bool variant_llama_heuristics(const ds4_hf_manifest_variant *variant,
                                     bool *primary,
                                     bool *siblings) {
    char ignored[256] = {0};
    *primary = ds4_hf_llama_primary_selectable(variant->selector,
                                               variant->receiver.path);
    *siblings = ds4_hf_llama_siblings_valid(
        variant->receiver.path, variant->llama_cpp_mmproj.path,
        variant->has_dspark ? variant->dspark.path : NULL,
        ignored, sizeof(ignored));
    return *primary && *siblings;
}

static void print_json_artifact(FILE *fp,
                                const ds4_hf_manifest_artifact *artifact) {
    const char *unknown[DS4_HF_MANIFEST_MAX_CAPABILITIES];
    for (size_t i = 0; i < artifact->unknown_optional_capability_count; i++)
        unknown[i] = artifact->unknown_optional_capabilities[i];
    fputs("{\"path\":", fp);
    hf_json_string(fp, artifact->path);
    fprintf(fp, ",\"bytes\":%" PRIu64 ",\"precision\":",
            artifact->bytes);
    hf_json_string(fp, artifact->precision);
    fputs(",\"runtime_compatibility\":{\"ds4\":", fp);
    fputs(artifact->supports_ds4 ? "true" : "false", fp);
    fputs(",\"llama_cpp\":", fp);
    fputs(artifact->supports_llama_cpp ? "true" : "false", fp);
    fputs(",\"ds4_minimum_revision\":", fp);
    hf_json_string(fp, artifact->ds4_minimum_revision);
    fputs(",\"llama_cpp_minimum_revision\":", fp);
    hf_json_string(fp, artifact->llama_cpp_minimum_revision);
    fputs("},\"declared_capabilities\":{\"required\":", fp);
    hf_json_capabilities(fp, artifact->required_capabilities, NULL, 0);
    fputs(",\"optional\":", fp);
    hf_json_capabilities(fp, artifact->optional_capabilities, unknown,
                         artifact->unknown_optional_capability_count);
    fputs("}}", fp);
}

static void print_json_variant(FILE *fp,
                               const ds4_hf_manifest_variant *variant) {
    bool primary = false, siblings = false;
    variant_llama_heuristics(variant, &primary, &siblings);
    variant_capability_summary capabilities;
    variant_capabilities(variant, &capabilities);
    fputs("{\"selector\":", fp);
    hf_json_string(fp, variant->selector);
    fputs(",\"default\":", fp);
    fputs(variant->is_default ? "true" : "false", fp);
    fputs(",\"receiver\":", fp);
    print_json_artifact(fp, &variant->receiver);
    fputs(",\"ds4_vision\":{\"tower\":", fp);
    print_json_artifact(fp, &variant->ds4_vision.tower);
    fputs(",\"projector\":", fp);
    print_json_artifact(fp, &variant->ds4_vision.projector);
    fputs(",\"config\":", fp);
    print_json_artifact(fp, &variant->ds4_vision.config);
    fputs("},\"llama_cpp_mmproj\":", fp);
    print_json_artifact(fp, &variant->llama_cpp_mmproj);
    fputs(",\"dspark\":", fp);
    if (variant->has_dspark) print_json_artifact(fp, &variant->dspark);
    else fputs("null", fp);
    fputs(",\"manifest_selection\":true,\"llama_cpp_heuristics\":{\"primary_filename_match\":", fp);
    fputs(primary ? "true" : "false", fp);
    fputs(",\"sibling_layout_match\":", fp);
    fputs(siblings ? "true" : "false", fp);
    fputs("},\"declared_capabilities\":{\"required\":", fp);
    hf_json_capabilities(fp, capabilities.required, NULL, 0);
    fputs(",\"optional\":", fp);
    hf_json_capabilities(fp, capabilities.optional,
                         capabilities.unknown_optional,
                         capabilities.unknown_optional_count);
    fputs("}}", fp);
}

static void print_human_artifact(FILE *fp, const char *role,
                                 const ds4_hf_manifest_artifact *artifact) {
    const char *unknown[DS4_HF_MANIFEST_MAX_CAPABILITIES];
    for (size_t i = 0; i < artifact->unknown_optional_capability_count; i++)
        unknown[i] = artifact->unknown_optional_capabilities[i];
    fprintf(fp,
            "  %s: path=%s bytes=%" PRIu64
            " precision=%s ds4=%s llama_cpp=%s"
            " ds4_minimum_revision=%s llama_cpp_minimum_revision=%s required=",
            role, artifact->path, artifact->bytes, artifact->precision,
            artifact->supports_ds4 ? "yes" : "no",
            artifact->supports_llama_cpp ? "yes" : "no",
            artifact->ds4_minimum_revision[0]
                ? artifact->ds4_minimum_revision : "unavailable",
            artifact->llama_cpp_minimum_revision[0]
                ? artifact->llama_cpp_minimum_revision : "unavailable");
    hf_human_capabilities(fp, artifact->required_capabilities, NULL, 0);
    fputs(" optional=", fp);
    hf_human_capabilities(fp, artifact->optional_capabilities, unknown,
                          artifact->unknown_optional_capability_count);
    fputc('\n', fp);
}

static void print_human_variant(FILE *fp,
                                const ds4_hf_manifest_variant *variant) {
    bool primary = false, siblings = false;
    variant_llama_heuristics(variant, &primary, &siblings);
    variant_capability_summary capabilities;
    variant_capabilities(variant, &capabilities);
    fprintf(fp, "variant %s%s\n", variant->selector,
            variant->is_default ? " (default)" : "");
    print_human_artifact(fp, "receiver", &variant->receiver);
    print_human_artifact(fp, "ds4_vision.tower", &variant->ds4_vision.tower);
    print_human_artifact(fp, "ds4_vision.projector",
                         &variant->ds4_vision.projector);
    print_human_artifact(fp, "ds4_vision.config", &variant->ds4_vision.config);
    print_human_artifact(fp, "llama_cpp_mmproj",
                         &variant->llama_cpp_mmproj);
    if (variant->has_dspark)
        print_human_artifact(fp, "dspark", &variant->dspark);
    else
        fputs("  dspark: unavailable\n", fp);
    fprintf(fp,
            "  selection: manifest=yes llama_primary_filename=%s llama_sibling_layout=%s\n",
            primary ? "match" : "mismatch",
            siblings ? "match" : "mismatch");
    fputs("  declared_capabilities: required=", fp);
    hf_human_capabilities(fp, capabilities.required, NULL, 0);
    fputs(" optional=", fp);
    hf_human_capabilities(fp, capabilities.optional,
                          capabilities.unknown_optional,
                          capabilities.unknown_optional_count);
    fputc('\n', fp);
}

static bool add_total(uint64_t *total, uint64_t value) {
    if (UINT64_MAX - *total < value) return false;
    *total += value;
    return true;
}

typedef struct {
    uint64_t transfer;
    uint64_t selected_weights;
    uint64_t receiver_only;
    uint64_t ds4_vision;
    uint64_t ds4_vision_dspark;
    uint64_t llama_cpp_mmproj;
    bool ds4_vision_dspark_available;
} diagnostic_totals;

static bool diagnostics_totals(const ds4_hf_diagnostics *diagnostics,
                               diagnostic_totals *totals) {
    memset(totals, 0, sizeof(*totals));
    const ds4_hf_manifest_variant *v = diagnostics->selected_variant;
    totals->receiver_only = v->receiver.bytes;
    totals->ds4_vision = v->receiver.bytes;
    totals->ds4_vision_dspark = v->receiver.bytes;
    totals->llama_cpp_mmproj = v->receiver.bytes;
    totals->ds4_vision_dspark_available = v->has_dspark;
    if (!add_total(&totals->ds4_vision, v->ds4_vision.tower.bytes) ||
        !add_total(&totals->ds4_vision, v->ds4_vision.projector.bytes) ||
        !add_total(&totals->ds4_vision_dspark, v->ds4_vision.tower.bytes) ||
        !add_total(&totals->ds4_vision_dspark, v->ds4_vision.projector.bytes) ||
        (v->has_dspark &&
         !add_total(&totals->ds4_vision_dspark, v->dspark.bytes)) ||
        !add_total(&totals->llama_cpp_mmproj,
                   v->llama_cpp_mmproj.bytes)) return false;
    for (size_t i = 0; i < diagnostics->plan.artifact_count; i++) {
        const ds4_hf_acquisition_artifact *artifact =
            &diagnostics->plan.artifacts[i];
        if (!artifact->requested) continue;
        if (!artifact->cache_hit &&
            !add_total(&totals->transfer, artifact->bytes)) return false;
        if (artifact->role != DS4_HF_ROLE_VISION_CONFIG &&
            !add_total(&totals->selected_weights, artifact->bytes)) return false;
    }
    return true;
}

static const char *diagnostic_runtime_name(const ds4_hf_cli_config *cfg) {
    if (cfg->vision_source == DS4_HF_VISION_CATALOG &&
        cfg->dspark_source == DS4_HF_DSPARK_CATALOG)
        return "ds4-receiver+vision+dspark";
    if (cfg->vision_source == DS4_HF_VISION_CATALOG)
        return "ds4-receiver+vision";
    if (cfg->dspark_source == DS4_HF_DSPARK_CATALOG)
        return "ds4-receiver+dspark";
    return "ds4-receiver-only";
}

static void print_json_dry_run(FILE *fp, const ds4_hf_cli_config *cfg,
                               const ds4_hf_diagnostics *diagnostics) {
    diagnostic_totals totals;
    bool totals_ok = diagnostics_totals(diagnostics, &totals);
    bool primary = false, siblings = false;
    variant_llama_heuristics(diagnostics->selected_variant,
                             &primary, &siblings);
    fputs("{\"schema_version\":1,\"mode\":\"hf-dry-run\",\"repository\":", fp);
    hf_json_string(fp, diagnostics->resolved.repo);
    fputs(",\"revision\":", fp);
    hf_json_string(fp, diagnostics->resolved.commit);
    fputs(",\"selector\":", fp);
    hf_json_string(fp, diagnostics->plan.selector);
    fputs(",\"metadata_source\":", fp);
    hf_json_string(fp, diagnostics->metadata_from_cache ? "cache" : "network");
    fputs(",\"selected_runtime\":", fp);
    hf_json_string(fp, diagnostic_runtime_name(cfg));
    fputs(",\"manifest_selection\":true,\"llama_cpp_heuristics\":{\"primary_filename_match\":", fp);
    fputs(primary ? "true" : "false", fp);
    fputs(",\"sibling_layout_match\":", fp);
    fputs(siblings ? "true" : "false", fp);
    fputs("},\"files\":[", fp);
    for (size_t i = 0; i < diagnostics->plan.artifact_count; i++) {
        const ds4_hf_acquisition_artifact *artifact =
            &diagnostics->plan.artifacts[i];
        if (i) fputc(',', fp);
        fputs("{\"role\":", fp);
        hf_json_string(fp, ds4_hf_artifact_role_name(artifact->role));
        fputs(",\"path\":", fp);
        hf_json_string(fp, artifact->repo_path);
        fputs(",\"selected\":", fp);
        fputs(artifact->requested ? "true" : "false", fp);
        fputs(",\"cache\":", fp);
        hf_json_string(fp, artifact->cache_hit ? "cached" : "missing");
        fprintf(fp, ",\"bytes\":%" PRIu64 "}", artifact->bytes);
    }
    if (totals_ok) {
        fprintf(fp,
                "],\"totals\":{\"transfer_bytes\":%" PRIu64
                ",\"selected_runtime_weight_bytes\":%" PRIu64
                ",\"receiver_only_bytes\":%" PRIu64
                ",\"ds4_receiver_vision_bytes\":%" PRIu64
                ",\"ds4_receiver_vision_dspark_bytes\":",
                totals.transfer, totals.selected_weights,
                totals.receiver_only, totals.ds4_vision);
        if (totals.ds4_vision_dspark_available)
            fprintf(fp, "%" PRIu64, totals.ds4_vision_dspark);
        else
            fputs("null", fp);
        fprintf(fp, ",\"llama_cpp_receiver_mmproj_bytes\":%" PRIu64 "}}\n",
                totals.llama_cpp_mmproj);
    } else {
        fputs("],\"totals\":null}\n", fp);
    }
}

static void print_human_dry_run(FILE *fp, const ds4_hf_cli_config *cfg,
                                const ds4_hf_diagnostics *diagnostics) {
    diagnostic_totals totals;
    bool totals_ok = diagnostics_totals(diagnostics, &totals);
    bool primary = false, siblings = false;
    variant_llama_heuristics(diagnostics->selected_variant,
                             &primary, &siblings);
    fprintf(fp,
            "HF dry run\nrepository: %s\nrevision: %s\nselector: %s\nmetadata_source: %s\nselected_runtime: %s\nmanifest_selection: yes\nllama_primary_filename: %s\nllama_sibling_layout: %s\nfiles:\n",
            diagnostics->resolved.repo, diagnostics->resolved.commit,
            diagnostics->plan.selector,
            diagnostics->metadata_from_cache ? "cache" : "network",
            diagnostic_runtime_name(cfg), primary ? "match" : "mismatch",
            siblings ? "match" : "mismatch");
    for (size_t i = 0; i < diagnostics->plan.artifact_count; i++) {
        const ds4_hf_acquisition_artifact *artifact =
            &diagnostics->plan.artifacts[i];
        fprintf(fp, "  %s: path=%s selected=%s cache=%s bytes=%" PRIu64 "\n",
                ds4_hf_artifact_role_name(artifact->role),
                artifact->repo_path, artifact->requested ? "yes" : "no",
                artifact->cache_hit ? "cached" : "missing", artifact->bytes);
    }
    if (totals_ok) {
        fprintf(fp,
                "totals:\n  transfer_bytes: %" PRIu64
                "\n  selected_runtime_weight_bytes: %" PRIu64
                "\n  receiver_only_bytes: %" PRIu64
                "\n  ds4_receiver_vision_bytes: %" PRIu64
                "\n  ds4_receiver_vision_dspark_bytes: ",
                totals.transfer, totals.selected_weights,
                totals.receiver_only, totals.ds4_vision);
        if (totals.ds4_vision_dspark_available)
            fprintf(fp, "%" PRIu64, totals.ds4_vision_dspark);
        else
            fputs("unavailable", fp);
        fprintf(fp, "\n  llama_cpp_receiver_mmproj_bytes: %" PRIu64 "\n",
                totals.llama_cpp_mmproj);
    } else {
        fputs("totals: overflow\n", fp);
    }
}

void ds4_hf_diagnostics_print(FILE *fp,
                              const ds4_hf_cli_config *cfg,
                              const ds4_hf_diagnostics *diagnostics) {
    if (!fp || !cfg || !diagnostics) return;
    if (cfg->dry_run) {
        if (cfg->diagnostics_json) print_json_dry_run(fp, cfg, diagnostics);
        else print_human_dry_run(fp, cfg, diagnostics);
        return;
    }
    if (cfg->diagnostics_json) {
        fputs("{\"schema_version\":1,\"mode\":\"list-hf-variants\",\"repository\":", fp);
        hf_json_string(fp, diagnostics->resolved.repo);
        fputs(",\"revision\":", fp);
        hf_json_string(fp, diagnostics->resolved.commit);
        fputs(",\"metadata_source\":", fp);
        hf_json_string(fp, diagnostics->metadata_from_cache ? "cache" : "network");
        fputs(",\"variants\":[", fp);
        for (size_t i = 0; i < diagnostics->manifest.variant_count; i++) {
            if (i) fputc(',', fp);
            print_json_variant(fp, &diagnostics->manifest.variants[i]);
        }
        fputs("]}\n", fp);
    } else {
        fprintf(fp,
                "HF variants\nrepository: %s\nrevision: %s\nmetadata_source: %s\n",
                diagnostics->resolved.repo, diagnostics->resolved.commit,
                diagnostics->metadata_from_cache ? "cache" : "network");
        for (size_t i = 0; i < diagnostics->manifest.variant_count; i++)
            print_human_variant(fp, &diagnostics->manifest.variants[i]);
    }
}

#define DS4_HF_SAFETENSORS_HEADER_MAX (1024u * 1024u)
#define DS4_HF_SAFETENSORS_RANK_MAX 8u

typedef struct {
    char dtype[16];
    uint64_t shape[DS4_HF_SAFETENSORS_RANK_MAX];
    size_t rank;
    bool have_dtype;
    bool have_shape;
} safetensors_tensor;

static bool safetensors_parse_shape(manifest_json_parser *jp,
                                    safetensors_tensor *tensor) {
    if (!manifest_json_open(jp, '[')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') {
        return manifest_json_close(jp, ']');
    }
    bool more = true;
    while (more) {
        if (tensor->rank >= DS4_HF_SAFETENSORS_RANK_MAX) {
            return json_fail(jp, "safetensors tensor rank exceeds %u",
                             DS4_HF_SAFETENSORS_RANK_MAX);
        }
        if (!manifest_json_uint64(jp, &tensor->shape[tensor->rank++]) ||
            !manifest_json_next(jp, ']', &more)) return false;
    }
    return true;
}

static bool safetensors_parse_tensor(manifest_json_parser *jp,
                                     safetensors_tensor *tensor) {
    memset(tensor, 0, sizeof(*tensor));
    if (!manifest_json_open(jp, '{')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        return json_fail(jp, "empty safetensors tensor descriptor");
    }
    bool more = true;
    while (more) {
        char key[64];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        if (!strcmp(key, "dtype")) {
            if (tensor->have_dtype || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, tensor->dtype,
                                      sizeof(tensor->dtype))) return false;
            tensor->have_dtype = true;
        } else if (!strcmp(key, "shape")) {
            if (tensor->have_shape ||
                !safetensors_parse_shape(jp, tensor)) return false;
            tensor->have_shape = true;
        } else if (!manifest_json_skip_value(jp)) {
            return false;
        }
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    return tensor->have_dtype && tensor->have_shape;
}

static bool safetensors_tensor_is(const safetensors_tensor *tensor,
                                  const uint64_t *shape, size_t rank) {
    return tensor->have_dtype && !strcmp(tensor->dtype, "BF16") &&
           tensor->have_shape && tensor->rank == rank &&
           !memcmp(tensor->shape, shape, rank * sizeof(*shape));
}

static bool safetensors_parse_projector_metadata(
    manifest_json_parser *jp, unsigned *metadata_seen) {
    if (!manifest_json_open(jp, '{')) return false;
    manifest_json_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') {
        return manifest_json_close(jp, '}');
    }
    bool more = true;
    while (more) {
        char key[DS4_HF_METADATA_MAX];
        if (!manifest_json_key(jp, key, sizeof(key))) return false;
        unsigned bit = 0;
        const char *expected = NULL;
        if (!strcmp(key, "encoder_dim")) {
            bit = 1u;
            expected = "896";
        } else if (!strcmp(key, "hidden")) {
            bit = 2u;
            expected = "4096";
        } else if (!strcmp(key, "image_token_id")) {
            bit = 4u;
            expected = "129279";
        }
        if (bit) {
            char value[32];
            if ((*metadata_seen & bit) || !manifest_json_charge(jp) ||
                !manifest_json_string(jp, value, sizeof(value)) ||
                strcmp(value, expected)) return false;
            *metadata_seen |= bit;
        } else if (!manifest_json_skip_value(jp)) {
            return false;
        }
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    return true;
}

static bool safetensors_semantics_valid(const char *json, size_t json_len,
                                        ds4_hf_artifact_role role) {
    char parse_error[256] = {0};
    manifest_json_parser jp = {
        json, json + json_len, 0, 0, parse_error, sizeof(parse_error),
    };
    if (!manifest_json_open(&jp, '{')) return false;
    manifest_json_ws(&jp);
    if (jp.p < jp.end && *jp.p == '}') return false;

    unsigned tensors_seen = 0;
    unsigned metadata_seen = 0;
    bool sam_namespace = false;
    bool qwen_namespace = false;
    bool more = true;
    while (more) {
        char name[DS4_HF_PATH_MAX];
        if (!manifest_json_key(&jp, name, sizeof(name))) return false;
        if (!strcmp(name, "__metadata__")) {
            if (role == DS4_HF_ROLE_VISION_PROJECTOR) {
                if (!safetensors_parse_projector_metadata(&jp,
                                                          &metadata_seen)) {
                    return false;
                }
            } else if (!manifest_json_skip_value(&jp)) {
                return false;
            }
        } else {
            safetensors_tensor tensor;
            if (!safetensors_parse_tensor(&jp, &tensor)) return false;
            if (role == DS4_HF_ROLE_VISION_TOWER) {
                static const uint64_t patch[] = {768, 3, 16, 16};
                static const uint64_t pos[] = {1, 64, 64, 768};
                static const uint64_t neck[] = {256, 768, 1, 1};
                static const uint64_t q_proj[] = {896, 896};
                static const uint64_t norm[] = {896};
                sam_namespace |= !strncmp(name, "model.sam_model.", 16);
                qwen_namespace |= !strncmp(name, "model.qwen2_model.", 18);
                if (!strcmp(name, "model.sam_model.patch_embed.proj.weight")) {
                    if ((tensors_seen & 1u) ||
                        !safetensors_tensor_is(&tensor, patch, 4)) return false;
                    tensors_seen |= 1u;
                } else if (!strcmp(name, "model.sam_model.pos_embed")) {
                    if ((tensors_seen & 2u) ||
                        !safetensors_tensor_is(&tensor, pos, 4)) return false;
                    tensors_seen |= 2u;
                } else if (!strcmp(name, "model.sam_model.neck.0.weight")) {
                    if ((tensors_seen & 4u) ||
                        !safetensors_tensor_is(&tensor, neck, 4)) return false;
                    tensors_seen |= 4u;
                } else if (!strcmp(name, "model.qwen2_model.model.model.layers.0.self_attn.q_proj.weight")) {
                    if ((tensors_seen & 8u) ||
                        !safetensors_tensor_is(&tensor, q_proj, 2)) return false;
                    tensors_seen |= 8u;
                } else if (!strcmp(name, "model.qwen2_model.model.model.layers.23.self_attn.q_proj.weight")) {
                    if ((tensors_seen & 16u) ||
                        !safetensors_tensor_is(&tensor, q_proj, 2)) return false;
                    tensors_seen |= 16u;
                } else if (!strcmp(name, "model.qwen2_model.model.model.norm.weight")) {
                    if ((tensors_seen & 32u) ||
                        !safetensors_tensor_is(&tensor, norm, 1)) return false;
                    tensors_seen |= 32u;
                }
            } else {
                static const uint64_t proj0[] = {4096, 896};
                static const uint64_t proj2[] = {4096, 4096};
                static const uint64_t separator[] = {4096};
                if (!strcmp(name, "proj.0.weight")) {
                    if ((tensors_seen & 1u) ||
                        !safetensors_tensor_is(&tensor, proj0, 2)) return false;
                    tensors_seen |= 1u;
                } else if (!strcmp(name, "proj.2.weight")) {
                    if ((tensors_seen & 2u) ||
                        !safetensors_tensor_is(&tensor, proj2, 2)) return false;
                    tensors_seen |= 2u;
                } else if (!strcmp(name, "view_seperator")) {
                    if ((tensors_seen & 4u) ||
                        !safetensors_tensor_is(&tensor, separator, 1)) return false;
                    tensors_seen |= 4u;
                }
            }
        }
        if (!manifest_json_next(&jp, '}', &more)) return false;
    }
    manifest_json_ws(&jp);
    if (jp.p != jp.end) return false;
    if (role == DS4_HF_ROLE_VISION_TOWER) {
        return tensors_seen == 63u && sam_namespace && qwen_namespace;
    }
    return role == DS4_HF_ROLE_VISION_PROJECTOR && tensors_seen == 7u &&
           metadata_seen == 7u;
}

static int runtime_verified_role_fd(const ds4_hf_runtime *runtime,
                                    ds4_hf_artifact_role role) {
    if (!runtime) return -1;
    for (size_t i = 0; i < runtime->plan.artifact_count; i++) {
        if (runtime->plan.artifacts[i].role == role) {
            return runtime->verified_fds[i];
        }
    }
    return -1;
}

static bool pread_exact(int fd, void *buffer, size_t bytes, off_t offset) {
    unsigned char *out = buffer;
    while (bytes) {
        ssize_t got = pread(fd, out, bytes, offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        out += (size_t)got;
        bytes -= (size_t)got;
        offset += got;
    }
    return true;
}

static bool runtime_safetensors_role_compatible(
    const ds4_hf_runtime *runtime, ds4_hf_artifact_role role,
    char *err, size_t errlen) {
    int fd = runtime_verified_role_fd(runtime, role);
    unsigned char prefix[8];
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        !pread_exact(fd, prefix, sizeof(prefix), 0)) {
        return fail(err, errlen,
                    "catalog vision role '%s' has an unreadable safetensors header",
                    ds4_hf_artifact_role_name(role));
    }
    uint64_t header_len = 0;
    for (unsigned i = 0; i < sizeof(prefix); i++) {
        header_len |= (uint64_t)prefix[i] << (8u * i);
    }
    if (!header_len || header_len > DS4_HF_SAFETENSORS_HEADER_MAX ||
        header_len > (uint64_t)st.st_size -
                         ((uint64_t)st.st_size >= sizeof(prefix) ?
                              sizeof(prefix) : (uint64_t)st.st_size)) {
        return fail(err, errlen,
                    "catalog vision role '%s' has an invalid bounded safetensors header",
                    ds4_hf_artifact_role_name(role));
    }
    char *header = malloc((size_t)header_len + 1u);
    if (!header) {
        return fail(err, errlen,
                    "catalog vision role '%s' safetensors header allocation failed",
                    ds4_hf_artifact_role_name(role));
    }
    bool readable = pread_exact(fd, header, (size_t)header_len,
                                (off_t)sizeof(prefix));
    header[header_len] = '\0';
    bool compatible = readable &&
                      safetensors_semantics_valid(header, (size_t)header_len,
                                                  role);
    free(header);
    if (!compatible) {
        return fail(err, errlen,
                    "catalog vision role '%s' has an incompatible safetensors semantic header",
                    ds4_hf_artifact_role_name(role));
    }
    return true;
}

void ds4_hf_runtime_close_verified(ds4_hf_runtime *runtime) {
    if (!runtime || !runtime->repository) return;
    for (size_t i = 0; i < DS4_HF_ACQUISITION_MAX_ARTIFACTS; i++) {
        if (runtime->verified_fds[i] >= 0) close(runtime->verified_fds[i]);
        runtime->verified_fds[i] = -1;
        runtime->open_paths[i][0] = '\0';
    }
}

bool ds4_hf_runtime_role_verified(const ds4_hf_runtime *runtime,
                                  ds4_hf_artifact_role role) {
    return runtime && role >= DS4_HF_ROLE_RECEIVER &&
           role <= DS4_HF_ROLE_DSPARK &&
           (runtime->verified_roles & (1u << (unsigned)role)) != 0;
}

bool ds4_hf_runtime_vision_artifacts_compatible(
    const ds4_hf_runtime *runtime, char *err, size_t errlen) {
    static const ds4_hf_artifact_role required[] = {
        DS4_HF_ROLE_VISION_TOWER,
        DS4_HF_ROLE_VISION_PROJECTOR,
        DS4_HF_ROLE_VISION_CONFIG,
    };
    if (!runtime || !runtime->repository) {
        return fail(err, errlen,
                    "catalog vision compatibility requires a repository runtime");
    }
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!ds4_hf_runtime_role_verified(runtime, required[i])) {
            return fail(err, errlen, "catalog vision role '%s' is not hash-verified",
                        ds4_hf_artifact_role_name(required[i]));
        }
    }
    if (!runtime->vision_bundle_verified) {
        return fail(err, errlen, "catalog DS4 vision bundle is incomplete");
    }
    if (!runtime_safetensors_role_compatible(
            runtime, DS4_HF_ROLE_VISION_TOWER, err, errlen) ||
        !runtime_safetensors_role_compatible(
            runtime, DS4_HF_ROLE_VISION_PROJECTOR, err, errlen)) {
        return false;
    }
    return true;
}

bool ds4_hf_runtime_vision_compatible(const ds4_hf_runtime *runtime,
                                      uint32_t receiver_image_token_id,
                                      char *err,
                                      size_t errlen) {
    if (!ds4_hf_runtime_vision_artifacts_compatible(runtime, err, errlen)) {
        return false;
    }
    if (runtime->vision_metadata.image_token_id != 129279) {
        return fail(err, errlen,
                    "catalog vision image token id %u is incompatible; expected 129279",
                    runtime->vision_metadata.image_token_id);
    }
    if (receiver_image_token_id != runtime->vision_metadata.image_token_id) {
        return fail(err, errlen,
                    "receiver image token id %u is incompatible with catalog vision image token id %u",
                    receiver_image_token_id,
                    runtime->vision_metadata.image_token_id);
    }
    return true;
}

const char *ds4_hf_runtime_open_path(const ds4_hf_runtime *runtime,
                                     ds4_hf_artifact_role role) {
    if (!ds4_hf_runtime_role_verified(runtime, role)) return NULL;
    for (size_t i = 0; i < runtime->plan.artifact_count; i++) {
        if (runtime->plan.artifacts[i].role == role &&
            runtime->open_paths[i][0]) return runtime->open_paths[i];
    }
    return NULL;
}

bool ds4_hf_runtime_prepare(const ds4_hf_cli_config *cfg,
                            ds4_hf_runtime *runtime,
                            char *err,
                            size_t errlen) {
    if (!runtime) return fail(err, errlen, "invalid HF runtime handoff");
    memset(runtime, 0, sizeof(*runtime));
    for (size_t i = 0; i < DS4_HF_ACQUISITION_MAX_ARTIFACTS; i++) {
        runtime->verified_fds[i] = -1;
    }
    if (!cfg || cfg->receiver_source != DS4_HF_RECEIVER_REPOSITORY) {
        return fail(err, errlen, "HF runtime handoff requires a repository receiver");
    }
    ds4_hf_resolved_repo resolved;
    ds4_hf_manifest manifest;
    bool metadata_from_cache = false;
    if (!repository_metadata_prepare(cfg, &resolved, &manifest,
                                     &metadata_from_cache,
                                     err, errlen)) return false;
    (void)metadata_from_cache;
    runtime->vision_metadata = manifest.shared_vision;
    if (!ds4_hf_acquisition_plan_build(cfg, &resolved, &manifest, false,
                                       &runtime->plan, err, errlen)) {
        return false;
    }
    if (cfg->offline) {
        if (!ds4_hf_acquisition_probe_cache(&runtime->plan, true,
                                            err, errlen)) return false;
    } else if (!ds4_hf_acquisition_execute(cfg, &runtime->plan, 0,
                                           err, errlen)) return false;

    runtime->repository = true;
    for (size_t i = 0; i < runtime->plan.artifact_count; i++) {
        const ds4_hf_acquisition_artifact *artifact =
            &runtime->plan.artifacts[i];
        if (!artifact->requested) continue;
        int fd = -1;
        if (!ds4_hf_acquisition_open_verified(&runtime->plan, i, &fd,
                                              err, errlen)) {
            ds4_hf_runtime_close_verified(runtime);
            return false;
        }
        runtime->verified_fds[i] = fd;
#if defined(__linux__)
        int written = snprintf(runtime->open_paths[i],
                               sizeof(runtime->open_paths[i]),
                               "/proc/self/fd/%d", fd);
#else
        int written = snprintf(runtime->open_paths[i],
                               sizeof(runtime->open_paths[i]),
                               "/dev/fd/%d", fd);
#endif
        if (written <= 0 ||
            (size_t)written >= sizeof(runtime->open_paths[i])) {
            ds4_hf_runtime_close_verified(runtime);
            return fail(err, errlen,
                        "HF verified descriptor path is unavailable");
        }
        runtime->verified_roles |= 1u << (unsigned)artifact->role;
    }
    runtime->vision_bundle_verified =
        ds4_hf_runtime_role_verified(runtime, DS4_HF_ROLE_VISION_TOWER) &&
        ds4_hf_runtime_role_verified(runtime, DS4_HF_ROLE_VISION_PROJECTOR) &&
        ds4_hf_runtime_role_verified(runtime, DS4_HF_ROLE_VISION_CONFIG);
    if (!ds4_hf_runtime_role_verified(runtime, DS4_HF_ROLE_RECEIVER)) {
        ds4_hf_runtime_close_verified(runtime);
        return fail(err, errlen,
                    "HF runtime handoff did not verify a receiver GGUF");
    }
    return true;
}
