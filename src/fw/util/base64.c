/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "net.h"
#include "system/passert.h"

#include <ctype.h>
#include <string.h>

static int8_t decode_char(uint8_t c) {
  if (isupper(c)) return c - 'A';
  if (islower(c)) return c - 'a' + 26;
  if (isdigit(c)) return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return 127;

  return -1;
}

unsigned int base64_decode_inplace(char* buffer, unsigned int length) {
  unsigned int read_index = 0;
  unsigned int write_index = 0;
  while (read_index < length) {

    int quad_index = 0;
    unsigned int v = 0;
    for (; quad_index < 4; ++quad_index) {
      int8_t c = decode_char(buffer[read_index + quad_index]);
      if (c == -1) return 0; // Error, invalid character
      if (c == 127) break; // Padding found

      v = (v * 64) + c;
    }

    // Handle the padding if we broke out the loop early (0-2 '=' characters).
    const unsigned int padding_amount = 4 - quad_index;
    if (padding_amount > 2) return 0; // Makes no sense to pad an entire triplet.
    if (memcmp(buffer + read_index + quad_index, "==", padding_amount) != 0) return 0; // There are characters after our padding?

    // Chop off extra unused low bits if we're padded.
    // If there's only 2 6-bit characters (+ 2 '='s for padding), this results in 12-bits of data.
    //  We only want the first 8-bits, so shift out 4.
    // If there's only 3 6-bit character (+ '=' for padding), this results in 18-bits of data.
    //  We only want the first 16-bits, so shift out 2.
    v = v >> (padding_amount * 2);

    const char* v_as_bytes = ((const char*) &v);
    for (unsigned int i = 0; i < (3 - padding_amount); ++i) {
      buffer[write_index + i] = v_as_bytes[(2 - padding_amount) - i];
    }

    read_index += 4;
    write_index += 3 - padding_amount;

    if (padding_amount != 0 && read_index < length) {
      // error, padded quad in the middle our string.
      return 0;
    }
  }
  return write_index;
}


static char prv_encode_char(uint8_t binary) {
  if (binary < 26) {
    return binary + 'A';
  } else if (binary < 52) {
    return binary - 26 + 'a';
  } else if (binary < 62) {
    return binary - 52 + '0';
  } else if (binary == 62) {
    return '+';
  } else if (binary == 63) {
    return '/';
  } else {
    WTF;
  }
}


int32_t base64_encode(char *out, int out_len, const uint8_t *data, int32_t data_len) {
  int result = (data_len + 2) / 3 * 4;
  if (result > out_len) {
    return result;
  }
  int32_t i;
  for (i = 0; i < data_len - 2; i += 3) {
    *out++ = prv_encode_char(data[i] >> 2);
    *out++ = prv_encode_char(((data[i] & 0x03) << 4) | (data[i + 1] >> 4));
    *out++ = prv_encode_char(((data[i + 1] & 0x0F) << 2) | (data[i + 2] >> 6));
    *out++ = prv_encode_char(data[i + 2] & 0x3F);
  }

  if (i < data_len) {
    if (i == data_len - 2) {
      // if 2 leftover bytes
      *out++ = prv_encode_char(data[i] >> 2);
      *out++ = prv_encode_char(((data[i] & 0x03) << 4) | (data[i + 1] >> 4));
      *out++ = prv_encode_char((data[i + 1] & 0x0F) << 2);
      *out++ = '=';
    } else if (i == data_len - 1) {
      // if 1 leftover byte
      *out++ = prv_encode_char(data[i] >> 2);
      *out++ = prv_encode_char((data[i] & 0x03) << 4);
      *out++ = '=';
      *out++ = '=';
    }
  }

  if (result < out_len) {
    *out++ = 0;
  }
  return result;
}
