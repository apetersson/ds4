#include "ds4_hf.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
    if (xdg && path_join(path, sizeof(path), xdg, "huggingface/token") &&
        read_token_file(path, token)) return true;
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
