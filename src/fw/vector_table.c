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

#include "util/attributes.h"

#include <stdbool.h>
#include <stdint.h>

extern uint8_t _estack[];
extern void Reset_Handler(void);

void Default_Handler(void) {
  // This handler is only called if we haven't defined a specific
  // handler for the interrupt. This means the interrupt is unexpected,
  // so we loop infinitely to preserve the system state for examination
  // by a debugger
  while (true) {}
}

// All these functions are weak references to the Default_Handler,
// so if we define a handler in elsewhere in the firmware, these
// will be overridden
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
#include "irq_stm32.def"
#undef IRQ_DEF


#if PROFILE_INTERRUPTS
#include "system/profiler.h"
#define IRQ_DEF(idx, irq) static inline void irq##_IRQHandler_profiled(void) { \
  extern ProfilerNode g_profiler_node_##irq##_IRQ; \
  g_profiler_node_##irq##_IRQ.start = DWT->CYCCNT; \
  irq##_IRQHandler();\
  profiler_node_stop(&g_profiler_node_##irq##_IRQ, DWT->CYCCNT); \
}
#include "irq_stm32.def"
#undef IRQ_DEF
#endif // PROFILE_INTERRUPTS


EXTERNALLY_VISIBLE SECTION(".isr_vector") const void * const vector_table[] = {
  _estack,
  Reset_Handler,
  NMI_Handler,
  HardFault_Handler,
  MemManage_Handler,
  BusFault_Handler,
  UsageFault_Handler,
  (void *)0x4E65576F, // NeWo, marks image as New World for bootloader
  0,
  0,
  0,
  SVC_Handler,
  DebugMon_Handler,
  0,
  PendSV_Handler,
  SysTick_Handler,

  // External Interrupts
#if PROFILE_INTERRUPTS
#define IRQ_DEF(idx, irq) [idx + 16] = irq##_IRQHandler_profiled,
#else
#define IRQ_DEF(idx, irq) [idx + 16] = irq##_IRQHandler,
#endif
#include "irq_stm32.def"
#undef IRQ_DEF
};
