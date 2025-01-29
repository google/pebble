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

#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

//! @file dict.h Generic key/value serializer and parser.

//! @addtogroup Foundation
//! @{
//!   @addtogroup Dictionary
//! \brief Data serialization utilities
//!
//!
//! Data residing in different parts of Pebble memory (RAM) may need to be gathered and assembled into
//! a single continuous block for transport over the network via Bluetooth. The process of gathering
//! and assembling this continuous block of data is called serialization.
//!
//! You use data serialization utilities, like Dictionary, Tuple and Tuplet data structures and accompanying
//! functions, to accomplish this task. No transformations are performed on the actual data, however.
//! These Pebble utilities simply help assemble the data into one continuous buffer according to a
//! specific format.
//!
//! \ref AppMessage uses these utilities--in particular, Dictionary--to send information between mobile
//! and Pebble watchapps.
//!
//! <h3>Writing key/value pairs</h3>
//! To write two key/value pairs, without using Tuplets, you would do this:
//! \code{.c}
//! // Byte array + key:
//! static const uint32_t SOME_DATA_KEY = 0xb00bf00b;
//! static const uint8_t data[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
//!
//! // CString + key:
//! static const uint32_t SOME_STRING_KEY = 0xabbababe;
//! static const char *string = "Hello World";
//!
//! // Calculate the buffer size that is needed for the final Dictionary:
//! const uint8_t key_count = 2;
//! const uint32_t size = dict_calc_buffer_size(key_count, sizeof(data),
//!                                             strlen(string) + 1);
//!
//! // Stack-allocated buffer in which to create the Dictionary:
//! uint8_t buffer[size];
//!
//! // Iterator variable, keeps the state of the creation serialization process:
//! DictionaryIterator iter;
//!
//! // Begin:
//! dict_write_begin(&iter, buffer, sizeof(buffer));
//! // Write the Data:
//! dict_write_data(&iter, SOME_DATA_KEY, data, sizeof(data));
//! // Write the CString:
//! dict_write_cstring(&iter, SOME_STRING_KEY, string);
//! // End:
//! const uint32_t final_size = dict_write_end(&iter);
//!
//! // buffer now contains the serialized information
//!
//! \endcode
//!
//! <h3>Reading key/value pairs</h3>
//! To iterate over the key/value pairs in the dictionary that
//! was created in the previous example code, you would do this:
//!
//! \code{.c}
//! Tuple *tuple = dict_read_begin_from_buffer(&iter, buffer, final_size);
//! while (tuple) {
//!   switch (tuple->key) {
//!     case SOME_DATA_KEY:
//!       foo(tuple->value->data, tuple->length);
//!       break;
//!     case SOME_STRING_KEY:
//!       bar(tuple->value->cstring);
//!       break;
//!   }
//!   tuple = dict_read_next(&iter);
//! }
//! \endcode
//!
//! <h3>Tuple and Tuplet data structures</h3>
//! To understand the difference between Tuple and Tuplet data structures:
//! Tuple is the header for a serialized key/value pair, while Tuplet is a helper
//! data structure that references the value you want to serialize. This data
//! structure exists to make the creation of a Dictionary easier to write.
//! Use this mnemonic to remember the difference: TupleT(emplate), the Tuplet being
//! a template to create a Dictionary with Tuple structures.
//!
//! For example:
//! \code{.c}
//! Tuplet pairs[] = {
//!   TupletInteger(WEATHER_ICON_KEY, (uint8_t) 1),
//!   TupletCString(WEATHER_TEMPERATURE_KEY, "1234 Fahrenheit"),
//! };
//! uint8_t buffer[256];
//! uint32_t size = sizeof(buffer);
//! dict_serialize_tuplets_to_buffer(pairs, ARRAY_LENGTH(pairs), buffer, &size);
//!
//! // buffer now contains the serialized information
//! \endcode
//!   @{

//! Return values for dictionary write/conversion functions.
typedef enum {
  //! The operation returned successfully
  DICT_OK = 0,
  //! There was not enough backing storage to complete the operation
  DICT_NOT_ENOUGH_STORAGE = 1 << 1,
  //! One or more arguments were invalid or uninitialized
  DICT_INVALID_ARGS = 1 << 2,
  //! The lengths and/or count of the dictionary its tuples are inconsistent
  DICT_INTERNAL_INCONSISTENCY = 1 << 3,
  //! A requested operation required additional memory to be allocated, but
  //! the allocation failed, likely due to insufficient remaining heap memory.
  DICT_MALLOC_FAILED = 1 << 4,
} DictionaryResult;

//! Values representing the type of data that the `value` field of a Tuple contains
typedef enum {
  //! The value is an array of bytes
  TUPLE_BYTE_ARRAY = 0,
  //! The value is a zero-terminated, UTF-8 C-string
  TUPLE_CSTRING = 1,
  //! The value is an unsigned integer. The tuple's `.length` field is used to
  //! determine the size of the integer (1, 2, or 4 bytes).
  TUPLE_UINT = 2,
  //! The value is a signed integer. The tuple's `.length` field is used to
  //! determine the size of the integer (1, 2, or 4 bytes).
  TUPLE_INT = 3,
} TupleType;

//! Data structure for one serialized key/value tuple
//! @note The structure is variable length! The length depends on the value data that the tuple
//! contains.
typedef struct __attribute__((__packed__)) {
  //! The key
  uint32_t key;
  //! The type of data that the `.value` fields contains.
  TupleType type:8;
  //! The length of `.value` in bytes
  uint16_t length;
  //! @brief The value itself.
  //!
  //! The different union fields are provided for convenience, avoiding the need for manual casts.
  //! @note The array length is of incomplete length on purpose, to facilitate
  //! variable length data and because a data length of zero is valid.
  //! @note __Important: The integers are little endian!__
  union {
    //! The byte array value. Valid when `.type` is \ref TUPLE_BYTE_ARRAY.
    uint8_t data[0];
    //! The C-string value. Valid when `.type` is \ref TUPLE_CSTRING.
    char cstring[0];
    //! The 8-bit unsigned integer value. Valid when `.type` is \ref TUPLE_UINT
    //! and `.length` is 1 byte.
    uint8_t uint8;
    //! The 16-bit unsigned integer value. Valid when `.type` is \ref TUPLE_UINT
    //! and `.length` is 2 byte.
    uint16_t uint16;
    //! The 32-bit unsigned integer value. Valid when `.type` is \ref TUPLE_UINT
    //! and `.length` is 4 byte.
    uint32_t uint32;
    //! The 8-bit signed integer value. Valid when `.type` is \ref TUPLE_INT
    //! and `.length` is 1 byte.
    int8_t int8;
    //! The 16-bit signed integer value. Valid when `.type` is \ref TUPLE_INT
    //! and `.length` is 2 byte.
    int16_t int16;
    //! The 32-bit signed integer value. Valid when `.type` is \ref TUPLE_INT
    //! and `.length` is 4 byte.
    int32_t int32;
  } value[];
} Tuple;

//! @internal
//! Header data structure of a serialized "dictionary" of zero or more Tuple
//! key-value pairs.
typedef struct __attribute__((__packed__)) {
  uint8_t count;  //!< The number of key-value pairs (Tuples) in the dictionary
  Tuple head[];   //!< The first Tuple in the dictionary
} Dictionary;

//! An iterator can be used to iterate over the key/value
//! tuples in an existing dictionary, using \ref dict_read_begin_from_buffer(),
//! \ref dict_read_first() and \ref dict_read_next().
//! An iterator can also be used to append key/value tuples to a dictionary,
//! for example using \ref dict_write_data() or \ref dict_write_cstring().
typedef struct {
  Dictionary *dictionary;  //!< The dictionary being iterated
  const void *end;  //!< Points to the first memory address after the last byte of the dictionary
  //! Points to the next Tuple in the dictionary. Given the end of the
  //! Dictionary has not yet been reached: when writing, the next key/value
  //! pair will be written at the cursor. When reading, the next call
  //! to \ref dict_read_next() will return the cursor.
  Tuple *cursor;
} DictionaryIterator;


//! Calculates the number of bytes that a dictionary will occupy, given
//! one or more value lengths that need to be stored in the dictionary.
//! @note The formula to calculate the size of a Dictionary in bytes is:
//! <pre>1 + (n * 7) + D1 + ... + Dn</pre>
//! Where `n` is the number of Tuples in the Dictionary and `Dx` are the sizes
//! of the values in the Tuples. The size of the Dictionary header is 1 byte.
//! The size of the header for each Tuple is 7 bytes.
//! @param tuple_count The total number of key/value pairs in the dictionary.
//! @param ... The sizes of each of the values that need to be
//! stored in the dictionary.
//! @return The total number of bytes of storage needed.
uint32_t dict_calc_buffer_size(const uint8_t tuple_count, ...);

//! Calculates the size of data that has been written to the dictionary.
//! AKA, the "dictionary size". Note that this is most likely different
//! than the size of the backing storage/backing buffer.
//! @param iter The dictionary iterator
//! @return The total number of bytes which have been written to the dictionary.
uint32_t dict_size(DictionaryIterator* iter);

//! Initializes the dictionary iterator with a given buffer and size,
//! resets and empties it, in preparation of writing key/value tuples.
//! @param iter The dictionary iterator
//! @param buffer The storage of the dictionary
//! @param size The storage size of the dictionary
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
//! @see dict_calc_buffer_size
//! @see dict_write_end
DictionaryResult dict_write_begin(DictionaryIterator *iter, uint8_t * const buffer, const uint16_t size);

//! Adds a key with a byte array value pair to the dictionary.
//! @param iter The dictionary iterator
//! @param key The key
//! @param data Pointer to the byte array
//! @param size Length of the byte array
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
//! @note The data will be copied into the backing storage of the dictionary.
//! @note There is _no_ checking for duplicate keys.
DictionaryResult dict_write_data(DictionaryIterator *iter, const uint32_t key, const uint8_t * const data, const uint16_t size);

//! Adds a key with a C string value pair to the dictionary.
//! @param iter The dictionary iterator
//! @param key The key
//! @param cstring Pointer to the zero-terminated C string
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
//! @note The string will be copied into the backing storage of the dictionary.
//! @note There is _no_ checking for duplicate keys.
DictionaryResult dict_write_cstring(DictionaryIterator *iter, const uint32_t key, const char * const cstring);

//! Adds a key with an integer value pair to the dictionary.
//! @param iter The dictionary iterator
//! @param key The key
//! @param integer Pointer to the integer value
//! @param width_bytes The width of the integer value
//! @param is_signed Whether the integer's type is signed or not
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
//! @note There is _no_ checking for duplicate keys. dict_write_int() is only for serializing a single
//! integer. width_bytes can only be 1, 2, or 4.
DictionaryResult dict_write_int(DictionaryIterator *iter, const uint32_t key, const void *integer, const uint8_t width_bytes, const bool is_signed);

//! Adds a key with an unsigned, 8-bit integer value pair to the dictionary.
//! @param iter The dictionary iterator
//! @param key The key
//! @param value The unsigned, 8-bit integer value
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
//! @note There is _no_ checking for duplicate keys.
//! @note There are counterpart functions for different signedness and widths,
//! `dict_write_uint16()`, `dict_write_uint32()`, `dict_write_int8()`,
//! `dict_write_int16()` and `dict_write_int32()`. The documentation is not
//! repeated for brevity's sake.
DictionaryResult dict_write_uint8(DictionaryIterator *iter, const uint32_t key, const uint8_t value);
DictionaryResult dict_write_uint16(DictionaryIterator *iter, const uint32_t key, const uint16_t value);
DictionaryResult dict_write_uint32(DictionaryIterator *iter, const uint32_t key, const uint32_t value);
DictionaryResult dict_write_int8(DictionaryIterator *iter, const uint32_t key, const int8_t value);
DictionaryResult dict_write_int16(DictionaryIterator *iter, const uint32_t key, const int16_t value);
DictionaryResult dict_write_int32(DictionaryIterator *iter, const uint32_t key, const int32_t value);

//! End a series of writing operations to a dictionary.
//! This must be called before reading back from the dictionary.
//! @param iter The dictionary iterator
//! @return The size in bytes of the finalized dictionary, or 0 if the parameters were invalid.
uint32_t dict_write_end(DictionaryIterator *iter);

//! Initializes the dictionary iterator with a given buffer and size,
//! in preparation of reading key/value tuples.
//! @param iter The dictionary iterator
//! @param buffer The storage of the dictionary
//! @param size The storage size of the dictionary
//! @return The first tuple in the dictionary, or NULL in case the dictionary was empty or if there was a parsing error.
Tuple * dict_read_begin_from_buffer(DictionaryIterator *iter, const uint8_t * const buffer, const uint16_t size);

//! Progresses the iterator to the next key/value pair.
//! @param iter The dictionary iterator
//! @return The next tuple in the dictionary, or NULL in case the end has been reached or if there was a parsing error.
Tuple * dict_read_next(DictionaryIterator *iter);

//! Resets the iterator back to the same state as a call to \ref dict_read_begin_from_buffer() would do.
//! @param iter The dictionary iterator
//! @return The first tuple in the dictionary, or NULL in case the dictionary was empty or if there was a parsing error.
Tuple * dict_read_first(DictionaryIterator *iter);

/** Dictionary Utilities */

//! Non-serialized, template data structure for a key/value pair.
//! For strings and byte arrays, it only has a pointer to the actual data.
//! For integers, it provides storage for integers up to 32-bits wide.
//! The Tuplet data structure is useful when creating dictionaries from values
//! that are already stored in arbitrary buffers.
//! See also \ref Tuple, with is the header of a serialized key/value pair.
typedef struct Tuplet {
  //! The type of the Tuplet. This determines which of the struct fields in the
  //! anonymous union are valid.
  TupleType type;
  //! The key.
  uint32_t key;
  //! Anonymous union containing the reference to the Tuplet's value, being
  //! either a byte array, c-string or integer. See documentation of `.bytes`,
  //! `.cstring` and `.integer` fields.
  union {
    //! Valid when `.type.` is \ref TUPLE_BYTE_ARRAY
    struct {
      //! Pointer to the data
      const uint8_t *data;
      //! Length of the data
      const uint16_t length;
    } bytes;
    //! Valid when `.type.` is \ref TUPLE_CSTRING
    struct {
      //! Pointer to the c-string data
      const char *data;
      //! Length of the c-string, including terminating zero.
      const uint16_t length;
    } cstring;
    //! Valid when `.type.` is \ref TUPLE_INT or \ref TUPLE_UINT
    struct {
      //! Actual storage of the integer.
      //! The signedness can be derived from the `.type` value.
      uint32_t storage;
      //! Width of the integer.
      const uint16_t width;
    } integer;
  }; //!< See documentation of `.bytes`, `.cstring` and `.integer` fields.
} Tuplet;

//! Macro to create a Tuplet with a byte array value
//! @param _key The key
//! @param _data Pointer to the bytes
//! @param _length Length of the buffer
#define TupletBytes(_key, _data, _length) \
((const Tuplet) { .type = TUPLE_BYTE_ARRAY, .key = _key, .bytes = { .data = _data, .length = _length }})

//! Macro to create a Tuplet with a c-string value
//! @param _key The key
//! @param _cstring The c-string value
#define TupletCString(_key, _cstring) \
((const Tuplet) { .type = TUPLE_CSTRING, .key = _key, .cstring = { .data = _cstring, .length = _cstring ? strlen(_cstring) + 1 : 0 }})

//! Macro to create a Tuplet with an integer value
//! @param _key The key
//! @param _integer The integer value
#define TupletInteger(_key, _integer) \
((const Tuplet) { .type = IS_SIGNED(_integer) ? TUPLE_INT : TUPLE_UINT, .key = _key, .integer = { .storage = _integer, .width = sizeof(_integer) }})

//! Callback for \ref dict_serialize_tuplets() utility.
//! @param data The data of the serialized dictionary
//! @param size The size of data
//! @param context The context pointer as passed in to \ref dict_serialize_tuplets()
//! @see dict_serialize_tuplets
typedef void (*DictionarySerializeCallback)(const uint8_t * const data, const uint16_t size, void *context);

//! Utility function that takes a list of Tuplets from which a dictionary
//! will be serialized, ready to transmit or store.
//! @note The callback will be called before the function returns, so the data that
//! that `context` points to, can be stack allocated.
//! @param callback The callback that will be called with the serialized data of the generated dictionary.
//! @param context Pointer to any application specific data that gets passed into the callback.
//! @param tuplets An array of Tuplets that need to be serialized into the dictionary.
//! @param tuplets_count The number of tuplets that follow.
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
DictionaryResult dict_serialize_tuplets(DictionarySerializeCallback callback, void *context, const Tuplet * const tuplets, const uint8_t tuplets_count);

// Legacy version to prevent previous app breakage, __deprecated preserves order
DictionaryResult dict_serialize_tuplets__deprecated(DictionarySerializeCallback callback, void *context, const uint8_t tuplets_count, const Tuplet * const tuplets);

//! Utility function that takes an array of Tuplets and serializes them into
//! a dictionary with a given buffer and size.
//! @param tuplets The array of tuplets
//! @param tuplets_count The number of tuplets in the array
//! @param buffer The buffer in which to write the serialized dictionary
//! @param [in] size_in_out The available buffer size in bytes
//! @param [out] size_in_out The number of bytes written
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
DictionaryResult dict_serialize_tuplets_to_buffer(const Tuplet * const tuplets, const uint8_t tuplets_count, uint8_t *buffer, uint32_t *size_in_out);

// Legacy version to prevent previous app breakage, __deprecated preserves order
DictionaryResult dict_serialize_tuplets_to_buffer__deprecated(const uint8_t tuplets_count, const Tuplet * const tuplets, uint8_t *buffer, uint32_t *size_in_out);

//! Serializes an array of Tuplets into a dictionary with a given buffer and size.
//! @param iter The dictionary iterator
//! @param tuplets The array of tuplets
//! @param tuplets_count The number of tuplets in the array
//! @param buffer The buffer in which to write the serialized dictionary
//! @param [in] size_in_out The available buffer size in bytes
//! @param [out] size_in_out The number of bytes written
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
DictionaryResult dict_serialize_tuplets_to_buffer_with_iter(DictionaryIterator *iter, const Tuplet * const tuplets, const uint8_t tuplets_count, uint8_t *buffer, uint32_t *size_in_out);

// Legacy version to prevent previous app breakage, __deprecated preserves order
DictionaryResult dict_serialize_tuplets_to_buffer_with_iter__deprecated(const uint8_t tuplets_count, const Tuplet * const tuplets, DictionaryIterator *iter, uint8_t *buffer, uint32_t *size_in_out);

//! Serializes a Tuplet and writes the resulting Tuple into a dictionary.
//! @param iter The dictionary iterator
//! @param tuplet The Tuplet describing the key/value pair to write
//! @return \ref DICT_OK, \ref DICT_NOT_ENOUGH_STORAGE or \ref DICT_INVALID_ARGS
DictionaryResult dict_write_tuplet(DictionaryIterator *iter, const Tuplet * const tuplet);

//! Calculates the number of bytes that a dictionary will occupy, given
//! one or more Tuplets that need to be stored in the dictionary.
//! @note See \ref dict_calc_buffer_size() for the formula for the calculation.
//! @param tuplets An array of Tuplets that need to be stored in the dictionary.
//! @param tuplets_count The total number of Tuplets that follow.
//! @return The total number of bytes of storage needed.
//! @see Tuplet
uint32_t dict_calc_buffer_size_from_tuplets(const Tuplet * const tuplets, const uint8_t tuplets_count);

// Legacy version to prevent previous app breakage, __deprecated preserves order
uint32_t dict_calc_buffer_size_from_tuplets__deprecated(const uint8_t tuplets_count, const Tuplet * const tuplets);

//! Tuple that represents an empty tuple.
//! @see DictionaryKeyUpdatedCallback
extern const Tuple * const NULL_TUPLE;

//! Type of the callback used in \ref dict_merge()
//! @param key The key that is being updated.
//! @param new_tuple The new tuple. The tuple points to the actual, updated destination dictionary or NULL_TUPLE
//! in case there was an error (e.g. backing buffer was too small).
//! Therefore the Tuple can be used after the callback returns, until the destination dictionary
//! storage is free'd (by the application itself).
//! @param old_tuple The values that will be replaced with `new_tuple`. The key, value and type will be
//! equal to the previous tuple in the old destination dictionary, however the `old_tuple points
//! to a stack-allocated copy of the old data.
//! @param context Pointer to application specific data
//! The storage backing `old_tuple` can only be used during the callback and
//! will no longer be valid after the callback returns.
//! @see dict_merge
typedef void (*DictionaryKeyUpdatedCallback)(const uint32_t key, const Tuple *new_tuple, const Tuple *old_tuple, void *context);

//! Merges entries from another "source" dictionary into a "destination" dictionary.
//! All Tuples from the source are written into the destination dictionary, while
//! updating the existing Tuples with matching keys.
//! @param dest The destination dictionary to update
//! @param [in,out] dest_max_size_in_out In: the maximum size of buffer backing `dest`. Out: the final size of the updated dictionary.
//! @param source The source dictionary of which its Tuples will be used to update dest.
//! @param update_existing_keys_only Specify True if only the existing keys in `dest` should be updated.
//! @param key_callback The callback that will be called for each Tuple in the merged destination dictionary.
//! @param context Pointer to app specific data that will get passed in when `update_key_callback` is called.
//! @return \ref DICT_OK, \ref DICT_INVALID_ARGS, \ref DICT_NOT_ENOUGH_STORAGE
DictionaryResult dict_merge(DictionaryIterator *dest, uint32_t *dest_max_size_in_out,
                             DictionaryIterator *source,
                             const bool update_existing_keys_only,
                             const DictionaryKeyUpdatedCallback key_callback, void *context);

//! Tries to find a Tuple with specified key in a dictionary
//! @param iter Iterator to the dictionary to search in.
//! @param key The key for which to find a Tuple
//! @return Pointer to a found Tuple, or NULL if there was no Tuple with the specified key.
Tuple *dict_find(const DictionaryIterator *iter, const uint32_t key);

//!   @} // end addtogroup Dictionary
//! @} // end addtogroup Foundation
