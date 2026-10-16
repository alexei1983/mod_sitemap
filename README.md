# mod_sitemap 1.0.1

`mod_sitemap` generates a Sitemap Protocol XML document from explicitly
configured static files, filesystem directories, and dynamic URLs.

## Build

```sh
make
sudo make install
```

The module targets Apache HTTP Server 2.4 and uses only APR/APR-util facilities.

## Configuration

```apache
SitemapEngine On
SitemapEndpoint /sitemap.xml
SitemapBaseURL https://www.example.com
SitemapStateDirectory /var/lib/mod_sitemap

SitemapFile /index.html /var/www/html/index.html
SitemapDirectory /docs/ /var/www/html/docs
SitemapLocation /reports
SitemapComposite /application /srv/app/app.py /srv/app/templates /etc/app/settings.json
SitemapLastModified /application 2026-10-14T22:00:00Z
SitemapChangeFrequency /application daily
SitemapPriority /application 0.8
```

`SitemapFile` reads the file's actual modification time whenever the sitemap is
requested. `SitemapDirectory` does the same for the directory entry itself; it
does not recursively enumerate files.

`SitemapComposite` maps multiple physical files or directories to one public
URL. Its `<lastmod>` is the newest modification time among all existing sources
and any dynamic notification recorded for that URL. Missing sources are
ignored. This is useful when a scripted location depends on an application
script, templates, and configuration files:

```apache
SitemapComposite /reports \
    /srv/reports/reports.py \
    /srv/reports/templates \
    /etc/reports/public-settings.json
```

Directory sources contribute their directory-entry mtime; their contents are
not recursively scanned.

`SitemapComposite` is also valid inside `<Location>` and `<Directory>`. Keep
the public URI explicit in both contexts:

```apache
<Location /reports>
    SitemapComposite /reports /srv/reports/app.py /srv/reports/templates
</Location>

<Directory /srv/www/downloads>
    SitemapComposite /downloads/ /srv/www/downloads /etc/downloads.json
</Directory>
```

### Change frequency and priority

Optional Sitemap Protocol metadata can be assigned to any configured URI:

```apache
SitemapChangeFrequency /reports daily
SitemapPriority /reports 0.8
```

Valid change frequencies are `always`, `hourly`, `daily`, `weekly`, `monthly`,
`yearly`, and `never`. Priority must be between `0.0` and `1.0`, inclusive.
Both directives are order-independent and may be placed at server/vhost scope
or inside `<Location>` and `<Directory>` blocks. The URI remains explicit.

`changefreq` is a crawler hint defined by the Sitemap Protocol; it does not
schedule filesystem scans, application jobs, or notification calls. Priority
is relative only to other URLs on the same site.

### Explicit last-modified values

`SitemapLastModified` assigns a fixed `<lastmod>` to any configured URI,
regardless of whether that URI was declared with `SitemapFile`,
`SitemapDirectory`, `SitemapLocation`, `SitemapComposite`, or `SitemapURL`:

```apache
SitemapLastModified /reports 2026-10-14T22:00:00Z
SitemapLastModified /documents/ 2026-10-14
```

Accepted formats are `YYYY-MM-DD` and UTC `YYYY-MM-DDTHH:MM:SSZ`. The directive
is order-independent and can appear before or after the corresponding entry.
An explicit value takes precedence over every computed or notified timestamp.
Each URI may have only one explicit override.

Inside `<Location>` or `<Directory>`, use an explicit public URI:

```apache
<Location /application>
    SitemapURL /application dynamic
</Location>

<Directory /srv/www/downloads>
    SitemapURL /downloads/ filesystem
</Directory>
```

Apache cannot reliably infer a public URI from a `<Directory>` because aliases,
rewrites, and virtual hosts may map it differently. Therefore the URI remains
explicit.

## Dynamic-page modification notifications

For configured dynamic entries, the module observes:

- a valid `Last-Modified` response header; or
- a successful `POST`, `PUT`, `PATCH`, or `DELETE` directly to the configured
  URI, in which case the completion time is stored.

State is persisted under `SitemapStateDirectory`, making it visible to every
prefork/worker/event child and across restarts. The Apache account must be able
to write that directory. A dynamic URL without a known timestamp is still
included, but without `<lastmod>`.

Create the state directory before starting Apache so it has the correct owner:

```sh
sudo install -d -o www-data -g www-data -m 0750 /var/lib/mod_sitemap
```

Use the platform's Apache account (`apache` on many Red Hat systems) instead of
`www-data` where applicable. Marker updates use an exclusive cross-process file
lock and never move a recorded timestamp backwards.

Application Apache modules can notify changes directly:

```c
#include "mod_sitemap.h"

APR_OPTIONAL_FN_TYPE(sitemap_notify) *notify_sitemap;

notify_sitemap = APR_RETRIEVE_OPTIONAL_FN(sitemap_notify);
if (notify_sitemap) {
    apr_status_t rv = notify_sitemap(r->server, "/reports", apr_time_now());
    /* APR_NOTFOUND means that URI is not configured as a dynamic entry. */
}
```

Pass `APR_TIME_C(-1)` instead of a timestamp to use the current time.

For PHP, Python, Perl, or CGI applications that cannot call an Apache optional
function directly, this release includes an optional HTTP bridge and PHP/Python
clients under `examples/`. The bridge remains separate so installations do not
expose a mutation endpoint unless they deliberately build and configure it.

Build the bridge after building/installing `mod_sitemap`:

```sh
cd examples
apxs -c -I.. mod_sitemap_notify_http.c
sudo apxs -i -a -n sitemap_notify_http .libs/mod_sitemap_notify_http.so
```

Then adapt `examples/sitemap-notify-http.conf`. The supplied configuration uses
`Require local` and should remain restricted to loopback callers. PHP uses
`notify_sitemap.php`; Python uses `notify_sitemap.py`. Both call the endpoint
only after the application transaction commits. Omit `modified` to record the
current time, or pass Unix seconds explicitly.

The notification request must reach the same virtual host that owns the
sitemap entry. Set PHP's `SITEMAP_NOTIFY_URL` environment variable or pass the
Python function's `endpoint` argument to an internal hostname mapped to
loopback when the numeric `127.0.0.1` URL would select a different default
virtual host.

Do not expose the bridge publicly. If applications run on another host, protect
it with TLS plus strong Apache authentication and authorize only the specific
application identity. Merely receiving a GET remains intentionally insufficient
to mark content as changed.

## Directives

| Directive | Context | Purpose |
| --- | --- | --- |
| `SitemapEngine On\|Off` | server/vhost | Enable the module |
| `SitemapEndpoint /uri` | server/vhost | XML endpoint |
| `SitemapBaseURL https://host` | server/vhost | Canonical public origin |
| `SitemapStateDirectory path` | server/vhost | Persistent dynamic timestamps |
| `SitemapFile URI PATH` | server/vhost | One URL backed by a regular file |
| `SitemapDirectory URI PATH` | server/vhost | One URL backed by a directory |
| `SitemapLocation URI` | server/vhost | One dynamic URL |
| `SitemapComposite URI PATH...` | server/vhost, Location, Directory | One URL backed by multiple files/directories plus notifications |
| `SitemapLastModified URI TIMESTAMP` | server/vhost, Location, Directory | Fixed lastmod override for any configured URL |
| `SitemapChangeFrequency URI VALUE` | server/vhost, Location, Directory | Protocol change-frequency hint |
| `SitemapPriority URI VALUE` | server/vhost, Location, Directory | Relative priority from 0.0 to 1.0 |
| `SitemapURL URI [dynamic\|filesystem]` | Location/Directory | Context-local declaration |

## Operational notes

- Entries are explicit; the module does not crawl the document root.
- Missing or wrong-type filesystem sources remain in the sitemap without a
  `<lastmod>` value.
- The endpoint permits only GET and HEAD.
- XML values are escaped.
- Configured public URLs must be valid UTF-8 and must not contain literal
  spaces or control characters. Percent-encode spaces and other URI bytes.
- `Cache-Control: no-cache` forces clients to revalidate dynamic timestamps.
- Keep `SitemapStateDirectory` owned by and writable only to the Apache service
  account (or its tightly controlled group). Marker files are trusted local
  state, not an application upload surface.
