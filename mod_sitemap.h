#ifndef MOD_SITEMAP_H
#define MOD_SITEMAP_H

#include "httpd.h"
#include "apr_optional.h"
#include "apr_time.h"

#define MOD_SITEMAP_VERSION "1.0.1"

/*
 * Notify mod_sitemap that a configured dynamic URI changed. Pass APR_TIME_C(-1)
 * to use the current time. The update is persisted for cross-process MPMs.
 */
APR_DECLARE_OPTIONAL_FN(apr_status_t, sitemap_notify,
                        (server_rec *server, const char *uri,
                         apr_time_t modified));

#endif
