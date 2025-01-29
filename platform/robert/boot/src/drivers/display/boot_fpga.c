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
#include "drivers/display/ice40lp_definitions.h"
#include "system/passert.h"
#include "util/delay.h"
#include "util/misc.h"
#include "util/sle.h"

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

#define UPDATE_PROGRESS_MAX (47)

static uint8_t s_decoded_fpga_image[35000];  // the FPGA image is currently ~30k

static bool prv_reset_fpga(void) {
  const uint32_t length = sle_decode(s_fpga_bitstream, sizeof(s_fpga_bitstream),
                                     s_decoded_fpga_image, sizeof(s_decoded_fpga_image));
  return display_program(s_decoded_fpga_image, length);
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
  display_write_cmd(CMD_DISPLAY_ON, NULL, 0);
}

static void prv_screen_off(void) {
  display_write_cmd(CMD_DISPLAY_OFF, NULL, 0);
}

static void prv_draw_scene(uint8_t scene) {
  display_write_cmd(CMD_DRAW_SCENE, &scene, sizeof(scene));
}

static void prv_set_parameter(uint32_t param) {
  display_write_cmd(CMD_SET_PARAMETER, (uint8_t *)&param, sizeof(param));
}

#ifdef DISPLAY_DEMO_LOOP
static void prv_play_demo_loop(void) {
  while (1) {
    for (int i = 0; i <= UPDATE_PROGRESS_MAX; ++i) {
      display_firmware_update_progress(i, UPDATE_PROGRESS_MAX);
      delay_ms(80);
    }

    for (uint32_t i = 0; i <= 0xf; ++i) {
      display_error_code(i * 0x11111111);
      delay_ms(200);
    }
    for (uint32_t i = 0; i < 8; ++i) {
      for (uint32_t j = 1; j <= 0xf; ++j) {
        display_error_code(j << (i * 4));
        delay_ms(200);
      }
    }
    display_error_code(0x01234567);
    delay_ms(200);
    display_error_code(0x89abcdef);
    delay_ms(200);
    display_error_code(0xcafebabe);
    delay_ms(200);
    display_error_code(0xfeedface);
    delay_ms(200);
    display_error_code(0x8badf00d);
    delay_ms(200);
    display_error_code(0xbad1ce40);
    delay_ms(200);
    display_error_code(0xbeefcace);
    delay_ms(200);
    display_error_code(0x0defaced);
    delay_ms(200);
    display_error_code(0xd15ea5e5);
    delay_ms(200);
    display_error_code(0xdeadbeef);
    delay_ms(200);
    display_boot_splash();
    delay_ms(1000);
  }
}
#endif

void display_init(void) {
  display_start();
  if (!prv_reset_fpga()) {
    dbgserial_putstr("FPGA configuration failed.");
    return;
  }

  // enable the power rails
  display_power_enable();

  // start with the screen off
  prv_screen_off();

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
  for (int retries = 0; retries <= 10; ++retries) {
    prv_draw_scene(SCENE_SPLASH);
    if (prv_wait_busy()) {
      prv_screen_on();
      dbgserial_print("Display initialized after ");
      dbgserial_print_hex(retries);
      dbgserial_putstr(" retries.");
#ifdef DISPLAY_DEMO_LOOP
      prv_play_demo_loop();
#endif
      return;
    }

    if (!prv_reset_fpga()) {
      dbgserial_putstr("FPGA configuration failed.");
      return;
    }
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
