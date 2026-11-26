#ifndef X509_CRT_BUNDLE_H
#define X509_CRT_BUNDLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CA bundle in the ESP-IDF v4.4 / arduino-esp32 2.0.x esp_crt_bundle format,
// generated into src/x509_crt_bundle.c by scripts/update_ca_bundle.sh.
extern const uint8_t x509_crt_bundle[];
extern const size_t x509_crt_bundle_len;

#ifdef __cplusplus
}
#endif

#endif // X509_CRT_BUNDLE_H
