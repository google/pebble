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

#include "drivers/display.h"

#include <stdbool.h>

#include "board/board.h"
#include "drivers/dbgserial.h"
#include "drivers/display/bootloader_fpga_bitstream.auto.h"
#include "drivers/display/ice40lp.h"
#include "drivers/flash/s29vs.h"
#include "drivers/gpio.h"
#include "drivers/periph_config.h"
#include "drivers/pmic.h"
#include "drivers/spi.h"
#include "flash_region.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_spi.h"
#include "system/passert.h"
#include "util/delay.h"
#include "util/misc.h"


#define CMD_NULL (0)
#define CMD_SET_PARAMETER (1)
#define CMD_DISPLAY_OFF (2)
#define CMD_DISPLAY_ON (3)
#define CMD_DRAW_SCENE (4)
#define CMD_RESET_RELEASE (8)
#define CMD_RESET_ASSERT (9)

#define SCENE_BLACK (0)
#define SCENE_SPLASH (1)
#define SCENE_UPDATE (2)
#define SCENE_ERROR (3)

#define UPDATE_PROGRESS_MAX (93)

// The FPGA bitstream stored in NVCM may be missing or defective; a replacement
// bitstream may be stored in the MFG info flash region, prefixed with a
// four-byte header. The header is composed of the bitstream length followed by
// its complement (all bits inverted).
#define FPGA_BITSTREAM_FLASH_ADDR (FMC_BANK_1_BASE_ADDRESS + \
                                   FLASH_REGION_MFG_INFO_BEGIN + 0x10000)

struct __attribute__((packed)) FlashBitstream {
  uint16_t len;
  uint16_t len_complement;
  uint8_t bitstream[0];
};

static bool prv_wait_programmed(void) {
  // The datasheet lists the typical NVCM configuration time as 56 ms.
  // Something is wrong if it takes more than twice that time.
  int timeout = 100 * 10;
  while (GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_CDONE) == 0) {
    if (timeout-- == 0) {
      dbgserial_putstr("FPGA CDONE timeout expired!");
      return false;
    }
    delay_us(100);
  }
  return true;
}

static bool prv_reset_into_nvcm(void) {
  // Reset the FPGA and wait for it to program itself via NVCM.
  // NVCM configuration is initiated by pulling CRESET high while SCS is high.
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_SET);
  // CRESET needs to be low for at least 200 ns
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_CRESET, Bit_RESET);
  delay_ms(1);
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_CRESET, Bit_SET);
  return prv_wait_programmed();
}

static bool prv_reset_fpga(void) {
#ifdef BLANK_FPGA
  return display_program(s_fpga_bitstream, sizeof(s_fpga_bitstream));
#endif

  const struct FlashBitstream *bitstream = (void *)FPGA_BITSTREAM_FLASH_ADDR;
  // Work around GCC bug https://gcc.gnu.org/bugzilla/show_bug.cgi?id=38341
  uint16_t len_complement_complement = ~bitstream->len_complement;
  if (bitstream->len != 0xffff && bitstream->len == len_complement_complement) {
    dbgserial_putstr("Configuring FPGA from bitstream in flash...");
    if (display_program(bitstream->bitstream, bitstream->len)) {
      return true;
    }
  } else {
    // Fall back to NVCM.
    dbgserial_putstr("No FPGA bitstream in flash.");
  }
  dbgserial_putstr("Falling back to NVCM.");
  return prv_reset_into_nvcm();
}

static void prv_start_command(uint8_t cmd) {
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_RESET);
  delay_us(100);
  display_write_byte(cmd);
}

static void prv_send_command_arg(uint8_t arg) {
  display_write_byte(arg);
}

static void prv_end_command(void) {
  while (SPI_I2S_GetFlagStatus(DISP_SPI, SPI_I2S_FLAG_BSY)) continue;
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_SET);
}

static bool prv_wait_busy(void) {
  // The display should come out of busy within 35 milliseconds;
  // it is a waste of time to wait more than twice that.
  int timeout = 50 * 10;
  while (display_busy()) {
    if (timeout-- == 0) {
      dbgserial_putstr("Display busy-wait timeout expired!");
      return false;
    }
    delay_us(100);
  }
  return true;
}

static void prv_screen_on(void) {
  prv_start_command(CMD_DISPLAY_ON);
  prv_end_command();
}

static void prv_screen_off(void) {
  prv_start_command(CMD_DISPLAY_OFF);
  prv_end_command();
}

void prv_draw_scene(uint8_t scene) {
  prv_start_command(CMD_DRAW_SCENE);
  prv_send_command_arg(scene);
  prv_end_command();
}
void prv_set_parameter(uint32_t param) {
  prv_start_command(CMD_SET_PARAMETER);
  // Send in little-endian byte order
  prv_send_command_arg(param & 0xff);
  prv_send_command_arg((param >> 8) & 0xff);
  prv_send_command_arg((param >> 16) & 0xff);
  prv_send_command_arg((param >> 24) & 0xff);
  prv_end_command();
}

static uint8_t prv_read_version(void) {
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_RESET);
  delay_us(100);

  uint8_t version_num = display_write_and_read_byte(0);
  GPIO_WriteBit(DISP_GPIO, DISP_PIN_SCS, Bit_SET);
  return version_num;
}

void display_init(void) {
  display_start();
  bool program_success = prv_reset_fpga();
  if (!program_success) {
    dbgserial_putstr("FPGA configuration failed. Is this a bigboard?");
    // Don't waste time trying to get the FPGA unstuck if it's not configured.
    // It's just going to waste time and frustrate bigboard users.
    return;
  }

  dbgserial_print("FPGA version: ");
  dbgserial_print_hex(prv_read_version());
  dbgserial_putstr("");

  // enable the power rails
  display_power_enable();

#ifdef TEST_FPGA_RESET_COMMAND
#define ASSERT_BUSY_IS(state) \
  dbgserial_putstr(GPIO_ReadInputDataBit(DISP_GPIO, DISP_PIN_BUSY) == state? \
                  "Yes" : "No")

  // Test out the FPGA soft-reset capability present in release-03 of the FPGA.
  dbgserial_putstr("FPGA soft-reset test");

  dbgserial_print("Precondition: BUSY asserted during scene draw? ");
  prv_draw_scene(SCENE_BLACK);
  ASSERT_BUSY_IS(Bit_SET);

  dbgserial_print("Is BUSY cleared after the reset command? ");
  prv_start_command(CMD_RESET_ASSERT);
  prv_end_command();
  ASSERT_BUSY_IS(Bit_RESET);

  dbgserial_print("Are draw-scene commands ineffectual while in reset? ");
  prv_draw_scene(SCENE_BLACK);
  ASSERT_BUSY_IS(Bit_RESET);

  dbgserial_print("Does releasing reset allow draw-scene commands "
                  "to function again? ");
  prv_start_command(CMD_RESET_RELEASE);
  prv_end_command();
  prv_draw_scene(SCENE_BLACK);
  ASSERT_BUSY_IS(Bit_SET);

  dbgserial_print("Does the draw-scene command complete? ");
  dbgserial_putstr(prv_wait_busy()? "Yes" : "No");
#endif

  // Work around an issue which some boards exhibit where the FPGA ring
  // oscillator can start up with higher harmonics, massively overclocking the
  // design and causing malfunction. When this occurs, the draw-scene command
  // will not work, asserting BUSY indefinitely but never updating the display.
  // Other commands such as display-on and display-off are less affected by the
  // overclocking, so the display can be turned on while the FPGA is in this
  // state, showing only garbage.
  // FPGA malfunction can be detected in software. In an attempt to restore
  // proper functioning, the FPGA can be reset and reconfigured in the hopes
  // that the ring oscillator will start up and oscillate without any higher
  // harmonics. Bootloader release 03 attempts to mitigate this problem by
  // delaying oscillator startup until after configuration completes. Time will
  // tell whether this actually fixes things.
  for (int retries = 0; retries <= 20; ++retries) {
    prv_draw_scene(SCENE_SPLASH);
    if (prv_wait_busy()) {
      prv_screen_on();
      dbgserial_print("Display initialized after ");
      dbgserial_print_hex(retries);
      dbgserial_putstr(" retries.");
      return;
    }

    prv_reset_fpga();
  }

  // It's taken too many attempts and the FPGA still isn't behaving. Give up on
  // showing the splash screen and keep the screen off so that the user doesn't
  // see a broken-looking staticky screen on boot.
  dbgserial_putstr("Display initialization failed.");
  prv_screen_off();
}

void display_boot_splash(void) {
  prv_wait_busy();
  prv_draw_scene(SCENE_SPLASH);
  // Don't turn the screen on until the boot-splash is fully drawn.
  prv_wait_busy();
  prv_screen_on();
}

void display_firmware_update_progress(
    uint32_t numerator, uint32_t denominator) {
  static uint8_t last_bar_fill = UINT8_MAX;
  // Scale progress to the number of pixels in the progress bar,
  // rounding half upwards.
  uint8_t bar_fill =
      ((numerator * UPDATE_PROGRESS_MAX) + ((denominator+1)/2)) / denominator;
  // Don't waste time and power redrawing the same screen repeatedly.
  if (bar_fill != last_bar_fill) {
    last_bar_fill = bar_fill;
    prv_set_parameter(bar_fill);
    prv_draw_scene(SCENE_UPDATE);
  }
}

void display_error_code(uint32_t error_code) {
  prv_set_parameter(error_code);
  prv_draw_scene(SCENE_ERROR);
}

void display_prepare_for_reset(void) {
  prv_screen_off();
}
