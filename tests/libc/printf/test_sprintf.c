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

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#include "clar.h"

// "Define" libc functions we're testing
#include "pblibc_private.h"
// I want real memset
#undef memset

///////////////////////////////////////////////////////////////////////////////////////////////////
//! Tests

void test_sprintf__basic(void) {
  char dstbuf[256];

  cl_assert_equal_i(snprintf(dstbuf, 256, "Hello!\nI am error"), 17);
  cl_assert_equal_s(dstbuf, "Hello!\nI am error");

  snprintf(dstbuf, 256, "What is the %%d");
  cl_assert_equal_s(dstbuf, "What is the %d");

  snprintf(dstbuf, 256, "What is the %%d");
  cl_assert_equal_s(dstbuf, "What is the %d");
}

void test_sprintf__truncate(void) {
  char dstbuf[256];

  memset(dstbuf, 0xFF, 256);
  cl_assert_equal_i(snprintf(dstbuf, 17, "Hello!\nI am error"), 17);
  cl_assert_equal_m(dstbuf, "Hello!\nI am erro\0\xFF", 18);

  memset(dstbuf, 0xFF, 256);
  cl_assert_equal_i(snprintf(dstbuf, 15, "Hello!\nI am error"), 17);
  cl_assert_equal_m(dstbuf, "Hello!\nI am er\0\xFF", 16);
}

void test_sprintf__long_conversion(void) {
  char dstbuf[256];

  uint64_t val = 0xFFFFFFFFFFFFFFFFULL;
  snprintf(dstbuf, 256, "%#llo", val);
  cl_assert_equal_s(dstbuf, "01777777777777777777777");
}


void test_sprintf__null(void) {
  cl_assert_equal_i(snprintf(NULL, 0, "Hello!\nI am error"), 17);
}

void test_sprintf__percent_d(void) {
  char dstbuf[256];

  // Simple %d
  snprintf(dstbuf, 256, "There are %d lights, %d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, -4");

  // Alternate form
  snprintf(dstbuf, 256, "There are %#d lights, %#d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, -4");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %02d lights, %02d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, -4");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %2d lights, %2d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are  4 lights, -4");

  // Left-align, Space padded minimum character output
  snprintf(dstbuf, 256, "There are %-2d lights, %-2d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4  lights, -4");

  // Space for positive signed
  snprintf(dstbuf, 256, "There are % d lights, the absolute value of % d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are  4 lights, the absolute value of -4");

  // Plus for positive signed
  snprintf(dstbuf, 256, "There are %+d lights, the absolute value of %+d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are +4 lights, the absolute value of -4");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.2d lights, %.2d", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, -04");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0dzero%.0d", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.dzero%.d", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3dzero%.-3d", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1dzero%.1d", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*d lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are   4 lights");

  // Left-align, Variable length character output
  snprintf(dstbuf, 256, "There are %*d lights", -3, 4);
  cl_assert_equal_s(dstbuf, "There are 4   lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*d lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are 004 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*dzero%.*d", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Length modifiers
  int64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hhd,%hd,%d,%ld", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "-16,-8464,-1698898192,-1698898192");
  snprintf(dstbuf, 256, "%lld,%jd", hurf, hurf);
  cl_assert_equal_s(dstbuf, "1311768467463790320,1311768467463790320");
  snprintf(dstbuf, 256, "%zd,%td", hurf, hurf);
  cl_assert_equal_s(dstbuf, "-1698898192,-1698898192");
}

// Literally copy-paste from %d
void test_sprintf__percent_i(void) {
  char dstbuf[256];

  // Simple %i
  snprintf(dstbuf, 256, "There are %i lights, %i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, -4");

  // Alternate form
  snprintf(dstbuf, 256, "There are %#i lights, %#i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, -4");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %02i lights, %02i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, -4");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %2i lights, %2i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are  4 lights, -4");

  // Left-align, Space padded minimum character output
  snprintf(dstbuf, 256, "There are %-2i lights, %-2i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4  lights, -4");

  // Space for positive signed
  snprintf(dstbuf, 256, "There are % i lights, the absolute value of % i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are  4 lights, the absolute value of -4");

  // Plus for positive signed
  snprintf(dstbuf, 256, "There are %+i lights, the absolute value of %+i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are +4 lights, the absolute value of -4");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.2i lights, %.2i", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, -04");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0izero%.0i", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.izero%.i", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3izero%.-3i", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1izero%.1i", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*i lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are   4 lights");

  // Left-align, Variable length character output
  snprintf(dstbuf, 256, "There are %*i lights", -3, 4);
  cl_assert_equal_s(dstbuf, "There are 4   lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*i lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are 004 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*izero%.*i", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Length modifiers
  int64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hhi,%hi,%i,%li", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "-16,-8464,-1698898192,-1698898192");
  snprintf(dstbuf, 256, "%lli,%ji", hurf, hurf);
  cl_assert_equal_s(dstbuf, "1311768467463790320,1311768467463790320");
  snprintf(dstbuf, 256, "%zi,%ti", hurf, hurf);
  cl_assert_equal_s(dstbuf, "-1698898192,-1698898192");
}

void test_sprintf__percent_u(void) {
  char dstbuf[256];

  // Simple %u
  snprintf(dstbuf, 256, "There are %u lights, %u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, 4294967292");

  // Alternate form
  snprintf(dstbuf, 256, "There are %#u lights, %#u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, 4294967292");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %02u lights, %02u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, 4294967292");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %2u lights, %2u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are  4 lights, 4294967292");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are % u lights, the absolute value of % u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, the absolute value of 4294967292");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are %+u lights, the absolute value of %+u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 4 lights, the absolute value of 4294967292");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.2u lights, %.2u", 4, -4);
  cl_assert_equal_s(dstbuf, "There are 04 lights, 4294967292");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0uzero%.0u", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.uzero%.u", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3uzero%.-3u", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1uzero%.1u", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*u lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are   4 lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*u lights", 3, 4);
  cl_assert_equal_s(dstbuf, "There are 004 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*uzero%.*u", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Length modifiers
  uint64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hhu,%hu,%u,%lu", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "240,57072,2596069104,2596069104");
  snprintf(dstbuf, 256, "%llu,%ju", hurf, hurf);
  cl_assert_equal_s(dstbuf, "1311768467463790320,1311768467463790320");
  snprintf(dstbuf, 256, "%zu,%tu", hurf, hurf);
  cl_assert_equal_s(dstbuf, "2596069104,2596069104");
}

void test_sprintf__percent_o(void) {
  char dstbuf[256];

  // Simple %o
  snprintf(dstbuf, 256, "There are %o lights, %o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, 37777777774");

  // Alternate form (adds 0 prefix)
  snprintf(dstbuf, 256, "There are %#o lights, %#o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, 037777777774");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %03o lights, %03o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, 37777777774");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %3o lights, %3o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are  10 lights, 37777777774");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are % o lights, the absolute value of % o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of 37777777774");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are %+o lights, the absolute value of %+o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of 37777777774");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.3o lights, %.3o", 8, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, 37777777774");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0ozero%.0o", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.ozero%.o", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3ozero%.-3o", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1ozero%.1o", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*o lights", 3, 8);
  cl_assert_equal_s(dstbuf, "There are  10 lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*o lights", 3, 8);
  cl_assert_equal_s(dstbuf, "There are 010 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*ozero%.*o", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Horrible thing
  snprintf(dstbuf, 256, "%#.ozero", 0);
  cl_assert_equal_s(dstbuf, "0zero");

  // Length modifiers
  uint64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hho,%ho,%o,%lo", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "360,157360,23257157360,23257157360");
  snprintf(dstbuf, 256, "%llo,%jo", hurf, hurf);
  cl_assert_equal_s(dstbuf, "110642547423257157360,110642547423257157360");
  snprintf(dstbuf, 256, "%zo,%to", hurf, hurf);
  cl_assert_equal_s(dstbuf, "23257157360,23257157360");
}

void test_sprintf__percent_x(void) {
  char dstbuf[256];

  // Simple %x
  snprintf(dstbuf, 256, "There are %x lights, %x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, fffffffc");

  // Alternate form (adds 0x prefix)
  snprintf(dstbuf, 256, "There are %#x lights, %#x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 0x10 lights, 0xfffffffc");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %03x lights, %03x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, fffffffc");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %3x lights, %3x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are  10 lights, fffffffc");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are % x lights, the absolute value of % x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of fffffffc");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are %+x lights, the absolute value of %+x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of fffffffc");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.3x lights, %.3x", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, fffffffc");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0xzero%.0x", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.xzero%.x", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3xzero%.-3x", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1xzero%.1x", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*x lights", 3, 16);
  cl_assert_equal_s(dstbuf, "There are  10 lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*x lights", 3, 16);
  cl_assert_equal_s(dstbuf, "There are 010 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*xzero%.*x", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Length modifiers
  uint64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hhx,%hx,%x,%lx", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "f0,def0,9abcdef0,9abcdef0");
  snprintf(dstbuf, 256, "%llx,%jx", hurf, hurf);
  cl_assert_equal_s(dstbuf, "123456789abcdef0,123456789abcdef0");
  snprintf(dstbuf, 256, "%zx,%tx", hurf, hurf);
  cl_assert_equal_s(dstbuf, "9abcdef0,9abcdef0");
}

void test_sprintf__percent_capitalx(void) {
  char dstbuf[256];

  // Simple %X
  snprintf(dstbuf, 256, "There are %X lights, %X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, FFFFFFFC");

  // Alternate form (adds 0X prefix)
  snprintf(dstbuf, 256, "There are %#X lights, %#X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 0X10 lights, 0XFFFFFFFC");

  // Zero padded minimum character output
  snprintf(dstbuf, 256, "There are %03X lights, %03X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, FFFFFFFC");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "There are %3X lights, %3X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are  10 lights, FFFFFFFC");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are % X lights, the absolute value of % X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of FFFFFFFC");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "There are %+X lights, the absolute value of %+X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 10 lights, the absolute value of FFFFFFFC");

  // Minimum digits output
  snprintf(dstbuf, 256, "There are %.3X lights, %.3X", 16, -4);
  cl_assert_equal_s(dstbuf, "There are 010 lights, FFFFFFFC");

  // Minimum digits output (zero digits)
  snprintf(dstbuf, 256, "%.0Xzero%.0X", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (unspecified digits, acts as zero)
  snprintf(dstbuf, 256, "%.Xzero%.X", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.-3Xzero%.-3X", 0, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Minimum digits output (non-zero digits outputting zero)
  snprintf(dstbuf, 256, "%.1Xzero%.1X", 0, 1);
  cl_assert_equal_s(dstbuf, "0zero1");

  // Variable length character output
  snprintf(dstbuf, 256, "There are %*X lights", 3, 16);
  cl_assert_equal_s(dstbuf, "There are  10 lights");

  // Variable length digits output
  snprintf(dstbuf, 256, "There are %.*X lights", 3, 16);
  cl_assert_equal_s(dstbuf, "There are 010 lights");

  // Variable length digits output (negative digits, acts as zero)
  snprintf(dstbuf, 256, "%.*Xzero%.*X", -3, 0, -3, 1);
  cl_assert_equal_s(dstbuf, "zero1");

  // Length modifiers
  uint64_t hurf = 0x123456789ABCDEF0ULL;
  snprintf(dstbuf, 256, "%hhX,%hX,%X,%lX", hurf, hurf, hurf, hurf);
  cl_assert_equal_s(dstbuf, "F0,DEF0,9ABCDEF0,9ABCDEF0");
  snprintf(dstbuf, 256, "%llX,%jX", hurf, hurf);
  cl_assert_equal_s(dstbuf, "123456789ABCDEF0,123456789ABCDEF0");
  snprintf(dstbuf, 256, "%zX,%tX", hurf, hurf);
  cl_assert_equal_s(dstbuf, "9ABCDEF0,9ABCDEF0");
}

void test_sprintf__percent_c(void) {
  char dstbuf[256];

  // Simple %c
  snprintf(dstbuf, 256, "Hur%c", 'f');
  cl_assert_equal_s(dstbuf, "Hurf");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "Hur%2c", 'f');
  cl_assert_equal_s(dstbuf, "Hur f");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "Hur% c", 'f');
  cl_assert_equal_s(dstbuf, "Hurf");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "Hur%+c", 'f');
  cl_assert_equal_s(dstbuf, "Hurf");

  // Variable length character output
  snprintf(dstbuf, 256, "Hur%*c", 2, 'f');
  cl_assert_equal_s(dstbuf, "Hur f");
}

void test_sprintf__percent_s(void) {
  char dstbuf[256];

  // Simple %s
  snprintf(dstbuf, 256, "You know Bagu? %s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you cross");

  // Space padded minimum character output
  snprintf(dstbuf, 256, "You know Bagu? %25s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu?  Then I can let you cross");

  // Space for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "You know Bagu? % s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you cross");

  // Plus for positive signed
  // No signed conversion occurs, so this is a no-op
  snprintf(dstbuf, 256, "You know Bagu? %+s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you cross");

  // Variable length character output
  snprintf(dstbuf, 256, "You know Bagu? %*s", 25, "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu?  Then I can let you cross");

  // Left align
  snprintf(dstbuf, 256, "You know Bagu? %-26s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you cross  ");

  // Maximum character output                    123456789012345678901234
  snprintf(dstbuf, 256, "You know Bagu? %.19s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you ");

  // Left-align, Space padded minimum + maximum character output
  snprintf(dstbuf, 256, "You know Bagu? %-25.19s", "Then I can let you cross");
  cl_assert_equal_s(dstbuf, "You know Bagu? Then I can let you       ");
}

void test_sprintf__percent_p(void) {
  // %p is almost entirely implementation defined.
  // Because of this, we are testing against newlib's output.
  // Newlib just treats it as %#x
  char dstbuf[256];

  snprintf(dstbuf, 256, "What's a cool number? %p", (void*)(uintptr_t)0x02468ACE);
  cl_assert_equal_s(dstbuf, "What's a cool number? 0x2468ace");

  snprintf(dstbuf, 256, "What's a cool number? %p", (void*)(uintptr_t)0);
  cl_assert_equal_s(dstbuf, "What's a cool number? 0");
}

void test_sprintf__percent_n(void) {
  char dstbuf[512];
  int val, val2;
  int8_t hhntest[4];
  int16_t hntest[2];
  int32_t lntest[2];
  int64_t llntest[2];
  int64_t jntest[2];
  int32_t zntest[2];
  int32_t tntest[2];

  snprintf(dstbuf, 256, "%n", &val);
  cl_assert_equal_i(val, 0);

  snprintf(dstbuf, 256, "Incredible mechanical monster%n coming soon%n!!", &val, &val2);
  cl_assert_equal_i(val, 29);
  cl_assert_equal_i(val2, 42);

  // Note: Newlib actually doesn't support %hhn, breaking standard.
  hhntest[0] = hhntest[1] = hhntest[2] = hhntest[3] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                    "%hhnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , hhntest);
  cl_assert_equal_i(hhntest[0], 280-256);
  cl_assert_equal_i(hhntest[1], 0);
  cl_assert_equal_i(hhntest[2], 0);
  cl_assert_equal_i(hhntest[3], 0);

  hntest[0] = hntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                     "%hnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , hntest);
  cl_assert_equal_i(hntest[0], 280);
  cl_assert_equal_i(hntest[1], 0);

  lntest[0] = lntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                     "%lnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , lntest);
  cl_assert_equal_i(lntest[0], 280);
  cl_assert_equal_i(lntest[1], 0);

  llntest[0] = llntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                    "%llnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , llntest);
  cl_assert_equal_i(llntest[0], 280);
  cl_assert_equal_i(llntest[1], 0);

  jntest[0] = jntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                     "%jnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , jntest);
  cl_assert_equal_i(jntest[0], 280);
  cl_assert_equal_i(jntest[1], 0);

  zntest[0] = zntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                     "%znaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , zntest);
  cl_assert_equal_i(zntest[0], 280);
  cl_assert_equal_i(zntest[1], 0);

  tntest[0] = tntest[1] = 0;
  snprintf(dstbuf, 512, "aaaa aaaa aaaa aaaa " //  0- 20
                        "aaaa aaaa aaaa aaaa " // 20- 40
                        "aaaa aaaa aaaa aaaa " // 40- 60
                        "aaaa aaaa aaaa aaaa " // 60- 80
                        "aaaa aaaa aaaa aaaa " // 80-100
                        "aaaa aaaa aaaa aaaa " //100-120
                        "aaaa aaaa aaaa aaaa " //120-140
                        "aaaa aaaa aaaa aaaa " //140-160
                        "aaaa aaaa aaaa aaaa " //160-180
                        "aaaa aaaa aaaa aaaa " //180-200
                        "aaaa aaaa aaaa aaaa " //200-220
                        "aaaa aaaa aaaa aaaa " //220-240
                        "aaaa aaaa aaaa aaaa " //240-260
                        "aaaa aaaa aaaa aaaa " //260-280
                     "%tnaaaa aaaa aaaa aaaa " //280-300
                        "aaaa aaaa aaaa aaaa " //300-320
                        , tntest);
  cl_assert_equal_i(tntest[0], 280);
  cl_assert_equal_i(tntest[1], 0);
}
