#include <cstddef>
#include <cstdint>

enum bmp_err_e
{
  BMP_NO_ERR,
  BMP_NOT_BMP,
  BMP_BAD_SIZE,
  BMP_COLOR_SCHEME_FAILED,
  BMP_INVALID_OFFSET,
};

// header (54) + 2-entry color table (8) of the 1-bpp 800x480 frames we draw
#define BMP_1BPP_HEADER_SIZE 62

/**
 * Validate a downloaded 1-bpp 800x480 BMP. size is the number of valid bytes
 * in data; anything shorter than header + pixel data is rejected so the
 * renderer (which flips and reads data + 62 .. + 48062 in place) can never run
 * past a truncated buffer.
 */
bmp_err_e parseBMPHeader(uint8_t *data, size_t size, bool &reversed);
