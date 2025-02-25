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

#include "drivers/accessory.h"

#include "board/board.h"
#include "console/console_internal.h"
#include "console/serial_console.h"
#include "console/prompt.h"
#include "drivers/dma.h"
#include "drivers/gpio.h"
#include "drivers/exti.h"
#include "drivers/periph_config.h"
#include "drivers/uart.h"
#include "kernel/util/stop.h"
#include "mcu/interrupts.h"
#include "os/mutex.h"
#include "os/tick.h"
#include "services/common/new_timer/new_timer.h"
#include "services/common/system_task.h"
#include "system/logging.h"
#include "system/passert.h"
#include "kernel/util/delay.h"
#include "kernel/util/sleep.h"
#include "util/attributes.h"
#include "util/likely.h"
#include "util/size.h"

#include "FreeRTOS.h"       /* FreeRTOS Kernel Prototypes/Constants.          */
#include "semphr.h"

#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>


//! The default baudrate for the accessory UART.
#define DEFAULT_BAUD AccessoryBaud115200
//! How long each interval should be in milliseconds.
#define ACCESSORY_STOP_INTERVAL_PERIOD_MS (250)
//! How many intervals we should wait outside of stop mode when we first see any noise on the
//! serial port.
#define ACCESSORY_INITIAL_STOP_INTERVALS (500 / ACCESSORY_STOP_INTERVAL_PERIOD_MS)
//! How many intervals we should wait outside of stop mode when we first see valid data on the
//! serial port.
#define ACCESSORY_VALID_DATA_STOP_INTERVALS (3000 / ACCESSORY_STOP_INTERVAL_PERIOD_MS)
//! How many bytes of send history to keep. This needs to be 3 bytes because the TX buffer will be
//! moved into the shift register (with a new byte being loaded into the buffer) before we receive
//! the byte we previously sent. So, when we receive a byte, we will have sent 2 more bytes by then.
#define SEND_HISTORY_LENGTH (1)
//! Within accessory_send_stream(), how long we wait for a byte to be sent before timing-out.
#define SEND_BYTE_TIMEOUT_MS (100)

//! We DMA into this buffer as a circular buffer
#define RX_BUFFER_LENGTH (200)
static uint8_t DMA_BSS s_rx_buffer[RX_BUFFER_LENGTH];

//! The current baud rate
static uint32_t s_baudrate;
//! Whether or not the accessory power is enabled
static bool s_power_enabled;
//! Whether or not we are in input mode (receiving)
static bool s_input_enabled;
//! We'll store up to the last 3 bytes which were sent for detecting bus contention
typedef struct {
  uint8_t data;
  bool has_data;
} SendHistory;
static volatile SendHistory s_send_history;
//! Flag which states whether or not we've detected bus contention since last disabling input
static volatile bool s_bus_contention_detected;
//! Whether or not we sent data since disabling input
static bool s_sent_data;
//! The callback for a stream being sent via accessory_send_stream()
static volatile AccessoryDataStreamCallback s_stream_cb;
//! Context passed to accessory_send_stream()
static void *s_stream_context;
//! Semaphore used for accessory_send_stream()
static SemaphoreHandle_t s_send_semaphore;
//! Mutex used for accessory_block() / accessory_unblock()
static PebbleRecursiveMutex *s_blocked_lock;
//! Used to track whether or not the accessory_send_stream callback sent a new byte via
//! accessory_send_byte()
static volatile bool s_did_send_byte;
//! Whether or not we should use DMA for receiving
static bool s_use_dma;
//! Whether or not DMA is enabled
static bool s_dma_enabled;
//! Used by accessory_send_stream() to track whether or not we've sent a byte recently
static volatile bool s_has_sent_byte;

//! We need to disable stop mode in order to receive data on the accessory connector. To do this,
//! we set up an exti that kicks us out of stop mode when data is seen. Then, we schedule a timer
//! to check for additional data being seen on the connector. If we go 5 seconds without seeing
//! data, we can go back into stop mode.
static struct {
  //! If the accessory connector is currently active...
  bool active;

  //! The timer that will fire once a second while we're active
  TimerID timer;

  //! How many intervals have gone by without data being seen
  int intervals_without_data;

  //! How many intervals we should wait for without data before going back into stop mode
  int max_intervals_without_data;

  //! If we saw data on the connector since the last time the timer fired.
  bool data_seen_this_interval;
} s_stop_mode_monitor;

static bool prv_rx_irq_handler(UARTDevice *dev, uint8_t data, const UARTRXErrorFlags *err_flags);
static bool prv_tx_irq_handler(UARTDevice *dev);


static void prv_lock(void) {
  if (mcu_state_is_isr()) {
    // assume we're in an ISR for the UART and don't need to worry about being blocked
    return;
  }
  mutex_lock_recursive(s_blocked_lock);
}

static void prv_unlock(void) {
  if (mcu_state_is_isr()) {
    // assume we're in an ISR for the UART and don't need to worry about being blocked
    return;
  }
  mutex_unlock_recursive(s_blocked_lock);
}

static void prv_enable_dma(void) {
  PBL_ASSERTN(!s_dma_enabled);
  s_dma_enabled = true;
  uart_start_rx_dma(ACCESSORY_UART, s_rx_buffer, sizeof(s_rx_buffer));
}

static void prv_disable_dma(void) {
  if (!s_dma_enabled) {
    return;
  }
  s_dma_enabled = false;
  uart_stop_rx_dma(ACCESSORY_UART);
}

//! The interval timer callback.
static void prv_timer_interval_expired_cb(void *data) {
  if (!s_stop_mode_monitor.data_seen_this_interval) {
    // The accessory connector didn't have any data since the last time this callback fired.
    ++s_stop_mode_monitor.intervals_without_data;

    if (s_stop_mode_monitor.intervals_without_data >=
        s_stop_mode_monitor.max_intervals_without_data) {
      // Enough intervals have passed and we should now turn stop mode back on.
      stop_mode_enable(InhibitorAccessory);

      s_stop_mode_monitor.active = false;
      s_stop_mode_monitor.intervals_without_data = 0;
      s_stop_mode_monitor.max_intervals_without_data = 0;

      new_timer_stop(s_stop_mode_monitor.timer);
    }
  } else {
    // Data was seen, reset the interval counter
    s_stop_mode_monitor.intervals_without_data = 0;
  }

  // Regardless of what happened, this interval is over and should be reset
  s_stop_mode_monitor.data_seen_this_interval = false;
}

static void prv_start_timer_cb(void *context) {
  new_timer_start(s_stop_mode_monitor.timer, ACCESSORY_STOP_INTERVAL_PERIOD_MS,
                  prv_timer_interval_expired_cb, NULL, TIMER_START_FLAG_REPEATING);
}

//! Callback run whenever the EXTI fires
static void prv_exti_cb(bool *should_context_switch) {
  if (!s_stop_mode_monitor.active) {
    // First time seeing data, let's go active

    s_stop_mode_monitor.active = true;
    s_stop_mode_monitor.intervals_without_data = 0;
    s_stop_mode_monitor.max_intervals_without_data = ACCESSORY_INITIAL_STOP_INTERVALS;

    stop_mode_disable(InhibitorAccessory);

    // Need to flip tasks because we can't start a timer from an interrupt
    system_task_add_callback_from_isr(prv_start_timer_cb, NULL, should_context_switch);
  }

  s_stop_mode_monitor.data_seen_this_interval = true;
}

//! The UART peripheral only runs if the accessory is not in stop mode. We listen to the txrx
//! pin on the accessory connector and if we see anything we'll disable stop mode for a few
//! seconds to see if anyone has something to say.
static void prv_initialize_exti(void) {
  s_stop_mode_monitor.timer = new_timer_create();

  gpio_input_init(&BOARD_CONFIG_ACCESSORY.int_gpio);
  exti_configure_pin(BOARD_CONFIG_ACCESSORY.exti, ExtiTrigger_Falling, prv_exti_cb);
  exti_enable(BOARD_CONFIG_ACCESSORY.exti);
}

static void prv_initialize_uart(uint32_t baudrate) {
#if RECOVERY_FW
  // In PRF / MFG, we'll have a strong (2k) external pull-up, so we should always be open-drain
  const bool is_open_drain = true;
#else
  // If we raise the baud rate above 115200 we need to configure as push-pull to ensure we are
  // sufficiently driving the line. Ideally, the accessory would have a strong-enough pull-up, but
  // now that we've released use of the accessory port via the smartstrap APIs, we can't easily
  // change this.
  const bool is_open_drain = (baudrate <= 115200);
#endif
  s_baudrate = baudrate;
  if (is_open_drain) {
    uart_init_open_drain(ACCESSORY_UART);
  } else {
    uart_init(ACCESSORY_UART);
  }
  uart_set_rx_interrupt_handler(ACCESSORY_UART, prv_rx_irq_handler);
  uart_set_tx_interrupt_handler(ACCESSORY_UART, prv_tx_irq_handler);
  uart_set_baud_rate(ACCESSORY_UART, s_baudrate);
  uart_set_rx_interrupt_enabled(ACCESSORY_UART, true);
}

static void prv_initialize_hardware(void) {
  periph_config_acquire_lock();

  gpio_output_init(&BOARD_CONFIG_ACCESSORY.power_en, GPIO_OType_PP, GPIO_Speed_2MHz);
  gpio_output_set(&BOARD_CONFIG_ACCESSORY.power_en, false);  // Turn power off

  accessory_set_baudrate(DEFAULT_BAUD);

  periph_config_release_lock();

  prv_initialize_exti();
}

static void prv_set_baudrate(uint32_t baudrate, bool force_update) {
  if ((baudrate != s_baudrate) || force_update) {
    PBL_LOG(LOG_LEVEL_DEBUG, "Changing accessory connector baud rate to %"PRIu32, baudrate);
    prv_initialize_uart(baudrate);
    if (s_dma_enabled) {
      // we need to reset DMA after resetting the UART
      prv_disable_dma();
      prv_enable_dma();
    }
  }
}

void accessory_init(void) {
  s_send_semaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(s_send_semaphore);
  s_blocked_lock = mutex_create_recursive();
  prv_initialize_hardware();
  accessory_set_power(false);
  accessory_enable_input();
}

void accessory_block(void) {
  prv_lock();
  accessory_send_stream_stop();
  uart_deinit(ACCESSORY_UART);
}

void accessory_unblock(void) {
  // We want to restore the previous baudrate, but clear s_baudrate in order to force a complete
  // re-init of the peripheral.
  prv_set_baudrate(s_baudrate, true /* force_update */);
  prv_unlock();
}

void accessory_send_byte(uint8_t data) {
  // NOTE: this may be run within an ISR
  prv_lock();
  s_has_sent_byte = true;
  s_did_send_byte = true;
  PBL_ASSERTN(!s_input_enabled);
  while (!(uart_is_tx_ready(ACCESSORY_UART))) continue;
  // this section needs to be atomic since the UART IRQ also modifies these variables
  portENTER_CRITICAL();
  if (s_send_history.has_data) {
    // The send buffer is full. This means that the receive interrupt hasn't fired to clear the
    // buffer which indicates that there is bus contention preventing a stop bit from occurring.
    s_bus_contention_detected = true;
  } else {
    s_send_history.data = data;
    s_send_history.has_data = true;
  }
  portEXIT_CRITICAL();
  uart_write_byte(ACCESSORY_UART, data);
  s_sent_data = true;
  prv_unlock();
}

void accessory_send_data(const uint8_t *data, size_t length) {
  // NOTE: this may be run within an ISR
  prv_lock();
  // When sending data, we need to temporarily disable input, as there's only one data line for
  // both directions and any data we send on that line will also be interpreted as data we can
  // read. This means there's a bit of overhead for sending data as we have to also make sure
  // we don't accidentally read it back. If you're going to be sending a large amount of data,
  // calling accessory_disable_input before will give you a nice speed boost as we don't have
  // to wait for it to be safe to turn the input back on after each byte.

  const bool temporarily_disabled = s_input_enabled;
  if (UNLIKELY(temporarily_disabled)) {
    accessory_disable_input();
  }

  for (size_t i = 0; i < length; ++i) {
    accessory_send_byte(data[i]);
  }

  if (UNLIKELY(temporarily_disabled)) {
    accessory_enable_input();
  }
  prv_unlock();
}

bool accessory_send_stream(AccessoryDataStreamCallback stream_callback, void *context) {
  bool success = true;
  prv_lock();
  PBL_ASSERTN(xSemaphoreTake(s_send_semaphore, portMAX_DELAY) == pdPASS);
  PBL_ASSERTN(stream_callback != NULL);
  PBL_ASSERTN(!s_input_enabled);
  if (s_dma_enabled) {
    uart_clear_rx_dma_buffer(ACCESSORY_UART);
  }
  s_stream_context = context;
  s_stream_cb = stream_callback;
  s_has_sent_byte = false;
  uart_set_tx_interrupt_enabled(ACCESSORY_UART, true);
  // Block until the sending is complete, but timeout if we aren't able to send a byte for a while.
  while (xSemaphoreTake(s_send_semaphore, milliseconds_to_ticks(SEND_BYTE_TIMEOUT_MS)) != pdPASS) {
    if (!s_has_sent_byte) {
      // we haven't sent a byte in the last timeout period, so time out the whole send
      s_stream_cb = NULL;
      s_stream_context = NULL;
      success = false;
      PBL_LOG(LOG_LEVEL_ERROR, "Timed-out while sending");
      break;
    }
    s_has_sent_byte = false;
  }
  xSemaphoreGive(s_send_semaphore);
  prv_unlock();
  return success;
}

void accessory_send_stream_stop(void) {
  prv_lock();
  if (s_stream_cb) {
    // wait for any in-progress write to finish
    PBL_ASSERTN(xSemaphoreTake(s_send_semaphore, portMAX_DELAY) == pdPASS);
    xSemaphoreGive(s_send_semaphore);
  }
  uart_set_tx_interrupt_enabled(ACCESSORY_UART, false);
  s_stream_cb = NULL;
  s_stream_context = NULL;
  prv_unlock();
}

void accessory_disable_input(void) {
  // NOTE: This function may be called from an ISR
  prv_lock();
  PBL_ASSERTN(s_input_enabled);

  s_input_enabled = false;
  s_send_history.has_data = false;
  s_bus_contention_detected = false;
  prv_unlock();
}

void accessory_enable_input(void) {
  // NOTE: This function may be called from an ISR
  if (s_input_enabled) {
    return;
  }

  prv_lock();
  if (s_sent_data) {
    // wait for the TC flag to be set
    uart_wait_for_tx_complete(ACCESSORY_UART);
    // wait a little for the lines to settle down
    const uint32_t us_to_wait = (1000000 / s_baudrate) * 2;
    delay_us(us_to_wait);
    s_sent_data = false;
  }

  // Read data and throw it away to clear the state. We don't want to handle data we received
  // while input was disabled
  uart_read_byte(ACCESSORY_UART);

  s_input_enabled = true;
  prv_unlock();
}

void accessory_use_dma(bool use_dma) {
  prv_lock();
  s_use_dma = use_dma;
  if (s_use_dma) {
    prv_enable_dma();
  } else {
    prv_disable_dma();
  }
  prv_unlock();
}

bool accessory_bus_contention_detected(void) {
  return s_bus_contention_detected;
}

static uint32_t prv_get_baudrate(AccessoryBaud baud_select) {
  const uint32_t BAUDS[] = { 9600, 14400, 19200, 28800, 38400, 57600, 62500, 115200, 125000, 230400,
                             250000, 460800, 921600 };
  _Static_assert(ARRAY_LENGTH(BAUDS) == AccessoryBaudInvalid,
                 "bauds table doesn't match up with AccessoryBaud enum");
  return BAUDS[baud_select];
}

void accessory_set_baudrate(AccessoryBaud baud_select) {
  prv_lock();
  PBL_ASSERTN(baud_select < AccessoryBaudInvalid);
  prv_set_baudrate(prv_get_baudrate(baud_select), false /* !force_update */);
  prv_unlock();
}

void accessory_set_power(bool on) {
  if (on == s_power_enabled) {
    return;
  }
  PBL_LOG(LOG_LEVEL_DEBUG, "Setting accessory power %s", on?"on":"off");
  s_power_enabled = on;
  gpio_output_set(&BOARD_CONFIG_ACCESSORY.power_en, on);
}

bool accessory_is_present(void) {
  accessory_set_power(true);
  gpio_input_init_pull_up_down(&BOARD_CONFIG_ACCESSORY.int_gpio, GPIO_PuPd_DOWN);
  // budget for a capacitance up to ~1uF and a resistance of 10kOhm
  psleep(10);
  bool result = (gpio_input_read(&BOARD_CONFIG_ACCESSORY.int_gpio) == SET);
  gpio_input_init(&BOARD_CONFIG_ACCESSORY.int_gpio);
  return result;
}

// ISRs
////////////////////////////////////////////////////////////////////

static bool prv_rx_irq_handler(UARTDevice *dev, uint8_t data, const UARTRXErrorFlags *err_flags) {
  bool should_context_switch = false;
  // We've now seen valid data on the serial port, make sure we stay out of stop mode for a
  // longer period of time.
  s_stop_mode_monitor.max_intervals_without_data = ACCESSORY_VALID_DATA_STOP_INTERVALS;
  if (s_input_enabled) {
    // we are receiving data from the accessory
    if (!err_flags->framing_error) {
      should_context_switch = accessory_manager_handle_character_from_isr((char)data);
    } else if (data == 0x00) {
      should_context_switch = accessory_manager_handle_break_from_isr();
    }
  } else {
    // we are receiving data we just sent since the RX/TX lines are tied together
    if (s_send_history.has_data) {
      if (s_send_history.data != data) {
        // The byte we are receiving doesn't match the next byte in the send queue.
        s_bus_contention_detected = true;
      }
      s_send_history.has_data = false;
    } else {
      // We received a byte without sending and the input is not enabled. This typically indicates
      // a race condition between when we disable input and start sending, or between when we
      // finish sending and enable input. Either way, we can't trust this data so treat it as bus
      // contention.
      s_bus_contention_detected = true;
    }
  }
  if (s_stream_cb) {
    // enable the TXE interrupt for sending the next byte
    uart_set_tx_interrupt_enabled(dev, true);
  }
  return should_context_switch;
}

static bool prv_tx_irq_handler(UARTDevice *dev) {
  bool should_context_switch = false;
  if (s_stream_cb && !s_send_history.has_data) {
    s_did_send_byte = false;
    if (s_stream_cb(s_stream_context)) {
      // the callback MUST send a byte in order for this interrupt to trigger again
      PBL_ASSERTN(s_did_send_byte);
    } else {
      // we're done sending
      portBASE_TYPE was_higher_task_woken = pdFALSE;
      xSemaphoreGiveFromISR(s_send_semaphore, &was_higher_task_woken);
      should_context_switch = (was_higher_task_woken != pdFALSE);
      uart_set_tx_interrupt_enabled(dev, false);
      s_stream_cb = NULL;
      s_stream_context = NULL;
    }
  } else {
    // we haven't yet received back the byte we sent
    uart_set_tx_interrupt_enabled(dev, false);
  }
  return should_context_switch;
}

// Commands
////////////////////////////////////////////////////////////////////
void command_accessory_power_set(const char *on) {
  if (!strcmp(on, "on")) {
    accessory_set_power(true);
  } else if (!strcmp(on, "off")) {
    accessory_set_power(false);
  } else {
    prompt_send_response("Usage: accessory power (on|off)");
  }
}

static volatile int32_t s_num_test_bytes;
static bool prv_test_send_stream(void *context) {
  accessory_send_byte((uint8_t)s_num_test_bytes);
  if (accessory_bus_contention_detected()) {
    return false;
  }
  return (--s_num_test_bytes > 0);
}

void command_accessory_stress_test(void) {
  if (s_baudrate != prv_get_baudrate(DEFAULT_BAUD)) {
    prompt_send_response("FAILED: accessory port is busy");
    return;
  }

  // send 1 second worth of data
  s_num_test_bytes = 46080;
  accessory_use_dma(true);
  accessory_set_baudrate(AccessoryBaud460800);
  accessory_disable_input();
  const bool success = accessory_send_stream(prv_test_send_stream, NULL);
  accessory_enable_input();
  accessory_set_baudrate(DEFAULT_BAUD);
  accessory_use_dma(false);

  char buffer[50];
  if (!success) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "FAILED: send timed-out");
  } else if (s_num_test_bytes == 0) {
    prompt_send_response_fmt(buffer, sizeof(buffer), "PASS!");
  } else {
    prompt_send_response_fmt(buffer, sizeof(buffer), "FAILED: %"PRId32" bytes left!",
                             s_num_test_bytes);
  }
}
