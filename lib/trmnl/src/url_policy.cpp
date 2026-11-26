#include <url_policy.h>
#include <string.h>

static const char *skip_scheme(const char *url)
{
  if (strncmp(url, "https://", 8) == 0)
    return url + 8;
  if (strncmp(url, "http://", 7) == 0)
    return url + 7;
  return nullptr;
}

bool url_extract_host(const char *url, char *out, size_t out_size)
{
  if (out == nullptr || out_size == 0)
    return false;
  out[0] = '\0';
  if (url == nullptr)
    return false;
  const char *p = skip_scheme(url);
  if (p == nullptr)
    return false;

  size_t len = 0;
  while (p[len] != '\0' && p[len] != '/' && p[len] != '?' && p[len] != '#')
  {
    // HTTPClient strips "user:pass@" and connects to what follows, so
    // "https://trusted@evil/" must not count as the trusted host
    if (p[len] == '@' || p[len] == '\\')
      return false;
    len++;
  }
  const char *colon = (const char *)memchr(p, ':', len);
  size_t host_len = colon ? (size_t)(colon - p) : len;
  if (host_len == 0 || host_len >= out_size)
    return false;

  for (size_t i = 0; i < host_len; i++)
  {
    char c = p[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[host_len] = '\0';
  return true;
}

bool url_is_trusted_firmware_source(const char *url, const char *trusted_base_url)
{
  if (url == nullptr || strncmp(url, "https://", 8) != 0)
    return false;
  char host[128];
  char trusted[128];
  if (!url_extract_host(url, host, sizeof(host)) || !url_extract_host(trusted_base_url, trusted, sizeof(trusted)))
    return false;
  return strcmp(host, trusted) == 0;
}
