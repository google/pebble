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

/*
 * This module contains the core dump logic which writes the core dump to SPI flash. It operates
 *  under a very limited set of constraints:
 *   1.) It can NOT use most FreeRTOS functions
 *   2.) It can not use the regular flash driver (because that uses FreeRTOS mutexes)
 *
 * There is a separate module, core_dump_protocol.c which implements the session endpoint logic for
 * fetching the core dump over bluetooth. That module is free to use FreeRTOS, regular flash
 * driver, etc.
 */

#include "kernel/core_dump.h"
#include "kernel/core_dump_private.h"

#include "console/dbgserial.h"
#include "kernel/logging_private.h"
#include "kernel/pulse_logging.h"

#include "drivers/flash.h"
#include "drivers/mpu.h"
#include "drivers/spi.h"
#include "drivers/watchdog.h"
#include "drivers/rtc.h"

#include "flash_region/flash_region.h"
#include "kernel/pbl_malloc.h"
#include "mfg/mfg_serials.h"

#include "pebbleos/chip_id.h"
#include "services/common/comm_session/session.h"

#include "system/bootbits.h"
#include "system/passert.h"
#include "system/reset.h"
#include "system/logging.h"
#include "system/version.h"

#include "util/attributes.h"
#include "util/build_id.h"
#include "util/math.h"
#include "util/net.h"
#include "util/size.h"
#include "util/string.h"

#define STM32F2_COMPATIBLE
#define STM32F4_COMPATIBLE
#define STM32F7_COMPATIBLE
#include <mcu.h>

#include "FreeRTOS.h"       /* FreeRTOS Kernel Prototypes/Constants.          */
#include "task.h"           /* FreeRTOS Task Prototypes/Constants.            */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>


//! Evaluates to 1 iff execution will use the process stack when returning from
//! the exception.
#define RETURNS_TO_PSP(exc_return) ((exc_return & 0x4) == 0x4)

//! This symbol and its contents are provided by the linker script, see the
//! .note.gnu.build-id section in src/fw/stm32f2xx_flash_fw.ld
extern const ElfExternalNote TINTIN_BUILD_ID;

extern const uint32_t __CCM_RAM_size__[];
extern const uint32_t __DTCM_RAM_size__[];

void cd_flash_init(void);
uint32_t cd_flash_write_bytes(const void* buffer_ptr, uint32_t start_addr, uint32_t buffer_size);
void cd_flash_erase_region(uint32_t start_addr, uint32_t total_bytes);
void cd_flash_read_bytes(void* buffer_ptr, uint32_t start_addr, uint32_t buffer_size);

// ----------------------------------------------------------------------------------------
// Private globals

static uint32_t s_flash_addr;                   // next address in flash to write to
// Saved registers before we trigger our interrupt: [r0-r12, sp, lr, pc, xpsr]
static ALIGN(4) CoreDumpSavedRegisters s_saved_registers;
static uint32_t s_time_stamp;
static bool s_core_dump_initiated = false;
static bool s_core_dump_is_forced = false;
static bool s_test_force_bus_fault = false;     // Used for unit testing
static bool s_test_force_inf_loop = false;      // Used for unit testing
static bool s_test_force_assert = false;        // Used for unit testing


// List of memory regions to include in the core dump
typedef struct {
  void*    start;
  uint32_t length;
  bool     word_reads_only;  // Some peripherals can only be read 32 bits at a
                             // time, or you BusFault (maybe). Set this to true
                             // for memory regions where reads smaller than 32
                             // bits will fail. The start pointer must also be
                             // word-aligned.
} MemoryRegion;

// Memory regions to dump
static const MemoryRegion MEMORY_REGIONS_DUMP[] = {
#if MICRO_FAMILY_STM32F2
  { .start = (void *)SRAM_BASE, .length = COREDUMP_RAM_SIZE },
#else
  { .start = (void *)SRAM1_BASE, .length = COREDUMP_RAM_SIZE },
#endif
#if PLATFORM_SNOWY || PLATFORM_SPALDING
  { .start = (void *)CCMDATARAM_BASE, .length = (uint32_t)__CCM_RAM_size__ },
#endif
#if MICRO_FAMILY_STM32F7
  { .start = (void *)RAMDTCM_BASE, .length = (uint32_t)__DTCM_RAM_size__ },
#endif
  { .start = (void *)RCC, .length = sizeof(*RCC) },
  { .start = (void *)&NVIC->ISER, .length = sizeof(NVIC->ISER) },  // Enabled interrupts
  { .start = (void *)&NVIC->ISPR, .length = sizeof(NVIC->ISPR) },  // Pending interrupts
  { .start = (void *)&NVIC->IABR, .length = sizeof(NVIC->IABR) },  // Active interrupts
  { .start = (void *)&NVIC->IP, .length = sizeof(NVIC->IP) },  // Interrupt priorities
  { .start = (void *)RTC, .length = sizeof(*RTC) },
  { .start = (void*)DMA1_BASE, .length = 0xD0, .word_reads_only = true },
  { .start = (void*)DMA2_BASE, .length = 0xD0, .word_reads_only = true },
};

static struct {
  RCC_TypeDef rcc;
  SPI_TypeDef spi1;
} s_stash_data;

// -------------------------------------------------------------------------------------------------
// Flash driver dual-API.
static bool s_use_cd_flash_driver = true;

static uint32_t prv_flash_write_bytes(const void* buffer_ptr,
                                      uint32_t start_addr, uint32_t buffer_size) {
  if (s_use_cd_flash_driver) {
    return cd_flash_write_bytes(buffer_ptr, start_addr, buffer_size);
  } else {
    flash_write_bytes(buffer_ptr, start_addr, buffer_size);
    return buffer_size;
  }
}

static void prv_flash_erase_region(uint32_t start_addr, uint32_t total_bytes) {
  if (s_use_cd_flash_driver) {
    cd_flash_erase_region(start_addr, total_bytes);
  } else {
    uint32_t end = start_addr + total_bytes;
    flash_region_erase_optimal_range_no_watchdog(start_addr, start_addr, end, end);
  }
}

static void prv_flash_read_bytes(void* buffer_ptr, uint32_t start_addr, uint32_t buffer_size) {
  if (s_use_cd_flash_driver) {
    cd_flash_read_bytes(buffer_ptr, start_addr, buffer_size);
  } else {
    flash_read_bytes(buffer_ptr, start_addr, buffer_size);
  }
}

// -------------------------------------------------------------------------------------------------
// NOTE: We are explicitly avoiding use of vsniprintf and cohorts to reduce our stack
// requirements
static void prv_debug_str(const char* msg) {
  kernel_pbl_log_from_fault_handler(__FILE_NAME__, 0, msg);
}


// -------------------------------------------------------------------------------------------------
// NOTE: We are explicitly avoiding use of vsniprintf and cohorts to reduce our stack
// requirements
static void prv_debug_str_str(const char* msg, const char* s) {
#if PULSE_EVERYWHERE
  void *ctx = pulse_logging_log_sync_begin(LOG_LEVEL_ALWAYS, __FILE_NAME__, 0);
  pulse_logging_log_sync_append(ctx, msg);
  pulse_logging_log_sync_append(ctx, s);
  pulse_logging_log_sync_send(ctx);
#else
  int max_length = 256;
  while (*msg && max_length--) {
    dbgserial_putchar(*msg);
    ++msg;
  }
  dbgserial_putstr(s);
#endif
}


// -------------------------------------------------------------------------------------------------
// NOTE: We are explicitly avoiding use of vsniprintf and cohorts to reduce our stack
// requirements
static void prv_debug_str_int(const char* msg, uint32_t i) {
  char buffer[12];
  itoa(i, buffer, sizeof(buffer));

#if PULSE_EVERYWHERE
  void *ctx = pulse_logging_log_sync_begin(LOG_LEVEL_ALWAYS, __FILE_NAME__, 0);
  pulse_logging_log_sync_append(ctx, msg);
  pulse_logging_log_sync_append(ctx, buffer);
  pulse_logging_log_sync_send(ctx);
#else
  int max_length = 256;
  while (*msg && max_length--) {
    dbgserial_putchar(*msg);
    ++msg;
  }
  dbgserial_putstr(buffer);
#endif
}

static NORETURN prv_reset(void) {
  dbgserial_flush();
  system_hard_reset();
}

// -----------------------------------------------------------------------------------------
void coredump_assert(int line) {
  prv_debug_str_int("CD: assert - line ", line);
  boot_bit_set(BOOT_BIT_SOFTWARE_FAILURE_OCCURRED);
  prv_reset();
}

// -------------------------------------------------------------------------------------------------
// Stash the flash status registers and peripheral clock state before the flash
// driver messes with them.
static void prv_stash_regions(void) {
  memcpy(&s_stash_data.rcc, RCC, sizeof(RCC_TypeDef));
  memcpy(&s_stash_data.spi1, SPI1, sizeof(SPI_TypeDef));
}

// -----------------------------------------------------------------------------------------------
// Return the start address of the flash region containing the core dump image. We write the core image to
// different regions in flash to avoid premature burnout of any particular region.
// @param[in] new If true, then return a pointer to a region where a new image can be stored.
//                If false, then return the region containing the most recent stored image or
//                  CORE_DUMP_FLASH_INVALID_ADDR if no image has been written.
// @return flash base address to use
static uint32_t prv_flash_start_address(bool new) {
  CoreDumpFlashHeader flash_hdr;
  CoreDumpFlashRegionHeader region_hdr;
  uint32_t  base_address;
  unsigned int i;


  // ----------------------------------------------------------------------------------
  // First, see if the flash header has been put in place
  prv_flash_read_bytes(&flash_hdr, CORE_DUMP_FLASH_START, sizeof(flash_hdr));

  if (flash_hdr.magic != CORE_DUMP_FLASH_HDR_MAGIC) {
    prv_flash_erase_region(CORE_DUMP_FLASH_START, SUBSECTOR_SIZE_BYTES);
    flash_hdr = (CoreDumpFlashHeader) {
      .magic = CORE_DUMP_FLASH_HDR_MAGIC,
      .unformatted = CORE_DUMP_ALL_UNFORMATTED,
    };
    prv_flash_write_bytes(&flash_hdr, CORE_DUMP_FLASH_START, sizeof(flash_hdr));
  }

  // If asking for an existing region and no regions have been formatted yet, return not found
  if (!new && flash_hdr.unformatted == CORE_DUMP_ALL_UNFORMATTED) {
    return CORE_DUMP_FLASH_INVALID_ADDR;
  }

  // ----------------------------------------------------------------------------------
  // Find which region was most recently used (highest last_used value).
  uint32_t max_last_used = 0;
  int last_used_idx = -1;

  for (i=0; i<CORE_DUMP_MAX_IMAGES; i++) {
    // Skip if unformatted
    if (flash_hdr.unformatted & (1 << i)) {
      continue;
    }
    base_address = core_dump_get_slot_address(i);
    prv_flash_read_bytes(&region_hdr, base_address, sizeof(region_hdr));

    // Skip if not written correctly or not most recently used
    if (region_hdr.magic == CORE_DUMP_FLASH_HDR_MAGIC && region_hdr.last_used > max_last_used) {
      max_last_used = region_hdr.last_used;
      last_used_idx = i;
    }
  }

  // If simply trying to find most recently used image, return that now.
  if (!new) {
    if (max_last_used > 0) {
      CD_ASSERTN(last_used_idx >= 0);
      return core_dump_get_slot_address(last_used_idx);
    } else {
      return CORE_DUMP_FLASH_INVALID_ADDR;
    }
  }

  // ----------------------------------------------------------------------------------
  // We need to write a new image. Find which region to put it in.
  // If no regions yet, pick one at random
  unsigned int start_idx;
  if (max_last_used == 0) {
    start_idx = s_time_stamp % CORE_DUMP_MAX_IMAGES;
  } else {
    // Else, put it into the next region
    start_idx = (last_used_idx + 1) % CORE_DUMP_MAX_IMAGES;
  }

  // Erase the new region and write out the region header
  base_address = core_dump_get_slot_address(start_idx);
  CD_ASSERTN(base_address + CORE_DUMP_MAX_SIZE <= CORE_DUMP_FLASH_END);
  prv_flash_erase_region(base_address, CORE_DUMP_MAX_SIZE);
  region_hdr = (CoreDumpFlashRegionHeader) {
    .magic = CORE_DUMP_FLASH_HDR_MAGIC,
    .last_used = max_last_used + 1,
    .unread = true,
  };
  prv_flash_write_bytes(&region_hdr, base_address, sizeof(region_hdr));

  // Clear the unformatted bit in the flash region header
  flash_hdr.unformatted &= ~(1 << start_idx);
  prv_flash_write_bytes(&flash_hdr, CORE_DUMP_FLASH_START, sizeof(flash_hdr));

  return base_address;
}


// -------------------------------------------------------------------------------------------------
// This callback gets called by FreeRTOS for each task during the call to vTaskListWalk.
static void prvTaskInfoCallback( const xPORT_TASK_INFO * const task_info, void * data) {
  CoreDumpChunkHeader chunk_hdr;
  CoreDumpThreadInfo packed_info;

  void *current_task_id = (void *)xTaskGetCurrentTaskHandle();

  prv_debug_str_str("CD: Th info ", task_info->pcName);

  // Unit testing various types of fault?
  if (s_test_force_bus_fault) {
    typedef void (*KaboomCallback)(void);
    KaboomCallback kaboom = 0;
    kaboom();
  }
  if (s_test_force_inf_loop) {
    while (true) ;
  }
  if (s_test_force_assert) {
    PBL_ASSERTN(false);
  }

  // Create the packed chunk header
  strncpy ((char *)packed_info.name, task_info->pcName, CORE_DUMP_THREAD_NAME_SIZE);
  packed_info.id = (uint32_t)task_info->taskHandle;
  packed_info.running = (current_task_id == task_info->taskHandle);
  for (int i = 0; i < portCANONICAL_REG_COUNT; i++) {
    // registers [r0-r12, sp, lr, pc, sr]
    packed_info.registers[i] = task_info->registers[i];
  }

  // If this is the current task, adjust the registers based on whether or not we were handling
  //  an exception at the time core_dump_reset() was called.
  if (packed_info.running) {
    if (!RETURNS_TO_PSP(s_saved_registers.core_reg[portCANONICAL_REG_INDEX_LR])) {
      // The core dump handler got invoked from another exception, therefore the
      // running task was interrupted by an exception.
      // Get R0-R3, R12, R14, PC, xpsr for the task off the process stack used
      // by the task.
      // The information for this task is going to be incorrect: the values of
      // R4-R11 will be completely bogus. The only way to recover them is to
      // properly unwind the full exception stack in a debugger with unwind
      // information available. Unfortunately mainline GDB is unable to unwind
      // across the MSP/PSP split stack so this incomplete hack is required to
      // get useable information.
      for (int i = 0; i < portCANONICAL_REG_COUNT; i++) {
        // Clear out all of the bogus values in the info
        packed_info.registers[i] = 0xa5a5a5a5;
      }
      uint32_t *sp = (uint32_t *)s_saved_registers.extra_reg.psp;
      packed_info.registers[portCANONICAL_REG_INDEX_R0] = sp[0];
      packed_info.registers[portCANONICAL_REG_INDEX_R1] = sp[1];
      packed_info.registers[portCANONICAL_REG_INDEX_R2] = sp[2];
      packed_info.registers[portCANONICAL_REG_INDEX_R3] = sp[3];
      packed_info.registers[portCANONICAL_REG_INDEX_R12] = sp[4];
      packed_info.registers[portCANONICAL_REG_INDEX_LR] = sp[5];
      packed_info.registers[portCANONICAL_REG_INDEX_PC] = sp[6];
      packed_info.registers[portCANONICAL_REG_INDEX_XPSR] = sp[7];
      // Pop the exception stack frame, taking stack alignment into account.
      // The 10th bit of the pushed xPSR indicates whether an alignment word was
      // inserted into the stack frame during exception entry in order to make
      // sp 8-byte aligned.
      // Note that this is going to be wrong if the floating-point registers
      // were stacked. The only way to know for sure whether the FP regs were
      // pushed during exception entry requires unwinding the ISR stack to
      // determine the EXC_RETURN value of the bottom-most ISR.
      if (sp[7] & 0x200) {
        packed_info.registers[portCANONICAL_REG_INDEX_SP] = (uint32_t)(&sp[9]);
      } else {
        packed_info.registers[portCANONICAL_REG_INDEX_SP] = (uint32_t)(&sp[8]);
      }
    } else {
      // If current task called core_dump_reset directly, then jam in the
      // registers we saved at the beginning.
      for (int i = 0; i < portCANONICAL_REG_COUNT; i++) {
        // registers [r0-r12, msp, lr, pc, psr]
        packed_info.registers[i] = s_saved_registers.core_reg[i];
      }
      // Set sp to the saved psp so that GDB can unwind the task's stack.
      packed_info.registers[portCANONICAL_REG_INDEX_SP] =
          s_saved_registers.extra_reg.psp;
    }
  }

  // Write out this thread info
  chunk_hdr.key = CORE_DUMP_CHUNK_KEY_THREAD;
  chunk_hdr.size = sizeof(packed_info);
  s_flash_addr += prv_flash_write_bytes(&chunk_hdr, s_flash_addr,
                                        sizeof(chunk_hdr));
  s_flash_addr += prv_flash_write_bytes(&packed_info, s_flash_addr,
                                        chunk_hdr.size);
}

static void prv_write_memory_regions(const MemoryRegion *regions, unsigned int count,
                                     uint32_t flash_base) {
  CoreDumpChunkHeader chunk_hdr;
  chunk_hdr.key = CORE_DUMP_CHUNK_KEY_MEMORY;

  for (unsigned int i = 0; i < count; i++) {
    chunk_hdr.size = regions[i].length + sizeof(CoreDumpMemoryHeader);
    CD_ASSERTN(s_flash_addr + chunk_hdr.size - flash_base < CORE_DUMP_MAX_SIZE);
    s_flash_addr += prv_flash_write_bytes(&chunk_hdr, s_flash_addr,
                                          sizeof(chunk_hdr));
    CoreDumpMemoryHeader mem_hdr;
    mem_hdr.start = (uint32_t)regions[i].start;
    s_flash_addr += prv_flash_write_bytes(&mem_hdr, s_flash_addr,
                                          sizeof(mem_hdr));

    if (regions[i].word_reads_only) {
      // Copy the memory into a temporary buffer before writing it to flash so
      // that we can be sure that the memory is only being accessed by word.
      uint32_t temp;
      for (uint32_t offset = 0;
           offset < regions[i].length;
           offset += sizeof(temp)) {
        temp = *(volatile uint32_t*)((char *)regions[i].start + offset);
        s_flash_addr += prv_flash_write_bytes(&temp, s_flash_addr,
                                              sizeof(temp));
        watchdog_feed();
      }
    } else {
      uint32_t bytes_remaining = regions[i].length;
      for (uint32_t offset = 0; offset < regions[i].length; offset += SECTOR_SIZE_BYTES) {
        uint32_t bytes_to_write = MIN(bytes_remaining, SECTOR_SIZE_BYTES);
        s_flash_addr += prv_flash_write_bytes(
            (void *) ((uint32_t)regions[i].start + offset),
            s_flash_addr, bytes_to_write);
        bytes_remaining -= bytes_to_write;
        watchdog_feed();
      }
    }
  }
}

// Write the Core Dump Image Header
// Returns number of bytes written @ flash_addr
static uint32_t prv_write_image_header(uint32_t flash_addr, uint8_t core_number,
                                       const ElfExternalNote *build_id, uint32_t timestamp) {
  CoreDumpImageHeader hdr = {
    .magic = CORE_DUMP_MAGIC,
    .core_number = core_number,
    .version = CORE_DUMP_VERSION,
    .time_stamp = timestamp,
  };
  strncpy((char *)hdr.serial_number, mfg_get_serial_number(), sizeof(hdr.serial_number));
  hdr.serial_number[sizeof(hdr.serial_number)-1] = 0;
  version_copy_build_id_hex_string((char *)hdr.build_id, sizeof(hdr.build_id), build_id);
  hdr.build_id[sizeof(hdr.build_id)-1] = 0;

  return prv_flash_write_bytes(&hdr, flash_addr, sizeof(hdr));
}

// =================================================================================================
// Public interface

// -----------------------------------------------------------------------------------------------
// Trigger a core dump
NORETURN core_dump_reset(bool is_forced) {
  // Big problem if we re-enter here - it likely means we encountered an
  // exception during the core dump
  if (s_core_dump_initiated) {
    prv_debug_str("CD: re-entered");
    prv_reset();
  }
  s_core_dump_initiated = true;

  s_core_dump_is_forced = is_forced;
  if (is_forced) {
    RebootReason reason = { RebootReasonCode_ForcedCoreDump, 0};
    reboot_reason_set(&reason);
  }

  // Pend the Non-Maskable Interrupt, as the NMI handler performs the core dump.
  SCB->ICSR = SCB_ICSR_NMIPENDSET_Msk;
  __DSB();
  __ISB();
  // Shouldn't get here
  RebootReason reason = { RebootReasonCode_CoreDumpEntryFailed, 0 };
  reboot_reason_set(&reason);
  prv_reset();
}

void __attribute__((naked)) NMI_Handler(void) {
  // Save the processor state at the moment the NMI exception was entered to a
  // struct of type CoreDumpSavedRegisters.
  //
  // Save the processor state which is not automatically stacked during
  // exception entry before any C code can clobber it.
  __asm volatile (
    "  ldr r0, =%[s_saved_registers]\n"
    "  stmia r0!, {r4-r11}          \n"
    "  str sp, [r0, #4]!            \n"       // sp, skipping r12
    "  str lr, [r0, #4]!            \n"       // lr
    "  mrs r1, xpsr                 \n"
    "  mrs r2, msp                  \n"
    "  mrs r3, psp                  \n"
    "  adds r0, #8                  \n"       // skip pc
    "  stmia r0!, {r1-r3}           \n"       // xpsr, msp, psp
    "  b core_dump_handler_c    \n"
    :
    : [s_saved_registers] "i"
          (&s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R4])
    : "r0", "r1", "r2", "r3", "cc"
    );
}

EXTERNALLY_VISIBLE void core_dump_handler_c(void) {
  // Locate the stack pointer where the processor state was stacked before the
  // NMI handler was executed so that the saved state can be copied into
  // s_saved_registers.
  uint32_t *process_sp = (uint32_t *)(
    RETURNS_TO_PSP(s_saved_registers.core_reg[portCANONICAL_REG_INDEX_LR])?
      s_saved_registers.extra_reg.psp : s_saved_registers.extra_reg.msp);
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R0] = process_sp[0];
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R1] = process_sp[1];
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R2] = process_sp[2];
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R3] = process_sp[3];
  // Replace the r12 saved earlier with the real value.
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_R12] = process_sp[4];
  // Make it look like the processor had halted at the start of this function.
  s_saved_registers.core_reg[portCANONICAL_REG_INDEX_PC] = (uint32_t)NMI_Handler;
  // Save the special registers that the C compiler won't clobber.
  s_saved_registers.extra_reg.primask = __get_PRIMASK();
  s_saved_registers.extra_reg.basepri = __get_BASEPRI();
  s_saved_registers.extra_reg.faultmask = __get_FAULTMASK();
  s_saved_registers.extra_reg.control = __get_CONTROL();

  // if we coredump after new fw has been installed but before we reboot, the
  // FW image will be overwritten with a coredump. Clear the boot bits so we
  // don't try and load the resources which would result in us dropping to PRF
  if (boot_bit_test(BOOT_BIT_NEW_FW_AVAILABLE)) {
    boot_bit_clear(BOOT_BIT_NEW_FW_AVAILABLE);
    boot_bit_clear(BOOT_BIT_NEW_SYSTEM_RESOURCES_AVAILABLE);
  }

  // Normally a reboot reason would be set before initiating a core dump. In
  // case this isn't true, set a default reason so that we know the reboot was
  // because of a core dump.
  RebootReason reason;
  reboot_reason_get(&reason);
  if (reason.code == RebootReasonCode_Unknown) {
    reason = (RebootReason) { RebootReasonCode_CoreDump, 0 };
    reboot_reason_set(&reason);
  }

  prv_debug_str("Starting core dump");

  // Save the current time now because rtc_get_ticks() disables and then re-enables interrupts
  s_time_stamp = rtc_get_time();

  prv_debug_str("CD: starting");

  // Feed the watchdog so that we don't get watchdog reset in the middle of dumping the core
  watchdog_feed();

  prv_stash_regions();

  // Init the flash and SPI bus
  s_use_cd_flash_driver = true;
  cd_flash_init();

  // If there is a fairly recent unread core image already present, don't replace it. Once it is read through
  // the get_bytes_protocol_msg_callback(), the unread flag gets cleared out.
  uint32_t flash_base;
  flash_base = prv_flash_start_address(false /*new*/);
  if (!s_core_dump_is_forced && flash_base != CORE_DUMP_FLASH_INVALID_ADDR) {
    CoreDumpFlashRegionHeader region_hdr;
    CoreDumpImageHeader image_hdr;
    prv_debug_str_int("CD: Checking: ", flash_base);
    prv_flash_read_bytes(&region_hdr, flash_base, sizeof(region_hdr));
    prv_flash_read_bytes(&image_hdr, flash_base + sizeof(region_hdr), sizeof(image_hdr));

    if ((image_hdr.magic == CORE_DUMP_MAGIC) && region_hdr.unread
        && ((s_time_stamp - image_hdr.time_stamp) < CORE_DUMP_MIN_AGE_SECONDS)) {
      prv_debug_str("CD: Still fresh");
      #ifndef IS_BIGBOARD
      prv_reset();
      #else
      prv_debug_str("CD: BigBoard, forcing dump");
      #endif
    }
  }

  // Get flash address to save new image to. This method also pre-erases the region for us.
  flash_base = prv_flash_start_address(true /*new*/);
  prv_debug_str_int("CD: Saving to: ", flash_base);

  // ---------------------------------------------------------------------------------------
  // Dump RAM and thread info into flash. We store data in flash using the following format:
  //
  // CoreDumpImageHeader  image_header          // includes magic signature, version, time stamp, serial number
  //                                         //  and build id.
  //
  // uint32_t          chunk_key             // CORE_DUMP_CHUNK_KEY_MEMORY, CORE_DUMP_CHUNK_KEY_THREAD, etc.
  // uint32_t          chunk_size            // # of bytes of data that follow
  // uint8_t           chunk[chunk_size]     // data for the above chunk
  //
  // uint32_t          chunk_key
  // uint32_t          chunk_size
  // uint8_t           chunk[chunk_size]
  // ...
  // uint32_t          0xFFFFFFFF            // terminates list
  //
  // For threads, we store a CoreDumpThreadInfo structure as the "chunk":
  //  chunk_key = 'THRD'
  //  chunk[] = { uint8_t  name[16];      // includes null termination
  //              uint32_t id;            // thread id
  //              uint8_t  running;       // true if this thread is running
  //              uint32_t registers[17]; // thread registers [r0-r12, sp, lr, pc, xpsr]
  //            }
  //

  // Start at the core dump image header
  s_flash_addr = flash_base + sizeof(CoreDumpFlashRegionHeader);

  // Write out the core dump header -----------------------------------
  s_flash_addr += prv_write_image_header(s_flash_addr, CORE_ID_MAIN_MCU, &TINTIN_BUILD_ID,
                                         s_time_stamp);

  // Write out the memory chunks ----------------------------------------
  prv_write_memory_regions(MEMORY_REGIONS_DUMP, ARRAY_LENGTH(MEMORY_REGIONS_DUMP),
                           flash_base);

  // Write out the extra registers chunk --------------------------------------------
  CoreDumpChunkHeader chunk_hdr;
  chunk_hdr.key = CORE_DUMP_CHUNK_KEY_EXTRA_REG;
  chunk_hdr.size = sizeof(CoreDumpExtraRegInfo);
  CD_ASSERTN(s_flash_addr + chunk_hdr.size - flash_base < CORE_DUMP_MAX_SIZE);
  s_flash_addr += prv_flash_write_bytes(&chunk_hdr, s_flash_addr,
                                        sizeof(chunk_hdr));
  s_flash_addr += prv_flash_write_bytes(&s_saved_registers.extra_reg,
                                        s_flash_addr, chunk_hdr.size);

  // Write out each of the thread chunks ----------------------------------
  // Note that we leave the threads for last just in case we encounter corrupted FreeRTOS structures.
  // In that case, the core dump will at least contain the RAM and registers info and perhaps some of the
  // threads. The format of the binary core dump is streamable and is read until we reach a chunk key
  // of 0xFFFFFFFF (what gets placed into flash after an erase).
  vTaskListWalk(prvTaskInfoCallback, NULL);

  // If we core dumped from an ISR, we make up a special "ISR" thread to hold the registers
  if (!RETURNS_TO_PSP(s_saved_registers.core_reg[portCANONICAL_REG_INDEX_LR])) {
    // Another exception invoked the core dump handler
    xPORT_TASK_INFO task_info;
    task_info.pcName = "ISR";
    task_info.taskHandle = (void *)1;
    for (int i = 0; i < portCANONICAL_REG_COUNT; i++) {
      // registers [r0-r12, sp, lr, pc, sr]
      task_info.registers[i] = s_saved_registers.core_reg[i];
    }
    prvTaskInfoCallback(&task_info, NULL);
  }

  // Write out chunk terminator
  chunk_hdr.key = CORE_DUMP_CHUNK_KEY_TERMINATOR;
  chunk_hdr.size = 0;
  s_flash_addr += prv_flash_write_bytes(&chunk_hdr, s_flash_addr,
                                        sizeof(chunk_hdr));

  // Reset!
  prv_debug_str("CD: completed");
  prv_reset();
}

// -----------------------------------------------------
// Warning: these functions use the normal flash driver
status_t core_dump_size(uint32_t flash_base, uint32_t *size) {
  CoreDumpChunkHeader chunk_hdr;
  uint32_t core_dump_base = flash_base + sizeof(CoreDumpFlashRegionHeader);
  uint32_t current_offset = sizeof(CoreDumpImageHeader);

  while (true) {
    flash_read_bytes((uint8_t *)&chunk_hdr, core_dump_base + current_offset, sizeof(chunk_hdr));
    if (chunk_hdr.key == CORE_DUMP_CHUNK_KEY_TERMINATOR) {
      current_offset += sizeof(chunk_hdr);
      break;
    } else if (chunk_hdr.key == CORE_DUMP_CHUNK_KEY_RAM
           || chunk_hdr.key == CORE_DUMP_CHUNK_KEY_THREAD
           || chunk_hdr.key == CORE_DUMP_CHUNK_KEY_EXTRA_REG
           || chunk_hdr.key == CORE_DUMP_CHUNK_KEY_MEMORY) {
      current_offset += sizeof(chunk_hdr) + chunk_hdr.size;
    } else {
      return E_INTERNAL;
    }

    // Totally bogus size?
    if (current_offset > CORE_DUMP_MAX_SIZE) {
      return E_INTERNAL;
    }
  }

  *size = current_offset;
  return S_SUCCESS;
}

void core_dump_mark_read(uint32_t flash_base) {
  CoreDumpFlashRegionHeader region_hdr;
  flash_read_bytes((uint8_t *)&region_hdr, flash_base, sizeof(region_hdr));
  region_hdr.unread = 0;
  flash_write_bytes((uint8_t *)&region_hdr, flash_base, sizeof(region_hdr));
}

bool core_dump_is_unread_available(uint32_t flash_base) {
  if (flash_base != CORE_DUMP_FLASH_INVALID_ADDR) { // a coredump is on flash
    CoreDumpFlashRegionHeader region_hdr;
    CoreDumpImageHeader image_hdr;
    flash_read_bytes((uint8_t *)&region_hdr, flash_base, sizeof(region_hdr));
    flash_read_bytes((uint8_t *)&image_hdr, flash_base + sizeof(region_hdr),
                     sizeof(image_hdr));
    return ((image_hdr.magic == CORE_DUMP_MAGIC) && (region_hdr.unread != 0));
  }

  return (false);
}

uint32_t core_dump_get_slot_address(unsigned int i) {
  return (CORE_DUMP_FLASH_START + SUBSECTOR_SIZE_BYTES + i * CORE_DUMP_MAX_SIZE);
}

// BLE API - reserve core dump slot in flash
bool core_dump_reserve_ble_slot(uint32_t *flash_base, uint32_t *max_size,
                                ElfExternalNote *build_id) {
  bool status = true;
  uint32_t flash_addr, flash_addr_base;

  // Use the standard flash driver
  s_use_cd_flash_driver = false;

  flash_addr_base = prv_flash_start_address(true /*new*/);
  if (flash_addr_base == CORE_DUMP_FLASH_INVALID_ADDR) {
    status = false;
    goto cleanup;
  }

  flash_addr = flash_addr_base + sizeof(CoreDumpFlashRegionHeader);
  flash_addr += prv_write_image_header(flash_addr, CORE_ID_BLE, build_id, rtc_get_time());

  *flash_base = flash_addr;
  *max_size = CORE_DUMP_MAX_SIZE - (flash_addr - flash_addr_base);

cleanup:
  s_use_cd_flash_driver = true;
  return status;
}

// --------------------------------------------------------------------------------------------------
// Used by unit tests in to cause fw/apps/demo_apps/test_core_dump_app to encounter a bus fault during the core dump
void core_dump_test_force_bus_fault(void) {
  s_test_force_bus_fault = true;
}

void core_dump_test_force_inf_loop(void) {
  s_test_force_inf_loop = true;
}

void core_dump_test_force_assert(void) {
  s_test_force_assert = true;
}
