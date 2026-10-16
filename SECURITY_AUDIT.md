# mod_sitemap 1.0.1 hardening audit

Scope: `mod_sitemap.c` and the optional
`examples/mod_sitemap_notify_http.c` bridge, reviewed for null dereferences,
out-of-bounds access, integer narrowing/overflow, unsafe string handling, and
unchecked APR failures.

## Corrected findings

- Added null checks around Apache request, server, process, pool, module-config,
  table, array, directive, URI, and path inputs before dereference.
- Added allocation-result checks and changed configuration construction so an
  allocation failure cannot leave a partially initialized array entry.
- Fixed `apr_file_write_full` to receive a real output-count pointer and added
  short-write detection.
- Preserved and returned state-file read, lock, unlock, close, truncate, seek,
  formatting, and directory-creation failures instead of masking them.
- Rejected full-buffer/truncated marker timestamps and malformed, negative,
  trailing, or overflowing timestamp values.
- Prevented Unix-seconds multiplication overflow in the optional HTTP bridge.
- Added overflow-checked sizing to XML escaping. All fixed-size copies use
  lengths matched to their literals and terminate the destination explicitly.
- Chunked URI hashing at `UINT_MAX` so `apr_size_t` input lengths are not
  silently narrowed to the SHA-1 API's `unsigned int` length.
- Replaced formatted hexadecimal writes with bounded direct nibble encoding.
- Checked `apr_snprintf` for error/truncation before manipulating its output.
- Added strict UTF-8, whitespace, control-character, scheme, host, and endpoint
  validation for public URL configuration.
- Pre-rendered and validated every sitemap entry before sending XML, avoiding a
  partially emitted response if a later allocation or formatting operation
  fails.

## Verification performed

- Compiled both C translation units with GCC C11, `-Wall`, `-Wextra`,
  `-Wconversion`, `-Wshadow`, and GCC's `-fanalyzer` against Apache/APR API
  stubs. No analyzer diagnostic for null dereferences, use-after-free,
  out-of-bounds access, or unsafe arithmetic remained. Stub-only callback
  liveness warnings were expected because the stub directive macros do not
  retain callback references.
- Ran the packaged contract regression suite successfully.
- Audited every direct array, fixed-buffer, copy, formatting, and string parsing
  operation manually.

## Operational boundaries

- APR pool allocation normally follows Apache's configured abort policy on
  exhaustion; the module nevertheless checks returned pointers where an API can
  report failure.
- The marker directory is trusted local state and must not be writable by
  untrusted users. This avoids replacement/symlink attacks outside the module's
  process boundary.
- Marker updates are cross-process locked but are not crash-atomic: a process or
  host failure between truncation and write can lose that URI's timestamp. A
  subsequent successful notification repairs it.
- Final release qualification must include `make`, `httpd -t`, module load, and
  request tests on each supported Apache/APR platform because local API stubs do
  not replace ABI testing against the target server.

No finite review can prove the absence of every defect. This audit records the
specific code paths examined, corrections made, tests run, and remaining
operational assumptions so release claims remain reproducible.
