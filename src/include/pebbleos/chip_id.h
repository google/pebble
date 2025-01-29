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

#pragma once

/*
 * chip_id.h
 *
 * This file specifies IDs for the different processors on our multi-processor devices.
 * The IDs are used to differentiate the source of system logs, core dumps, etc.
 *
 * The IDs must be unique within a platform and must fit in 2 bits.
 * If we build a device with more than 4 log/core dump producing processors, this will need to be
 * addressed.
 */

#define CORE_ID_MAIN_MCU 0
#define CORE_ID_BLE 1
