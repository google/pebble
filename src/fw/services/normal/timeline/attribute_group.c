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

#include "attribute_group.h"

#include "services/normal/contacts/attributes_address.h"
#include "system/passert.h"
#include "util/attributes.h"

typedef struct PACKED {
  uint8_t id;
  uint8_t type;
  uint8_t num_attributes;
} SerializedActionHeader;

typedef struct PACKED {
  Uuid uuid;
  uint8_t type;
  uint8_t num_attributes;
} SerializedAddressHeader;


// -----------------------------------------------------------------------------
static bool prv_parse_serial_group_type_data(AttributeGroupType type,
                                             const uint8_t *data,
                                             const uint8_t *end,
                                             const uint8_t num_group_type_elements,
                                             size_t *string_alloc_size_out,
                                             uint8_t *attributes_per_group_type_element_out) {
  for (int i = 0; i < num_group_type_elements; i++) {
    int num_attributes;
    int header_size;
    if (type == AttributeGroupType_Action) {
      header_size = sizeof(SerializedActionHeader);
      num_attributes = ((SerializedActionHeader *)data)->num_attributes;
    } else {
      header_size = sizeof(SerializedAddressHeader);
      num_attributes = ((SerializedAddressHeader *)data)->num_attributes;
    }

    data += header_size;
    attributes_per_group_type_element_out[i] = num_attributes;
    const int result = attribute_get_buffer_size_for_serialized_attributes(
        attributes_per_group_type_element_out[i], &data, end);
    if (result < 0) {
      return false;
    }
    *string_alloc_size_out += result;
  }

  return true;
}


bool attribute_group_parse_serial_data(AttributeGroupType type,
                                       uint8_t num_attributes,
                                       uint8_t num_group_type_elements,
                                       const uint8_t *data,
                                       size_t size,
                                       size_t *string_alloc_size_out,
                                       uint8_t *attributes_per_group_type_element_out) {

  PBL_ASSERTN(data != NULL && string_alloc_size_out != NULL);

  *string_alloc_size_out = 0;

  const uint8_t *end = data + size;
  const uint8_t *cursor = data;
  int32_t result = attribute_get_buffer_size_for_serialized_attributes(num_attributes,
                                                                       &cursor,
                                                                       end);
  if (result < 0) {
    return false;
  }
  *string_alloc_size_out += result;

  return prv_parse_serial_group_type_data(type,
                                          cursor,
                                          end,
                                          num_group_type_elements,
                                          string_alloc_size_out,
                                          attributes_per_group_type_element_out);
}

// -----------------------------------------------------------------------------
static int prv_get_action_group_buffer_size(AttributeGroupType type,
                                            uint8_t num_group_type_elements,
                                            const uint8_t *attributes_per_group_type_element) {
  int size;

  if (type == AttributeGroupType_Action) {
    size = num_group_type_elements * sizeof(TimelineItemAction);
  } else {
    size = num_group_type_elements * sizeof(Address);
  }

  for (int i = 0; i < num_group_type_elements; i++) {
    size += attributes_per_group_type_element[i] * sizeof(Attribute);
  }

  return size;
}

size_t attribute_group_get_required_buffer_size(AttributeGroupType type,
                                                uint8_t num_attributes,
                                                uint8_t num_group_type_elements,
                                                const uint8_t *attributes_per_group_type_element,
                                                size_t required_size_for_strings) {
  // this reflects the physical layout of the memory
  //  1. list of all attributes
  //  2. list of all group elements (actions or addresses)
  //  3. all lists of all the group elements' attributes
  //  4. additional space for heap allocated strings
  size_t buffer_size = num_attributes * sizeof(Attribute);

  buffer_size += prv_get_action_group_buffer_size(type,
                                                  num_group_type_elements,
                                                  attributes_per_group_type_element);

  buffer_size += required_size_for_strings;

  return buffer_size;
}

// -----------------------------------------------------------------------------
static void prv_init_attribute_list(AttributeList *attr_list,
                                    uint8_t **buffer,
                                    const uint8_t attributes_per_group_type_element) {
  attr_list->num_attributes = attributes_per_group_type_element;
  attr_list->attributes = (Attribute *)*buffer;
  *buffer += attributes_per_group_type_element * sizeof(Attribute);
}

static void prv_init_group_type(AttributeGroupType type,
                                void *group_ptr,
                                uint8_t **buffer,
                                uint8_t num_group_type_elements,
                                const uint8_t *attributes_per_group_type_element) {
  if (num_group_type_elements > 0) {
    if (type == AttributeGroupType_Action) {
      ((TimelineItemActionGroup *) group_ptr)->num_actions = num_group_type_elements;
      ((TimelineItemActionGroup *) group_ptr)->actions = (TimelineItemAction *)*buffer;
      *buffer += num_group_type_elements * sizeof(TimelineItemAction);
    } else {
      ((AddressList *) group_ptr)->num_addresses = num_group_type_elements;
      ((AddressList *) group_ptr)->addresses = (Address *)*buffer;
      *buffer += num_group_type_elements * sizeof(Address);
    }

    for (int i = 0; i < num_group_type_elements; i++) {
      AttributeList *attr_list;
      if (type == AttributeGroupType_Action) {
        attr_list = &((TimelineItemActionGroup *) group_ptr)->actions[i].attr_list;
      } else {
        attr_list = &((AddressList *) group_ptr)->addresses[i].attr_list;
      }
      prv_init_attribute_list(attr_list, buffer, attributes_per_group_type_element[i]);
    }
  }
}

void attribute_group_init(AttributeGroupType type,
                          AttributeList *attr_list,
                          void *group_ptr,
                          uint8_t **buffer,
                          uint8_t num_attributes,
                          uint8_t num_group_type_elements,
                          const uint8_t *attributes_per_group_type_element) {
  attr_list->num_attributes = num_attributes;
  attr_list->attributes = (Attribute *)*buffer;
  *buffer += num_attributes * sizeof(Attribute);

  prv_init_group_type(type,
                      group_ptr,
                      buffer,
                      num_group_type_elements,
                      attributes_per_group_type_element);
}

// -----------------------------------------------------------------------------
static AttributeList *prv_deserialize_action(TimelineItemAction *action,
                                             const uint8_t **cursor,
                                             const uint8_t *payload_end,
                                             const uint8_t *buffer,
                                             const uint8_t *buf_end) {
  SerializedActionHeader *serialized_action = (SerializedActionHeader *)*cursor;
  *cursor += sizeof(SerializedActionHeader);
  action->id = serialized_action->id;
  action->type = serialized_action->type;
  action->attr_list.num_attributes = serialized_action->num_attributes;

  return &action->attr_list;
}

static AttributeList *prv_deserialize_address(Address *address,
                                              const uint8_t **cursor,
                                              const uint8_t *payload_end,
                                              const uint8_t *buffer,
                                              const uint8_t *buf_end) {
  SerializedAddressHeader *serialized_address = (SerializedAddressHeader *)*cursor;
  *cursor += sizeof(SerializedAddressHeader);
  address->id = serialized_address->uuid;
  address->type = serialized_address->type;
  address->attr_list.num_attributes = serialized_address->num_attributes;

  return &address->attr_list;
}

static bool prv_deserialize_group_element(AttributeGroupType type,
                                          void *group_ptr,
                                          const uint8_t *payload,
                                          const uint8_t *payload_end,
                                          const uint8_t *buffer,
                                          const uint8_t *buf_end) {
  const uint8_t *cursor = payload;

  int num_group_type_elements;
  if (type == AttributeGroupType_Action) {
    num_group_type_elements = ((TimelineItemActionGroup *)group_ptr)->num_actions;
  } else {
    num_group_type_elements = ((AddressList *)group_ptr)->num_addresses;
  }

  for (int i = 0; i < num_group_type_elements; i++) {
    AttributeList *group_type_element_attribute_list;
    if (type == AttributeGroupType_Action) {
      group_type_element_attribute_list = prv_deserialize_action(
          &((TimelineItemActionGroup *)group_ptr)->actions[i], &cursor, payload_end,
          buffer, buf_end);
    } else {
      group_type_element_attribute_list = prv_deserialize_address(
          &((AddressList *)group_ptr)->addresses[i], &cursor, payload_end,
          buffer, buf_end);
    }

    if (!attribute_deserialize_list((char**)&buffer, (char *)buf_end, &cursor,
                                    payload_end, *group_type_element_attribute_list)) {
      return false;
    }
  }
  return true;
}

bool attribute_group_deserialize(AttributeGroupType type,
                                 AttributeList *attr_list,
                                 void *group_ptr,
                                 uint8_t *buffer,
                                 uint8_t *buf_end,
                                 const uint8_t *payload,
                                 size_t payload_size) {

  PBL_ASSERTN(payload != NULL);

  const uint8_t *payload_end = payload + payload_size;
  const uint8_t *cursor = payload;

  if (!attribute_deserialize_list((char**)&buffer, (char*)buf_end,
                                  &cursor, payload_end, *attr_list)) {
    return false;
  }

  return prv_deserialize_group_element(type,
                                       group_ptr,
                                       cursor,
                                       payload_end,
                                       buffer,
                                       buf_end);
}

// -----------------------------------------------------------------------------
static int prv_get_serialized_action_group_size(TimelineItemActionGroup *action_group) {
  int size = action_group->num_actions * sizeof(SerializedActionHeader);
  for (int i = 0; i < action_group->num_actions; i++) {
    size += attribute_list_get_serialized_size(&action_group->actions[i].attr_list);
  }

  return size;
}

static int prv_get_serialized_address_list_size(AddressList *addr_list) {
  int size = (addr_list->num_addresses * sizeof(SerializedAddressHeader));
  for (int i = 0; i < addr_list->num_addresses; i++) {
    size += attribute_list_get_serialized_size((AttributeList *)&addr_list->addresses[i].attr_list);
  }

  return size;
}

size_t attribute_group_get_serialized_payload_size(AttributeGroupType type,
                                                   AttributeList *attr_list,
                                                   void *group_ptr) {
  size_t size = 0;
  if (attr_list) {
    size += attribute_list_get_serialized_size(attr_list);
  }

  if (group_ptr) {
    if (type == AttributeGroupType_Action) {
      size += prv_get_serialized_action_group_size((TimelineItemActionGroup *)group_ptr);
    } else {
      size += prv_get_serialized_address_list_size((AddressList *)group_ptr);
    }
  }
  return size;
}

// -----------------------------------------------------------------------------
static AttributeList *prv_serialize_action(TimelineItemAction *action,
                                           uint8_t **buffer) {
  SerializedActionHeader *serialized_action = (SerializedActionHeader *)*buffer;
  *buffer += sizeof(SerializedActionHeader);

  serialized_action->id = action->id;
  serialized_action->type = action->type;
  serialized_action->num_attributes = action->attr_list.num_attributes;

  return &action->attr_list;
}

static AttributeList *prv_serialize_address(Address *address,
                                            uint8_t **buffer) {
  SerializedAddressHeader *serialized_address = (SerializedAddressHeader *)*buffer;
  *buffer += sizeof(SerializedAddressHeader);

  serialized_address->uuid = address->id;
  serialized_address->type = address->type;
  serialized_address->num_attributes = address->attr_list.num_attributes;

  return &address->attr_list;
}

static uint8_t* prv_serialize_group_element(AttributeGroupType type,
                                            void *group_ptr,
                                            uint8_t *buffer,
                                            uint8_t *buf_end) {
  int num_group_type_elements;
  if (type == AttributeGroupType_Action) {
    num_group_type_elements = ((TimelineItemActionGroup *)group_ptr)->num_actions;
  } else {
    num_group_type_elements = ((AddressList *)group_ptr)->num_addresses;
  }

  for (int i = 0; i < num_group_type_elements; i++) {
    AttributeList *group_type_element_attribute_list;
    if (type == AttributeGroupType_Action) {
      group_type_element_attribute_list = prv_serialize_action(
          &((TimelineItemActionGroup *)group_ptr)->actions[i], &buffer);
    } else {
      group_type_element_attribute_list = prv_serialize_address(
          &((AddressList *)group_ptr)->addresses[i], &buffer);
    }
    PBL_ASSERTN(buffer <= buf_end);

    buffer += attribute_list_serialize(group_type_element_attribute_list,
                                       buffer, buf_end);
  }

  return buffer;
}

size_t attribute_group_serialize_payload(AttributeGroupType type,
                                         AttributeList *attr_list,
                                         void *group_ptr,
                                         uint8_t *buffer,
                                         size_t buffer_size) {
  PBL_ASSERTN(buffer != NULL);

  uint8_t *buf_start = buffer;
  uint8_t *buf_end = buffer + buffer_size;
  if (attr_list) {
    buffer += attribute_list_serialize(attr_list, buffer, buf_end);
  }

  if (group_ptr) {
    buffer = prv_serialize_group_element(type, group_ptr, buffer, buf_end);
  }

  return buffer - buf_start;
}
