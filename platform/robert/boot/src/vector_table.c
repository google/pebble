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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>


extern int main(void);

//! These symbols are defined in the linker script for use in initializing
//! the data sections. uint8_t since we do arithmetic with section lengths.
//! These are arrays to avoid the need for an & when dealing with linker symbols.
extern uint8_t __data_load_start[];
extern uint8_t __data_start[];
extern uint8_t __data_end[];
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];
extern uint8_t _estack[];

__attribute__((__noreturn__)) void Reset_Handler(void) {
  memcpy(__data_start, __data_load_start, __data_end - __data_start);

  // Clear the bss section, assumes .bss goes directly after .data
  memset(__bss_start, 0, __bss_end - __bss_start);

  main();

  __builtin_unreachable();
}

__attribute__((__noreturn__)) void Default_Handler(void) {
  // This handler is only called if we haven't defined a specific
  // handler for the interrupt. This means the interrupt is unexpected,
  // so we loop infinitely to preserve the system state for examination
  // by a debugger
  while (true) {}
}


// All these functions are weak references to the Default_Handler,
// so if we define a handler in elsewhere in the firmware, these
// will be overridden
#define ALIAS(sym) __attribute__((__weak__, __alias__(sym)))
ALIAS("Default_Handler") void NMI_Handler(void);
ALIAS("Default_Handler") void HardFault_Handler(void);
ALIAS("Default_Handler") void MemManage_Handler(void);
ALIAS("Default_Handler") void BusFault_Handler(void);
ALIAS("Default_Handler") void UsageFault_Handler(void);
ALIAS("Default_Handler") void SVC_Handler(void);
ALIAS("Default_Handler") void DebugMon_Handler(void);
ALIAS("Default_Handler") void PendSV_Handler(void);
ALIAS("Default_Handler") void SysTick_Handler(void);

// External Interrupts
#define IRQ_DEF(idx, irq) ALIAS("Default_Handler") void irq##_IRQHandler(void);
#include "irq_stm32f7.def"
#undef IRQ_DEF


__attribute__((__section__(".isr_vector"))) const void * const vector_table[] = {
  _estack,
  Reset_Handler,
  NMI_Handler,
  HardFault_Handler,
  MemManage_Handler,
  BusFault_Handler,
  UsageFault_Handler,
  0,
  0,
  0,
  0,
  SVC_Handler,
  DebugMon_Handler,
  0,
  PendSV_Handler,
  SysTick_Handler,

  // External Interrupts
#define IRQ_DEF(idx, irq) [idx + 16] = irq##_IRQHandler,
#include "irq_stm32f7.def"
#undef IRQ_DEF
};
