#include "mod_sitemap.h"

#include "apr_file_io.h"
#include "apr_date.h"
#include "apr_lib.h"
#include "apr_sha1.h"
#include "apr_strings.h"
#include "apr_time.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "httpd.h"
#include "util_script.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>

module AP_MODULE_DECLARE_DATA sitemap_module;

typedef enum {
    SITEMAP_SOURCE_DYNAMIC = 0,
    SITEMAP_SOURCE_FILE = 1,
    SITEMAP_SOURCE_DIRECTORY = 2,
    SITEMAP_SOURCE_COMPOSITE = 3
} sitemap_source;

typedef struct {
    const char *uri;
    const char *path;
    apr_array_header_t *paths;
    sitemap_source source;
    int observe;
} sitemap_entry;

typedef struct {
    const char *uri;
    apr_time_t modified;
} sitemap_lastmod_override;

typedef struct {
    const char *uri;
    const char *change_frequency;
    const char *priority;
} sitemap_metadata;

typedef struct {
    int enabled;
    const char *endpoint;
    const char *base_url;
    const char *state_dir;
    apr_array_header_t *entries;
    apr_array_header_t *overrides;
    apr_array_header_t *metadata;
} sitemap_cfg;

static void *create_server_cfg(apr_pool_t *pool, server_rec *server)
{
    sitemap_cfg *cfg;
    (void)server;
    if (!pool) return NULL;
    cfg = apr_pcalloc(pool, sizeof(*cfg));
    if (!cfg) return NULL;
    cfg->enabled = 0;
    cfg->endpoint = "/sitemap.xml";
    cfg->state_dir = "logs/mod_sitemap";
    cfg->entries = apr_array_make(pool, 16, sizeof(sitemap_entry));
    cfg->overrides = apr_array_make(pool, 8, sizeof(sitemap_lastmod_override));
    cfg->metadata = apr_array_make(pool, 8, sizeof(sitemap_metadata));
    if (!cfg->entries || !cfg->overrides || !cfg->metadata) return NULL;
    return cfg;
}

static sitemap_cfg *command_cfg(cmd_parms *cmd)
{
    if (!cmd || !cmd->pool || !cmd->server || !cmd->server->module_config)
        return NULL;
    return ap_get_module_config(cmd->server->module_config, &sitemap_module);
}

static int valid_url_text(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    if (!cursor) return 0;
    while (*cursor) {
        if (*cursor <= 0x20 || *cursor == 0x7f) return 0;
        if (*cursor < 0x80) {
            ++cursor;
        }
        else if (*cursor >= 0xc2 && *cursor <= 0xdf &&
                 cursor[1] >= 0x80 && cursor[1] <= 0xbf) {
            cursor += 2;
        }
        else if (*cursor == 0xe0 &&
                 cursor[1] >= 0xa0 && cursor[1] <= 0xbf &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf) {
            cursor += 3;
        }
        else if (((*cursor >= 0xe1 && *cursor <= 0xec) ||
                  (*cursor >= 0xee && *cursor <= 0xef)) &&
                 cursor[1] >= 0x80 && cursor[1] <= 0xbf &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf) {
            cursor += 3;
        }
        else if (*cursor == 0xed &&
                 cursor[1] >= 0x80 && cursor[1] <= 0x9f &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf) {
            cursor += 3;
        }
        else if (*cursor == 0xf0 &&
                 cursor[1] >= 0x90 && cursor[1] <= 0xbf &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf &&
                 cursor[3] >= 0x80 && cursor[3] <= 0xbf) {
            cursor += 4;
        }
        else if (*cursor >= 0xf1 && *cursor <= 0xf3 &&
                 cursor[1] >= 0x80 && cursor[1] <= 0xbf &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf &&
                 cursor[3] >= 0x80 && cursor[3] <= 0xbf) {
            cursor += 4;
        }
        else if (*cursor == 0xf4 &&
                 cursor[1] >= 0x80 && cursor[1] <= 0x8f &&
                 cursor[2] >= 0x80 && cursor[2] <= 0xbf &&
                 cursor[3] >= 0x80 && cursor[3] <= 0xbf) {
            cursor += 4;
        }
        else return 0;
    }
    return 1;
}

static int valid_uri_path(const char *value)
{
    return value && value[0] == '/' && valid_url_text(value);
}

static const char *set_flag(cmd_parms *cmd, void *dummy, int enabled)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    (void)dummy;
    if (!cfg) return "SitemapEngine: module configuration is unavailable";
    cfg->enabled = enabled;
    return NULL;
}

static const char *set_endpoint(cmd_parms *cmd, void *dummy, const char *value)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    (void)dummy;
    if (!cfg) return "SitemapEndpoint: module configuration is unavailable";
    if (!valid_uri_path(value) || strchr(value, '?') || strchr(value, '#'))
        return "SitemapEndpoint must be an absolute URI path without query or fragment";
    cfg->endpoint = apr_pstrdup(cmd->pool, value);
    if (!cfg->endpoint) return "SitemapEndpoint: allocation failed";
    return NULL;
}

static const char *set_base_url(cmd_parms *cmd, void *dummy, const char *value)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    (void)dummy;
    if (!cfg) return "SitemapBaseURL: module configuration is unavailable";
    if (!value || (strncmp(value, "http://", 7) && strncmp(value, "https://", 8)))
        return "SitemapBaseURL must begin with http:// or https://";
    {
        apr_size_t length = strlen(value);
        apr_size_t scheme_length = !strncmp(value, "https://", 8) ? 8 : 7;
        if (length <= scheme_length || strchr(value + scheme_length, '?') ||
            strchr(value + scheme_length, '#') ||
            value[scheme_length] == '/')
            return "SitemapBaseURL requires a host and must not contain a query or fragment";
        if (!valid_url_text(value))
            return "SitemapBaseURL must be valid UTF-8 without spaces or control characters";
        while (length > 8 && value[length - 1] == '/') --length;
        cfg->base_url = apr_pstrndup(cmd->pool, value, length);
        if (!cfg->base_url) return "SitemapBaseURL: allocation failed";
    }
    return NULL;
}

static const char *set_state_dir(cmd_parms *cmd, void *dummy, const char *value)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    (void)dummy;
    if (!cfg) return "SitemapStateDirectory: module configuration is unavailable";
    if (!value || !*value) return "SitemapStateDirectory must not be empty";
    cfg->state_dir = apr_pstrdup(cmd->pool, value);
    if (!cfg->state_dir) return "SitemapStateDirectory: allocation failed";
    return NULL;
}

static const char *add_entry(cmd_parms *cmd, sitemap_source source,
                             const char *uri, const char *path, int observe)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    sitemap_entry *entry;
    const char *saved_uri;
    const char *saved_path = NULL;
    int i;
    sitemap_entry *existing;
    if (!cfg || !cfg->entries) return "sitemap entry: module configuration is unavailable";
    existing = (sitemap_entry *)cfg->entries->elts;
    if (!valid_uri_path(uri)) return "sitemap entry URI must begin with / and contain no control characters";
    if ((source == SITEMAP_SOURCE_FILE || source == SITEMAP_SOURCE_DIRECTORY) &&
        (!path || path[0] != '/'))
        return "sitemap filesystem sources must use an absolute path";
    for (i = 0; i < cfg->entries->nelts; ++i)
        if (existing[i].uri && !strcmp(existing[i].uri, uri))
            return apr_psprintf(cmd->pool, "duplicate sitemap URI: %s", uri);
    saved_uri = apr_pstrdup(cmd->pool, uri);
    if (path) saved_path = apr_pstrdup(cmd->pool, path);
    if (!saved_uri || (path && !saved_path))
        return "sitemap entry: allocation failed";
    entry = apr_array_push(cfg->entries);
    if (!entry) return "sitemap entry: allocation failed";
    entry->uri = saved_uri;
    entry->path = saved_path;
    entry->paths = NULL;
    entry->source = source;
    entry->observe = observe;
    return NULL;
}

static const char *add_file(cmd_parms *cmd, void *dummy,
                            const char *uri, const char *path)
{ (void)dummy; return add_entry(cmd, SITEMAP_SOURCE_FILE, uri, path, 0); }

static const char *add_directory(cmd_parms *cmd, void *dummy,
                                 const char *uri, const char *path)
{ (void)dummy; return add_entry(cmd, SITEMAP_SOURCE_DIRECTORY, uri, path, 0); }

static const char *add_location(cmd_parms *cmd, void *dummy, const char *uri)
{ (void)dummy; return add_entry(cmd, SITEMAP_SOURCE_DYNAMIC, uri, NULL, 1); }

static apr_status_t parse_explicit_lastmod(const char *value, apr_time_t *result)
{
    apr_time_exp_t exploded;
    apr_time_exp_t verified;
    apr_status_t rv;
    int year, month, day, hour = 0, minute = 0, second = 0;
    apr_size_t length;
    if (!value || !result) return APR_EINVAL;
    length = strlen(value);
    {
        apr_size_t i;
        for (i = 0; i < length; ++i) {
            if (i == 4 || i == 7 || (length == 20 &&
                (i == 10 || i == 13 || i == 16 || i == 19))) continue;
            if (value[i] < '0' || value[i] > '9') return APR_EINVAL;
        }
    }
    if (length == 10) {
        if (sscanf(value, "%4d-%2d-%2d", &year, &month, &day) != 3 ||
            value[4] != '-' || value[7] != '-') return APR_EINVAL;
    }
    else if (length == 20) {
        if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2dZ",
                   &year, &month, &day, &hour, &minute, &second) != 6 ||
            value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
            value[13] != ':' || value[16] != ':' || value[19] != 'Z')
            return APR_EINVAL;
    }
    else return APR_EINVAL;
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) return APR_EINVAL;
    memset(&exploded, 0, sizeof(exploded));
    exploded.tm_year = year - 1900;
    exploded.tm_mon = month - 1;
    exploded.tm_mday = day;
    exploded.tm_hour = hour;
    exploded.tm_min = minute;
    exploded.tm_sec = second;
    rv = apr_time_exp_gmt_get(result, &exploded);
    if (rv != APR_SUCCESS || apr_time_exp_gmt(&verified, *result) != APR_SUCCESS)
        return APR_EINVAL;
    if (verified.tm_year != exploded.tm_year || verified.tm_mon != exploded.tm_mon ||
        verified.tm_mday != exploded.tm_mday || verified.tm_hour != exploded.tm_hour ||
        verified.tm_min != exploded.tm_min || verified.tm_sec != exploded.tm_sec)
        return APR_EINVAL;
    return APR_SUCCESS;
}

static const char *set_explicit_lastmod(cmd_parms *cmd, void *dummy,
                                        const char *uri, const char *value)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    sitemap_lastmod_override *items;
    sitemap_lastmod_override *item;
    apr_time_t modified;
    const char *saved_uri;
    int i;
    (void)dummy;
    if (!cfg || !cfg->overrides)
        return "SitemapLastModified: module configuration is unavailable";
    items = (sitemap_lastmod_override *)cfg->overrides->elts;
    if (!valid_uri_path(uri))
        return "SitemapLastModified URI must begin with / and contain no control characters";
    if (parse_explicit_lastmod(value, &modified) != APR_SUCCESS)
        return "SitemapLastModified must use YYYY-MM-DD or YYYY-MM-DDTHH:MM:SSZ";
    for (i = 0; i < cfg->overrides->nelts; ++i)
        if (items[i].uri && !strcmp(items[i].uri, uri))
            return apr_psprintf(cmd->pool,
                                "duplicate SitemapLastModified URI: %s", uri);
    saved_uri = apr_pstrdup(cmd->pool, uri);
    if (!saved_uri) return "SitemapLastModified: allocation failed";
    item = apr_array_push(cfg->overrides);
    if (!item) return "SitemapLastModified: allocation failed";
    item->uri = saved_uri;
    item->modified = modified;
    return NULL;
}

static sitemap_metadata *metadata_for(cmd_parms *cmd, sitemap_cfg *cfg,
                                      const char *uri)
{
    sitemap_metadata *items;
    sitemap_metadata *item;
    const char *saved_uri;
    int i;
    if (!cmd || !cmd->pool || !cfg || !cfg->metadata || !uri) return NULL;
    items = (sitemap_metadata *)cfg->metadata->elts;
    for (i = 0; i < cfg->metadata->nelts; ++i)
        if (items[i].uri && !strcmp(items[i].uri, uri)) return &items[i];
    saved_uri = apr_pstrdup(cmd->pool, uri);
    if (!saved_uri) return NULL;
    item = apr_array_push(cfg->metadata);
    if (!item) return NULL;
    item->uri = saved_uri;
    item->change_frequency = NULL;
    item->priority = NULL;
    return item;
}

static const char *set_change_frequency(cmd_parms *cmd, void *dummy,
                                        const char *uri, const char *value)
{
    static const char *allowed[] = {
        "always", "hourly", "daily", "weekly", "monthly", "yearly", "never"
    };
    sitemap_cfg *cfg = command_cfg(cmd);
    sitemap_metadata *item;
    int i, valid = 0;
    (void)dummy;
    if (!cfg || !cfg->metadata)
        return "SitemapChangeFrequency: module configuration is unavailable";
    if (!valid_uri_path(uri))
        return "SitemapChangeFrequency URI must begin with / and contain no control characters";
    if (!value) return "SitemapChangeFrequency requires a value";
    for (i = 0; i < (int)(sizeof(allowed) / sizeof(allowed[0])); ++i)
        if (!strcasecmp(value, allowed[i])) { value = allowed[i]; valid = 1; break; }
    if (!valid)
        return "SitemapChangeFrequency must be always, hourly, daily, weekly, monthly, yearly, or never";
    item = metadata_for(cmd, cfg, uri);
    if (!item) return "SitemapChangeFrequency: allocation failed";
    if (item->change_frequency)
        return apr_psprintf(cmd->pool,
                            "duplicate SitemapChangeFrequency URI: %s", uri);
    item->change_frequency = apr_pstrdup(cmd->pool, value);
    return item->change_frequency ? NULL :
           "SitemapChangeFrequency: allocation failed";
}

static const char *set_priority(cmd_parms *cmd, void *dummy,
                                const char *uri, const char *value)
{
    sitemap_cfg *cfg = command_cfg(cmd);
    sitemap_metadata *item;
    char *end = NULL;
    double parsed;
    char normalized[16];
    char *tail;
    int normalized_length;
    (void)dummy;
    if (!cfg || !cfg->metadata)
        return "SitemapPriority: module configuration is unavailable";
    if (!valid_uri_path(uri))
        return "SitemapPriority URI must begin with / and contain no control characters";
    if (!value) return "SitemapPriority requires a value";
    errno = 0;
    parsed = strtod(value, &end);
    if (errno || end == value || *end || parsed != parsed || parsed < 0.0 || parsed > 1.0)
        return "SitemapPriority must be a number from 0.0 through 1.0";
    normalized_length = apr_snprintf(normalized, sizeof(normalized), "%.3f", parsed);
    if (normalized_length < 0 || (apr_size_t)normalized_length >= sizeof(normalized))
        return "SitemapPriority could not be normalized";
    tail = normalized + strlen(normalized) - 1;
    while (tail > normalized && *tail == '0' && tail[-1] != '.') *tail-- = '\0';
    item = metadata_for(cmd, cfg, uri);
    if (!item) return "SitemapPriority: allocation failed";
    if (item->priority)
        return apr_psprintf(cmd->pool, "duplicate SitemapPriority URI: %s", uri);
    item->priority = apr_pstrdup(cmd->pool, normalized);
    return item->priority ? NULL : "SitemapPriority: allocation failed";
}

static const char *add_composite(cmd_parms *cmd, void *dummy, const char *args)
{
    const char *cursor = args;
    const char *uri;
    const char *path;
    const char *error;
    sitemap_cfg *cfg;
    sitemap_entry *entry;
    apr_array_header_t *paths;
    (void)dummy;
    if (!cmd || !cmd->pool || !args)
        return "SitemapComposite requires a public URI and one or more paths";
    uri = ap_getword_conf(cmd->pool, &cursor);
    if (!uri || !*uri)
        return "SitemapComposite requires a public URI and one or more paths";
    if (!valid_uri_path(uri))
        return "SitemapComposite URI must begin with / and contain no control characters";
    paths = apr_array_make(cmd->pool, 4, sizeof(const char *));
    if (!paths) return "SitemapComposite: allocation failed";
    while (cursor && *cursor) {
        const char **slot;
        path = ap_getword_conf(cmd->pool, &cursor);
        if (!path || !*path) continue;
        if (path[0] != '/')
            return "SitemapComposite paths must be absolute";
        slot = apr_array_push(paths);
        if (!slot) return "SitemapComposite: allocation failed";
        *slot = apr_pstrdup(cmd->pool, path);
        if (!*slot) return "SitemapComposite: allocation failed";
    }
    if (!paths->nelts)
        return "SitemapComposite requires at least one filesystem path";
    error = add_entry(cmd, SITEMAP_SOURCE_COMPOSITE, uri, NULL, 1);
    if (error) return error;
    cfg = command_cfg(cmd);
    if (!cfg || !cfg->entries || cfg->entries->nelts < 1)
        return "SitemapComposite: module configuration is unavailable";
    entry = &((sitemap_entry *)cfg->entries->elts)[cfg->entries->nelts - 1];
    entry->paths = paths;
    return NULL;
}

/* Usable inside <Location> or <Directory>. The URI stays explicit because a
 * filesystem directory does not always map uniquely to a public URL. */
static const char *add_context_url(cmd_parms *cmd, void *dummy,
                                   const char *uri, const char *mode)
{
    sitemap_source source = SITEMAP_SOURCE_DYNAMIC;
    const char *path = NULL;
    int observe = 1;
    (void)dummy;
    if (!cmd) return "SitemapURL: command context is unavailable";
    if (mode && !strcasecmp(mode, "filesystem")) {
        if (!cmd->path) return "SitemapURL filesystem requires a <Directory> context";
        source = SITEMAP_SOURCE_DIRECTORY;
        path = cmd->path;
        observe = 0;
    }
    else if (mode && strcasecmp(mode, "dynamic"))
        return "SitemapURL mode must be dynamic or filesystem";
    return add_entry(cmd, source, uri, path, observe);
}

static const command_rec directives[] = {
    AP_INIT_FLAG("SitemapEngine", set_flag, NULL, RSRC_CONF, "Enable sitemap output"),
    AP_INIT_TAKE1("SitemapEndpoint", set_endpoint, NULL, RSRC_CONF, "Sitemap endpoint URI"),
    AP_INIT_TAKE1("SitemapBaseURL", set_base_url, NULL, RSRC_CONF, "Absolute public base URL"),
    AP_INIT_TAKE1("SitemapStateDirectory", set_state_dir, NULL, RSRC_CONF, "Writable notification state directory"),
    AP_INIT_TAKE2("SitemapFile", add_file, NULL, RSRC_CONF, "Public URI and absolute file path"),
    AP_INIT_TAKE2("SitemapDirectory", add_directory, NULL, RSRC_CONF, "Public URI and absolute directory path"),
    AP_INIT_TAKE1("SitemapLocation", add_location, NULL, RSRC_CONF, "Dynamic public URI"),
    AP_INIT_RAW_ARGS("SitemapComposite", add_composite, NULL, RSRC_CONF | OR_FILEINFO, "Public URI followed by one or more absolute paths"),
    AP_INIT_TAKE2("SitemapLastModified", set_explicit_lastmod, NULL, RSRC_CONF | OR_FILEINFO, "Public URI and explicit YYYY-MM-DD or UTC RFC3339 time"),
    AP_INIT_TAKE2("SitemapChangeFrequency", set_change_frequency, NULL, RSRC_CONF | OR_FILEINFO, "Public URI and Sitemap Protocol change frequency"),
    AP_INIT_TAKE2("SitemapPriority", set_priority, NULL, RSRC_CONF | OR_FILEINFO, "Public URI and priority from 0.0 through 1.0"),
    AP_INIT_TAKE12("SitemapURL", add_context_url, NULL, OR_FILEINFO, "Public URI [dynamic|filesystem]"),
    { NULL }
};

static const char *resolved_state_dir(apr_pool_t *pool, const sitemap_cfg *cfg)
{
    if (!pool || !cfg || !cfg->state_dir || !*cfg->state_dir) return NULL;
    if (cfg->state_dir[0] == '/') return cfg->state_dir;
    return ap_server_root_relative(pool, cfg->state_dir);
}

static const char *marker_path(apr_pool_t *pool, const sitemap_cfg *cfg,
                               const char *uri)
{
    apr_sha1_ctx_t context;
    unsigned char digest[APR_SHA1_DIGESTSIZE];
    char hex[APR_SHA1_DIGESTSIZE * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    const char *directory = resolved_state_dir(pool, cfg);
    const char *cursor;
    apr_size_t remaining;
    int i;
    if (!pool || !cfg || !uri || !directory) return NULL;
    apr_sha1_init(&context);
    cursor = uri;
    remaining = strlen(uri);
    while (remaining) {
        unsigned int chunk = remaining > (apr_size_t)UINT_MAX
                           ? UINT_MAX : (unsigned int)remaining;
        apr_sha1_update(&context, cursor, chunk);
        cursor += chunk;
        remaining -= chunk;
    }
    apr_sha1_final(digest, &context);
    for (i = 0; i < APR_SHA1_DIGESTSIZE; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[APR_SHA1_DIGESTSIZE * 2] = '\0';
    return apr_pstrcat(pool, directory, "/", hex, ".lastmod", NULL);
}

static apr_status_t persist_notification(server_rec *server, const char *uri,
                                         apr_time_t modified, apr_pool_t *pool)
{
    sitemap_cfg *cfg;
    const char *directory, *path, *text;
    apr_file_t *file = NULL;
    apr_size_t length;
    apr_off_t start = 0;
    apr_status_t rv;
    apr_size_t written = 0;
    int i, found = 0;
    sitemap_entry *entries;
    if (!server || !pool || !valid_uri_path(uri) || !server->module_config)
        return APR_EINVAL;
    cfg = ap_get_module_config(server->module_config, &sitemap_module);
    if (!cfg || !cfg->enabled || !cfg->entries) return APR_EGENERAL;
    entries = (sitemap_entry *)cfg->entries->elts;
    for (i = 0; i < cfg->entries->nelts; ++i)
        if (entries[i].observe && entries[i].uri &&
            !strcmp(entries[i].uri, uri)) {
            found = 1; break;
        }
    if (!found) return APR_NOTFOUND;
    if (modified == APR_TIME_C(-1)) modified = apr_time_now();
    else if (modified <= 0) return APR_EINVAL;
    directory = resolved_state_dir(pool, cfg);
    path = marker_path(pool, cfg, uri);
    if (!directory || !path) return APR_EINVAL;
    rv = apr_dir_make_recursive(directory, APR_OS_DEFAULT, pool);
    if (rv != APR_SUCCESS && !APR_STATUS_IS_EEXIST(rv)) return rv;
    rv = apr_file_open(&file, path,
                       APR_FOPEN_CREATE | APR_FOPEN_READ | APR_FOPEN_WRITE,
                       APR_OS_DEFAULT, pool);
    if (rv != APR_SUCCESS) return rv;
    rv = apr_file_lock(file, APR_FLOCK_EXCLUSIVE);
    if (rv != APR_SUCCESS) { apr_file_close(file); return rv; }
    {
        char existing[64];
        apr_size_t count = sizeof(existing) - 1;
        char *end = NULL;
        apr_int64_t prior = 0;
        rv = apr_file_read(file, existing, &count);
        if (rv != APR_SUCCESS && !APR_STATUS_IS_EOF(rv)) {
            (void)apr_file_unlock(file);
            (void)apr_file_close(file);
            return rv;
        }
        if (count == sizeof(existing) - 1) {
            /* A valid APR timestamp is much shorter. Treat a full buffer as
             * truncated/corrupt instead of accepting an unseen suffix. */
        }
        else if (rv == APR_SUCCESS || (APR_STATUS_IS_EOF(rv) && count > 0)) {
            existing[count] = '\0';
            errno = 0;
            prior = (apr_int64_t)strtoll(existing, &end, 10);
            while (end && (*end == ' ' || *end == '\t' || *end == '\r' ||
                           *end == '\n')) ++end;
            if (!errno && end != existing && end && !*end && prior > 0 &&
                prior > (apr_int64_t)modified)
                modified = (apr_time_t)prior;
        }
    }
    rv = apr_file_trunc(file, 0);
    if (rv == APR_SUCCESS) rv = apr_file_seek(file, APR_SET, &start);
    text = apr_psprintf(pool, "%" APR_TIME_T_FMT "\n", modified);
    if (!text) rv = APR_ENOMEM;
    length = text ? strlen(text) : 0;
    if (rv == APR_SUCCESS)
        rv = apr_file_write_full(file, text, length, &written);
    if (rv == APR_SUCCESS && written != length) rv = APR_EGENERAL;
    {
        apr_status_t unlock_rv = apr_file_unlock(file);
        apr_status_t close_rv = apr_file_close(file);
        if (rv == APR_SUCCESS && unlock_rv != APR_SUCCESS) rv = unlock_rv;
        if (rv == APR_SUCCESS && close_rv != APR_SUCCESS) rv = close_rv;
    }
    return rv;
}

static apr_status_t sitemap_notify(server_rec *server, const char *uri,
                                   apr_time_t modified)
{
    apr_pool_t *pool = NULL;
    apr_status_t rv;
    if (!server || !uri || !server->process || !server->process->pool)
        return APR_EINVAL;
    rv = apr_pool_create(&pool, server->process->pool);
    if (rv != APR_SUCCESS) return rv;
    rv = persist_notification(server, uri, modified, pool);
    if (pool) apr_pool_destroy(pool);
    return rv;
}

static apr_time_t marker_time(apr_pool_t *pool, const sitemap_cfg *cfg,
                              const char *uri)
{
    apr_file_t *file = NULL;
    char buffer[64];
    apr_size_t count = sizeof(buffer) - 1;
    char *end = NULL;
    apr_int64_t value;
    const char *path;
    apr_status_t rv;
    if (!pool || !cfg || !uri) return 0;
    path = marker_path(pool, cfg, uri);
    if (!path || apr_file_open(&file, path, APR_FOPEN_READ, APR_OS_DEFAULT, pool) != APR_SUCCESS)
        return 0;
    rv = apr_file_read(file, buffer, &count);
    if ((rv != APR_SUCCESS && !APR_STATUS_IS_EOF(rv)) || count == 0) {
        apr_file_close(file); return 0;
    }
    apr_file_close(file);
    if (count >= sizeof(buffer) - 1) return 0;
    buffer[count] = '\0';
    errno = 0;
    value = (apr_int64_t)strtoll(buffer, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' ||
                   *end == '\n')) ++end;
    if (errno || end == buffer || !end || *end || value <= 0) return 0;
    return (apr_time_t)value;
}

static const char *xml_escape(apr_pool_t *pool, const char *input)
{
    const unsigned char *p;
    char *result, *out;
    apr_size_t size = 0;
    if (!pool || !input) return NULL;
    for (p = (const unsigned char *)input; *p; ++p) {
        apr_size_t addition = *p == '&' ? 5 :
            (*p == '<' || *p == '>' ? 4 :
            (*p == '"' || *p == '\'' ? 6 : 1));
        if (size > (apr_size_t)-1 - addition - 1) return NULL;
        size += addition;
    }
    result = out = apr_palloc(pool, size + 1);
    if (!result) return NULL;
    for (p = (const unsigned char *)input; *p; ++p) {
        if (*p == '&') { memcpy(out, "&amp;", 5); out += 5; }
        else if (*p == '<') { memcpy(out, "&lt;", 4); out += 4; }
        else if (*p == '>') { memcpy(out, "&gt;", 4); out += 4; }
        else if (*p == '"') { memcpy(out, "&quot;", 6); out += 6; }
        else if (*p == '\'') { memcpy(out, "&apos;", 6); out += 6; }
        else *out++ = (char)*p;
    }
    *out = '\0';
    return result;
}

static const char *format_time(apr_pool_t *pool, apr_time_t value)
{
    apr_time_exp_t exploded;
    apr_size_t length = 0;
    char buffer[32];
    if (!pool || !value || apr_time_exp_gmt(&exploded, value) != APR_SUCCESS)
        return NULL;
    if (apr_strftime(buffer, &length, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ",
                     &exploded) != APR_SUCCESS || !length) return NULL;
    return apr_pstrdup(pool, buffer);
}

static apr_time_t entry_time(request_rec *request, const sitemap_cfg *cfg,
                             const sitemap_entry *entry)
{
    apr_finfo_t info;
    apr_int32_t wanted = APR_FINFO_TYPE | APR_FINFO_MTIME;
    apr_time_t latest = 0;
    int i;
    sitemap_lastmod_override *overrides;
    if (!request || !request->pool || !cfg || !entry || !entry->uri ||
        !cfg->overrides) return 0;
    overrides = (sitemap_lastmod_override *)cfg->overrides->elts;
    for (i = 0; i < cfg->overrides->nelts; ++i)
        if (overrides[i].uri && !strcmp(overrides[i].uri, entry->uri))
            return overrides[i].modified;
    if (entry->source == SITEMAP_SOURCE_DYNAMIC)
        return marker_time(request->pool, cfg, entry->uri);
    if (entry->source == SITEMAP_SOURCE_COMPOSITE) {
        if (!entry->paths) return marker_time(request->pool, cfg, entry->uri);
        const char **paths = (const char **)entry->paths->elts;
        for (i = 0; i < entry->paths->nelts; ++i)
            if (paths[i] && apr_stat(&info, paths[i], wanted, request->pool) == APR_SUCCESS &&
                (info.filetype == APR_REG || info.filetype == APR_DIR) &&
                info.mtime > latest)
                latest = info.mtime;
        {
            apr_time_t notified = marker_time(request->pool, cfg, entry->uri);
            return notified > latest ? notified : latest;
        }
    }
    if (!entry->path || apr_stat(&info, entry->path, wanted, request->pool) != APR_SUCCESS)
        return 0;
    if (entry->source == SITEMAP_SOURCE_FILE && info.filetype != APR_REG) return 0;
    if (entry->source == SITEMAP_SOURCE_DIRECTORY && info.filetype != APR_DIR) return 0;
    return info.mtime;
}

static const sitemap_metadata *entry_metadata(const sitemap_cfg *cfg,
                                              const char *uri)
{
    sitemap_metadata *items;
    int i;
    if (!cfg || !cfg->metadata || !uri) return NULL;
    items = (sitemap_metadata *)cfg->metadata->elts;
    for (i = 0; i < cfg->metadata->nelts; ++i)
        if (items[i].uri && !strcmp(items[i].uri, uri)) return &items[i];
    return NULL;
}

static const char *base_url(request_rec *request, const sitemap_cfg *cfg)
{
    if (!request || !request->pool || !cfg) return NULL;
    if (cfg->base_url && *cfg->base_url) return cfg->base_url;
    return ap_construct_url(request->pool, "", request);
}

static int sitemap_handler(request_rec *request)
{
    typedef struct {
        const char *location;
        const char *stamp;
        const sitemap_metadata *metadata;
    } rendered_entry;
    sitemap_cfg *cfg;
    sitemap_entry *entries;
    apr_array_header_t *rendered;
    const char *base;
    int i;
    if (!request || !request->server || !request->server->module_config ||
        !request->pool || !request->uri || !request->method ||
        !request->headers_out)
        return DECLINED;
    cfg = ap_get_module_config(request->server->module_config,&sitemap_module);
    if (!cfg || !cfg->enabled || !cfg->endpoint ||
        strcmp(request->uri, cfg->endpoint)) return DECLINED;
    if (strcmp(request->method, "GET") && strcmp(request->method, "HEAD"))
        return HTTP_METHOD_NOT_ALLOWED;
    base = base_url(request, cfg);
    if (!base || !cfg->entries) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, request,
                      "mod_sitemap: sitemap configuration is incomplete");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    ap_set_content_type(request, "application/xml; charset=utf-8");
    apr_table_setn(request->headers_out, "Cache-Control", "no-cache");
    if (request->header_only) return OK;
    rendered = apr_array_make(request->pool,
                              cfg->entries->nelts > 0 ? cfg->entries->nelts : 1,
                              sizeof(rendered_entry));
    if (!rendered) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, request,
                      "mod_sitemap: cannot allocate rendered-entry array");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    entries = (sitemap_entry *)cfg->entries->elts;
    for (i = 0; i < cfg->entries->nelts; ++i) {
        const char *escaped_location;
        apr_time_t modified = entry_time(request, cfg, &entries[i]);
        const char *stamp = format_time(request->pool, modified);
        const char *location;
        const sitemap_metadata *metadata = entry_metadata(cfg, entries[i].uri);
        rendered_entry *output;
        if (!entries[i].uri) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, request,
                          "mod_sitemap: sitemap entry %d has no URI", i);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        location = apr_pstrcat(request->pool, base, entries[i].uri, NULL);
        escaped_location = location ? xml_escape(request->pool, location) : NULL;
        if (!escaped_location) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, request,
                          "mod_sitemap: cannot construct XML location for entry %d", i);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        if (modified && !stamp) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, request,
                          "mod_sitemap: cannot format timestamp for entry %d", i);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        output = apr_array_push(rendered);
        if (!output) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_ENOMEM, request,
                          "mod_sitemap: cannot append rendered entry %d", i);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        output->location = escaped_location;
        output->stamp = stamp;
        output->metadata = metadata;
    }
    ap_rputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", request);
    ap_rputs("<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n", request);
    {
        rendered_entry *output = (rendered_entry *)rendered->elts;
        for (i = 0; i < rendered->nelts; ++i) {
            const sitemap_metadata *metadata = output[i].metadata;
            ap_rprintf(request, "  <url><loc>%s</loc>", output[i].location);
            if (output[i].stamp)
                ap_rprintf(request, "<lastmod>%s</lastmod>", output[i].stamp);
            if (metadata && metadata->change_frequency)
            ap_rprintf(request, "<changefreq>%s</changefreq>",
                       metadata->change_frequency);
            if (metadata && metadata->priority)
                ap_rprintf(request, "<priority>%s</priority>", metadata->priority);
            ap_rputs("</url>\n", request);
        }
    }
    ap_rputs("</urlset>\n", request);
    return OK;
}

static int successful_mutation(const request_rec *request)
{
    if (!request || !request->method) return 0;
    if (request->status < 200 || request->status >= 400) return 0;
    return !strcmp(request->method, "POST") || !strcmp(request->method, "PUT") ||
           !strcmp(request->method, "PATCH") || !strcmp(request->method, "DELETE");
}

static int observe_request(request_rec *request)
{
    sitemap_cfg *cfg;
    sitemap_entry *entries;
    const char *header;
    apr_time_t modified = 0;
    int i;
    if (!request || !request->server || !request->server->module_config ||
        !request->pool || !request->uri) return DECLINED;
    cfg = ap_get_module_config(request->server->module_config,&sitemap_module);
    if (!cfg || !cfg->enabled || !cfg->entries || request->main) return DECLINED;
    header = request->headers_out
           ? apr_table_get(request->headers_out, "Last-Modified") : NULL;
    if (!header && request->err_headers_out)
        header = apr_table_get(request->err_headers_out, "Last-Modified");
    if (header) modified = apr_date_parse_http(header);
    if (!modified && successful_mutation(request)) modified = apr_time_now();
    if (!modified) return DECLINED;
    entries = (sitemap_entry *)cfg->entries->elts;
    for (i = 0; i < cfg->entries->nelts; ++i) {
        apr_status_t rv;
        if (!entries[i].observe || !entries[i].uri ||
            strcmp(entries[i].uri, request->uri)) continue;
        rv = persist_notification(request->server, entries[i].uri, modified,
                                  request->pool);
        if (rv != APR_SUCCESS)
            ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, request,
                          "mod_sitemap: cannot persist lastmod for %s",
                          entries[i].uri);
    }
    return DECLINED;
}

static int post_config(apr_pool_t *configuration, apr_pool_t *log_pool,
                       apr_pool_t *temporary, server_rec *server)
{
    server_rec *current;
    (void)log_pool; (void)temporary;
    if (!configuration || !server) return HTTP_INTERNAL_SERVER_ERROR;
    APR_REGISTER_OPTIONAL_FN(sitemap_notify);
    for (current = server; current; current = current->next) {
        sitemap_cfg *cfg = current->module_config
                         ? ap_get_module_config(current->module_config,
                                                &sitemap_module) : NULL;
        const char *directory;
        apr_status_t rv;
        if (!cfg || !cfg->enabled) continue;
        directory = resolved_state_dir(configuration, cfg);
        if (!directory) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, current,
                         "mod_sitemap: SitemapStateDirectory is missing or invalid");
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        rv = apr_dir_make_recursive(directory, APR_OS_DEFAULT, configuration);
        if (rv != APR_SUCCESS && !APR_STATUS_IS_EEXIST(rv)) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, current,
                         "mod_sitemap: cannot create state directory %s", directory);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    return OK;
}

static void register_hooks(apr_pool_t *pool)
{
    (void)pool;
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(sitemap_handler, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_log_transaction(observe_request, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA sitemap_module = {
    STANDARD20_MODULE_STUFF,
    NULL,
    NULL,
    create_server_cfg,
    NULL,
    directives,
    register_hooks
};
