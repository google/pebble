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

//! @file gap_le_slave_discovery.h
//! This sub-module is responsible for advertising explicitly for device
//! discovery purposes. The advertisement will contain the device name,
//! transmit power level (to be able to order devices by estimated proximity),
//! Pebble Service UUID and discoverability flags.
//! Advertising devices will implicitly become the slave when being connected
//! to, so the "slave" part in the file name is redundant, but kept for
//! the sake of completeness.

#include <stdbool.h>
#include <stdint.h>

//! @return True is Pebble is currently explicitly discoverable as BLE slave
//! or false if not.
bool gap_le_slave_is_discoverable(void);

//! @param discoverable True to make Pebble currently explicitly discoverable
//! as BLE slave. Initially, Pebble will advertise at a relatively high rate for
//! a few seconds. After this, the rate will drop to save battery life.
void gap_le_slave_set_discoverable(bool discoverable);

//! Initializes the gap_le_slave_discovery module.
void gap_le_slave_discovery_init(void);

//! De-Initializes the gap_le_slave_discovery module.
void gap_le_slave_discovery_deinit(void);
