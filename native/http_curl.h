/*
 * HTTPS range reads through the TV's libcurl (dlopen).
 *
 * webOS 4 ships libcurl.so.5 (7.53.1 / OpenSSL 1.0.2). The SDK's libcurl.so.4
 * needs libssl.so.3, which this set does not have. The easy/multi option
 * numbers we use have been stable since well before 7.53, so the SDK header
 * and the system library agree.
 *
 * Verification stays on. The CA file is CURLOPT_CAINFO. There is no
 * verify-off path in this file.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct http_client http_client;

/* Return 0 to abort the transfer (EXIT key, or a prefetch being dropped). */
typedef int (*http_pump)(void *ctx);

http_client *http_client_create(const char *ca_file);
void http_client_destroy(http_client *client);

/* Bytes this process has pulled from the network. */
int64_t http_bytes_downloaded(void);

/* Last HTTP status from this client, or 0 if the request never got one. */
int http_last_status(const http_client *client);

/*
 * Inclusive range [start, start+len). Writes exactly `len` bytes to `dst`.
 * `total` may be NULL; on success it receives the object size from
 * Content-Range when the server sent one.
 *
 * Returns 0 on success, -2 if `pump` aborted, -1 otherwise (message in err).
 */
int http_get_range(http_client *client, const char *url, int64_t start, size_t len,
                   uint8_t *dst, int64_t *total, http_pump pump, void *pump_ctx,
                   char *err, size_t err_len);
