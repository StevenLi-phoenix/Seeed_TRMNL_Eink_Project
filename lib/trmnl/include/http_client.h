#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <x509_crt_bundle.h>

// Error codes for the HTTP utilities - using distinct values to avoid overlap
enum HttpError
{
  HTTPCLIENT_SUCCESS = 100,
  HTTPCLIENT_WIFICLIENT_ERROR = 101, // Failed to create client
  HTTPCLIENT_HTTPCLIENT_ERROR = 102  // Failed to connect
};

/**
 * @brief Higher-order function that sets up WiFiClient and HTTPClient, then runs a callback
 * @param url The initial URL to connect to
 * @param callback Function to call with the HTTPClient pointer and error code
 * @return The value returned by the callback
 */
template <typename Callback, typename ReturnType = decltype(std::declval<Callback>()(nullptr, (HttpError)0))>
ReturnType withHttp(const String &url, Callback callback)
{
  Log_info("==== withHttp() %s", url.c_str());

  bool isHttps = url.startsWith("https://");

  // Conditionally allocate only the client we need
  WiFiClient *client = nullptr;
  WiFiClientSecure *secureClient = nullptr;

  if (isHttps)
  {
    secureClient = new WiFiClientSecure();
#ifdef TRMNL_TLS_INSECURE
    // opt-in escape hatch for self-signed servers; any on-path attacker can
    // then impersonate the server (images, commands, firmware)
    secureClient->setInsecure();
    Log_info("TLS: server certificate NOT verified (TRMNL_TLS_INSECURE)");
#else
    // verify the server certificate chain + hostname against the embedded
    // Mozilla CA bundle (src/x509_crt_bundle.c)
    secureClient->setCACertBundle(x509_crt_bundle);
#endif
    client = secureClient;
  }
  else
  {
    client = new WiFiClient();
  }

  // Check if client creation succeeded
  if (!client)
  {
    return callback(nullptr, HTTPCLIENT_WIFICLIENT_ERROR);
  }

  ReturnType result;
  { // Add a scoping block for HTTPClient https to make sure it is destroyed before WiFiClientSecure *client is

    HTTPClient https;
    if (https.begin(*client, url))
    {
      result = callback(&https, HTTPCLIENT_SUCCESS);
      https.end();
    }
    else
    {
      result = callback(nullptr, HTTPCLIENT_HTTPCLIENT_ERROR);
    }
  }
  if (secureClient)
  {
    // a rejected certificate surfaces as a generic connection error in
    // HTTPClient; keep the mbedTLS reason in the log (lastError() holds the
    // socket fd after a successful handshake, a negative mbedTLS code after a
    // failed one)
    char tlsError[100] = "";
    int tlsCode = secureClient->lastError(tlsError, sizeof(tlsError));
    if (tlsCode < 0)
    {
      Log_error("TLS error %d (%s) for %s", tlsCode, tlsError, url.c_str());
    }
  }
  delete client;

  return result;
}

#endif // HTTP_UTILS_H