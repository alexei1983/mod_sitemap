/*
 * Example HTTP-to-optional-function bridge for PHP, Python, CGI, and other
 * non-native applications. This is intentionally a separate module so sites
 * that do not need an HTTP notification surface do not expose one.
 */
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"
#include "apr_optional.h"
#include "apr_strings.h"
#include "apr_tables.h"
#include "apr_time.h"
#include "mod_sitemap.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

module AP_MODULE_DECLARE_DATA sitemap_notify_http_module;

static APR_OPTIONAL_FN_TYPE(sitemap_notify) *notify_sitemap = NULL;

static int valid_uri_path(const char *uri)
{
    const unsigned char *cursor = (const unsigned char *)uri;
    if (!cursor || *cursor != '/') return 0;
    for (; *cursor; ++cursor)
        if (*cursor <= 0x20 || *cursor == 0x7f) return 0;
    return 1;
}

static int notify_handler(request_rec *request)
{
    apr_table_t *arguments = NULL;
    const char *uri;
    const char *modified_text;
    apr_time_t modified = APR_TIME_C(-1);
    apr_status_t rv;
    if (!request || !request->handler ||
        strcmp(request->handler, "sitemap-notify"))
        return DECLINED;
    if (!request->method || !request->headers_out || !request->pool ||
        !request->server) return HTTP_INTERNAL_SERVER_ERROR;
    if (strcmp(request->method, "POST")) {
        apr_table_setn(request->headers_out, "Allow", "POST");
        return HTTP_METHOD_NOT_ALLOWED;
    }
    if (!notify_sitemap) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, request,
                      "mod_sitemap_notify_http: sitemap_notify is unavailable");
        return HTTP_SERVICE_UNAVAILABLE;
    }
    ap_args_to_table(request, &arguments);
    if (!arguments) return HTTP_BAD_REQUEST;
    uri = apr_table_get(arguments, "uri");
    modified_text = apr_table_get(arguments, "modified");
    if (!valid_uri_path(uri) || strstr(uri, "://"))
        return HTTP_BAD_REQUEST;
    if (modified_text && *modified_text) {
        char *end = NULL;
        apr_int64_t seconds;
        errno = 0;
        seconds = (apr_int64_t)strtoll(modified_text, &end, 10);
        if (errno || end == modified_text || *end || seconds <= 0 ||
            seconds > APR_INT64_MAX / APR_USEC_PER_SEC)
            return HTTP_BAD_REQUEST;
        modified = apr_time_from_sec(seconds);
    }
    rv = notify_sitemap(request->server, uri, modified);
    if (rv == APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv != APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, request,
                      "mod_sitemap_notify_http: notification failed for %s", uri);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    request->status = HTTP_NO_CONTENT;
    return OK;
}

static int bridge_post_config(apr_pool_t *configuration, apr_pool_t *log_pool,
                              apr_pool_t *temporary, server_rec *server)
{
    (void)configuration; (void)log_pool; (void)temporary;
    notify_sitemap = APR_RETRIEVE_OPTIONAL_FN(sitemap_notify);
    if (!notify_sitemap && server)
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, server,
                     "mod_sitemap_notify_http: mod_sitemap optional function is unavailable");
    return OK;
}

static void register_hooks(apr_pool_t *pool)
{
    (void)pool;
    ap_hook_post_config(bridge_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(notify_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA sitemap_notify_http_module = {
    STANDARD20_MODULE_STUFF,
    NULL, NULL, NULL, NULL, NULL, register_hooks
};
