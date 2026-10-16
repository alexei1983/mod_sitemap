#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "mod_sitemap.c").read_text()
public = (root / "mod_sitemap.h").read_text()

assert "APR_DECLARE_OPTIONAL_FN(apr_status_t, sitemap_notify" in public
assert "APR_REGISTER_OPTIONAL_FN(sitemap_notify)" in source
assert "apr_stat(&info, entry->path" in source
assert "APR_FINFO_TYPE | APR_FINFO_MTIME" in source
assert "apr_strftime(buffer, &length" in source
assert "apr_strftime(buffer, NULL" not in source
assert "ap_hook_log_transaction(observe_request" in source
assert "Last-Modified" in source
assert "BEGIN" not in source  # no hidden database dependency
assert "SitemapURL" in source
assert "SitemapComposite" in source
assert "SITEMAP_SOURCE_COMPOSITE" in source
assert "notified > latest ? notified : latest" in source
assert "SitemapLastModified" in source
assert "parse_explicit_lastmod" in source
assert "return overrides[i].modified" in source
bridge = (root / "examples" / "mod_sitemap_notify_http.c").read_text()
assert "APR_RETRIEVE_OPTIONAL_FN(sitemap_notify)" in bridge
assert 'strcmp(request->method, "POST")' in bridge
assert "Require local" in (root / "examples" / "sitemap-notify-http.conf").read_text()
assert "notifySitemapChanged" in (root / "examples" / "notify_sitemap.php").read_text()
assert "notify_sitemap_changed" in (root / "examples" / "notify_sitemap.py").read_text()
assert "RSRC_CONF | OR_FILEINFO" in source
assert "SitemapChangeFrequency" in source
assert "SitemapPriority" in source
assert "<changefreq>%s</changefreq>" in source
assert "<priority>%s</priority>" in source
assert "source == SITEMAP_SOURCE_FILE || source == SITEMAP_SOURCE_DIRECTORY" in source
assert "valid_url_text" in source
assert "cursor[3] >= 0x80 && cursor[3] <= 0xbf" in source
assert "size > (apr_size_t)-1 - addition - 1" in source
assert "remaining > (apr_size_t)UINT_MAX" in source
assert "apr_file_write_full(file, text, length, &written)" in source
assert "written != length" in source
assert "count >= sizeof(buffer) - 1" in source
assert "apr_status_t unlock_rv" in source
assert "if (!request || !request->server" in source
assert "if (!server || !uri || !server->process" in source
assert "rendered = apr_array_make" in source
assert "seconds > APR_INT64_MAX / APR_USEC_PER_SEC" in bridge
print("contract tests: ok")
