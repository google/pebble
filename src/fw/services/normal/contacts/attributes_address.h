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

#include "services/normal/timeline/item.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

typedef enum {
  AddressTypeInvalid,
  AddressTypePhoneNumber,
  AddressTypeEmail,
} AddressType;

typedef struct {
  Uuid id;
  AddressType type;
  AttributeList attr_list;
} Address;

typedef struct {
  uint8_t num_addresses;
  Address *addresses;
} AddressList;


//! Takes serialized data and fills two arrays: string_alloc_size_out and attributes_per_action_out
//! The information in these arrays is used in the following steps
//! @param num_attributes     number of non-address attributes
//! @param num_addresses      number of addresses
//! @param data               serialized data buffer
//! @param data_size          size of the serial data buffer
//! @param string_alloc_size_out         size of string buffer that is required
//! @param attributes_per_address_out    an array of counts for the number of attributes per address
//!                                      in order corresponding to address order
//! @return True if the data was parsed successfully, False if not
bool attributes_address_parse_serial_data(uint8_t num_attributes,
                                          uint8_t num_addresses,
                                          const uint8_t *data,
                                          size_t data_size,
                                          size_t *string_alloc_size_out,
                                          uint8_t *attributes_per_address_out);

//! Return the size of the buffer needed to store the attributes, addresses and their strings
//! @param num_attributes   number of non-address attributes
//! @param num_addresses    number of addresses
//! @param attributes_per_address    an array of counts for the number of attributes per address
//!                                  in order corresponding to address order
//! @param required_size_for_strings    total size of all attribute strings
//! @return The size of the buffer required to store the attributes, address and strings
size_t attributes_address_get_buffer_size(uint8_t num_attributes,
                                          uint8_t num_addresses,
                                          const uint8_t *attributes_per_address,
                                          size_t required_size_for_strings);

//! Initializes an AttributeList and AddressList
//! @param attr_list          The AttributeList to initialize
//! @param addr_list          The AddressList to initialize
//! @param buffer             The buffer to hold the list of attributes and address
//! @param num_attributes     number of attributes
//! @param num_addresses      number of addresses
//! @param attributes_per_address   an array of counts for the number of attributes per address
//!                                 in order corresponding to address order
void attributes_address_init(AttributeList *attr_list,
                             AddressList *addr_list,
                             uint8_t **buffer,
                             uint8_t num_attributes,
                             uint8_t num_addresses,
                             const uint8_t *attributes_per_address);

//! Fills an AttributeList and AddressList from serialized data
//! @param attr_list          The AttributeList to fill
//! @param addr_list          The AddressList to fill
//! @param buffer             The buffer which holds the list of attributes and address
//! @param buf_end            A pointer to the end of the buffer
//! @param payload            Serialized payload buffer
//! @param payload_size       Size of the payload buffer in bytes
bool attributes_address_deserialize(AttributeList *attr_list,
                                    AddressList *addr_list,
                                    uint8_t *buffer,
                                    uint8_t *buf_end,
                                    const uint8_t *payload,
                                    size_t payload_size);

//! Calculate the required size for a buffer to store address & attributes
size_t attributes_address_get_serialized_payload_size(AttributeList *list,
                                                      AddressList *addr_list);

//! Serializes an attribute list and address list into a buffer
//! @param attr_list          The AttributeList to serialize
//! @param addr_list          The AddressList to serialize
//! @param buffer a pointer to the buffer to write to
//! @param buffer_size the size of the buffer in bytes
//! @returns the number of bytes written to buffer
size_t attributes_address_serialize_payload(AttributeList *attr_list,
                                            AddressList *addr_list,
                                            uint8_t *buffer,
                                            size_t buffer_size);
