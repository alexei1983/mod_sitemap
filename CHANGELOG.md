# Changelog

## 1.0.1

- Added defensive validation for Apache request, server, configuration, pool,
  table, array, URI, filesystem-path, and optional-function inputs.
- Prevented partially initialized configuration entries when allocation fails.
- Added checked arithmetic to XML escaping and chunked SHA-1 input lengths to
  avoid size narrowing and overflow.
- Fixed state writes to pass a valid byte-count pointer to
  `apr_file_write_full` and to detect short writes.
- Stopped masking state-file read, unlock, close, formatting, and allocation
  failures.
- Hardened timestamp parsing against malformed, truncated, trailing, negative,
  and overflowing values.
- Hardened the optional HTTP notification bridge against null request fields,
  invalid URI whitespace, and timestamp multiplication overflow.

## 1.0.0

- Added XML sitemap generation for explicit static files, directories, and
  virtual locations.
- Added live file and directory modification-time inspection.
- Added persistent cross-process timestamps for dynamic content.
- Added automatic observation of `Last-Modified` and successful mutations.
- Added the `sitemap_notify` Apache optional function for application modules.
- Added `<Location>` and `<Directory>` configuration support.
- Added `SitemapComposite` to combine multiple file or directory modification
  times and dynamic notifications into one URL's `<lastmod>` value.
- Added the order-independent `SitemapLastModified` directive for fixed
  date-only or UTC RFC 3339 overrides on any configured URI.
- Added an optional example HTTP bridge that calls `sitemap_notify`, with PHP
  and Python clients for non-native applications.
- Allowed `SitemapComposite` inside `<Location>` and `<Directory>` blocks.
- Added validated per-URI `SitemapChangeFrequency` and `SitemapPriority`
  metadata and XML output.
- Fixed `SitemapComposite` being rejected by the single-path filesystem
  validator before its path list could be parsed.
