#include "ds4_hf.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
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
                         bool server,
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
                                            bool required, uint32_t *bits) {
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
                !manifest_parse_capability_array(jp, true, &artifact->required_capabilities)) return false;
        } else if (!strcmp(key, "optional")) {
            if (!manifest_mark_field(jp, &seen, 2, key) ||
                !manifest_parse_capability_array(jp, false, &artifact->optional_capabilities)) return false;
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
    if (seen != 7) return json_fail(jp, "incomplete DS4 vision bundle; tower, projector, and config are required");
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
        } else if (!strcmp(key, "dspark")) {
            if (!manifest_mark_field(jp, &seen, 64, key) ||
                !manifest_parse_artifact(jp, &variant->dspark)) return false;
            variant->has_dspark = true;
        } else if (!manifest_json_skip_value(jp)) return false;
        if (!manifest_json_next(jp, '}', &more)) return false;
    }
    if ((seen & 63u) != 63u) return json_fail(jp, "variant is missing selector, directory, default, receiver, ds4_vision, or llama_cpp_mmproj");
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

bool ds4_hf_llama_primary_selectable(const char *selector,
                                     const char *receiver_path) {
    if (!selector || !selector[0] || !manifest_safe_path(receiver_path)) return false;
    const char *filename = manifest_basename(receiver_path);
    static const char *const excluded[] = {
        "mmproj", "imatrix", "mtp-", "eagle3-", "dflash-", "dspark-",
    };
    if (!manifest_suffix(filename, ".gguf") || manifest_split_gguf_name(filename)) return false;
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        if (strstr(filename, excluded[i])) return false;
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
