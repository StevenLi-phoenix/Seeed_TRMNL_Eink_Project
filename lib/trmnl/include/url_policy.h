#ifndef URL_POLICY_H
#define URL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Which URLs the device trusts for what (fw 1.7.1).
 *
 * OTA firmware is executable code and the app image is not signed, so a
 * firmware_url is only followed when it is https:// on the compile-time
 * API_BASE_URL host. The /api/display server is not enough: anyone in range of
 * the open recovery portal can point NVS api_url at their own server.
 */

/**
 * Copy the lowercase host of an http:// or https:// URL into out.
 * Rejects other schemes, an empty host, userinfo ("user@host"), and hosts that
 * do not fit in out (including the NUL). The port, path, query and fragment are
 * dropped.
 * @return true on success; out is an empty string on failure
 */
bool url_extract_host(const char *url, char *out, size_t out_size);

/**
 * @return true if url is https:// and its host equals the host of
 *         trusted_base_url (case-insensitive)
 */
bool url_is_trusted_firmware_source(const char *url, const char *trusted_base_url);

#endif // URL_POLICY_H
