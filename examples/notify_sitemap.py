#!/usr/bin/env python3
"""Notify mod_sitemap after a successful application transaction."""

import time
import urllib.parse
import urllib.request


def notify_sitemap_changed(uri, modified=None, endpoint=None):
    if not isinstance(uri, str) or not uri.startswith("/"):
        raise ValueError("sitemap URI must begin with /")
    endpoint = endpoint or "http://127.0.0.1/_internal/sitemap-notify"
    parameters = {"uri": uri}
    if modified is not None:
        parameters["modified"] = str(int(modified))
    request = urllib.request.Request(
        endpoint + "?" + urllib.parse.urlencode(parameters),
        data=b"",
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=3) as response:
        if response.status != 204:
            raise RuntimeError(
                "sitemap notification failed with HTTP {}".format(response.status)
            )


# Example: call only after a committed database update.
notify_sitemap_changed("/reports", time.time())
