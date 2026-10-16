<?php
declare(strict_types=1);

/** Notify mod_sitemap only after the application's transaction commits. */
function notifySitemapChanged(
    string $uri,
    ?int $unixTime = null,
    ?string $endpoint = null
): void
{
    if ($uri === '' || $uri[0] !== '/') {
        throw new InvalidArgumentException('Sitemap URI must begin with /.');
    }
    $query = ['uri' => $uri];
    if ($unixTime !== null) {
        $query['modified'] = (string)$unixTime;
    }
    $endpoint = $endpoint
        ?? (getenv('SITEMAP_NOTIFY_URL') ?: 'http://127.0.0.1/_internal/sitemap-notify');
    $url = $endpoint . '?'
         . http_build_query($query, '', '&', PHP_QUERY_RFC3986);
    $curl = curl_init($url);
    if ($curl === false) {
        throw new RuntimeException('Unable to initialize cURL.');
    }
    curl_setopt_array($curl, [
        CURLOPT_POST => true,
        CURLOPT_POSTFIELDS => '',
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_TIMEOUT => 3,
        CURLOPT_CONNECTTIMEOUT => 1,
    ]);
    curl_exec($curl);
    $status = (int)curl_getinfo($curl, CURLINFO_RESPONSE_CODE);
    $error = curl_error($curl);
    curl_close($curl);
    if ($status !== 204) {
        throw new RuntimeException(
            'Sitemap notification failed with HTTP ' . $status
            . ($error !== '' ? ': ' . $error : '')
        );
    }
}

// Example: call after a committed database update.
notifySitemapChanged('/reports');
