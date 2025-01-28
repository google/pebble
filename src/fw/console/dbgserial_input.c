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

#include "dbgserial_input.h"

#include "board/board.h"
#include "drivers/dma.h"
#include "drivers/exti.h"
#include "drivers/uart.h"
#include "kernel/util/stop.h"
#include "os/tick.h"
#include "services/common/system_task.h"
#include "services/common/new_timer/new_timer.h"
#include "system/passert.h"
#include "util/attributes.h"
#include "util/likely.h"

#include "drivers/gpio.h"

#define STOP_MODE_TIMEOUT_MS (2000)

static void dbgserial_interrupt_handler(bool *should_context_switch);

static DbgSerialCharacterCallback s_character_callback;
static TimerID s_stop_mode_timeout_timer;
//! Use a separate variable so it's safe to check from the ISR.
static bool s_stop_mode_inhibited = false;

//! We DMA into this buffer as a circular buffer
#define DMA_BUFFER_LENGTH (200)
static uint8_t DMA_BSS s_dma_buffer[DMA_BUFFER_LENGTH];
static bool s_dma_enabled = false;


static void stop_mode_timeout_timer_callback(void* cb_data) {
  // re-enable stop mode
  if (s_stop_mode_inhibited) {
    stop_mode_enable(InhibitorDbgSerial);
    s_stop_mode_inhibited = false;
  }
}

static bool prv_uart_irq_handler(UARTDevice *dev, uint8_t data, const UARTRXErrorFlags *err_flags) {
  bool should_context_switch = false;
  if (s_character_callback) {
    s_character_callback(data, &should_context_switch);
  }
  return should_context_switch;
}

void dbgserial_input_init(void) {
  exti_configure_pin(BOARD_CONFIG.dbgserial_int, ExtiTrigger_Falling, dbgserial_interrupt_handler);

  // some platforms have a separate pin for the EXTI int and the USART
  if (BOARD_CONFIG.dbgserial_int_gpio.gpio != NULL) {
    gpio_input_init(&BOARD_CONFIG.dbgserial_int_gpio);
  }

  // set up the USART interrupt on RX
  uart_set_rx_interrupt_handler(DBG_UART, prv_uart_irq_handler);
  uart_set_rx_interrupt_enabled(DBG_UART, true);

  s_stop_mode_timeout_timer = new_timer_create();

  // Enable receive interrupts
  dbgserial_enable_rx_exti();
}

void dbgserial_enable_rx_exti(void) {
  exti_enable(BOARD_CONFIG.dbgserial_int);
}

void dbgserial_register_character_callback(DbgSerialCharacterCallback callback) {
  s_character_callback = callback;
}

// This callback gets installed by dbgserial_interrupt_handler()
// using system_task_add_callback_from_isr().
// It is used to start up our timer since doing so from an ISR is not allowed.
static void prv_start_timer_callback(void* data) {
  new_timer_start(s_stop_mode_timeout_timer, STOP_MODE_TIMEOUT_MS, stop_mode_timeout_timer_callback,
                  NULL, 0 /*flags*/);
}

static void dbgserial_interrupt_handler(bool *should_context_switch) {
  exti_disable(BOARD_CONFIG.dbgserial_int);

  // Start the timer
  system_task_add_callback_from_isr(prv_start_timer_callback, (void *)0, should_context_switch);

  if (!s_stop_mode_inhibited) {
    // We don't bother cancelling the timer if we leave the state where we don't want to stop mode
    // anymore. For example, if we ctrl-c to enter the prompt (disable stop and start timer),
    // ctrl-d to leave the prompt, and then ctrl-c again before the timer goes off, we'll have the
    // timer still running. If we were to disable stop again after rescheduling the timer, the timer
    // would only go off once for the two disables and we'd end up jamming the reference count.
    stop_mode_disable(InhibitorDbgSerial);
    s_stop_mode_inhibited = true;
  }
}

void dbgserial_set_rx_dma_enabled(bool enabled) {
#if TARGET_QEMU
  // we can't use DMA on QEMU
  enabled = false;
#endif
  if (enabled == s_dma_enabled) {
    return;
  }
  s_dma_enabled = enabled;
  if (enabled) {
    uart_start_rx_dma(DBG_UART, s_dma_buffer, DMA_BUFFER_LENGTH);
  } else {
    uart_stop_rx_dma(DBG_UART);
  }
}
