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


// This app only makes sense on Snowy, as it uses addresses and sector sizes that only make sense
// on our parallel flash hardware
#if CAPABILITY_USE_PARALLEL_FLASH

#include <inttypes.h>

#include "mfg_flash_test.h"

#include "drivers/flash.h"
#include "drivers/task_watchdog.h"
#include "flash_region/flash_region.h"
#include "system/logging.h"
#include "kernel/pbl_malloc.h"


static const uint8_t data_pattern = 0xAA;
static const uint8_t test_pattern = 0x55;
static volatile bool enable_flash_test = false;

static FlashTestErrorType prv_read_verify_byte(uint32_t read_addr,
                                               uint8_t expected_val,
                                               FlashTestErrorType err_code,
                                               uint8_t bitpos,
                                               bool disp_logs) {
  uint8_t read_buffer = 0;
  flash_read_bytes((uint8_t *)&read_buffer, read_addr, sizeof(read_buffer));

  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx8,
            read_addr, read_buffer);
  }

  if (read_buffer != expected_val) {
    switch (err_code) {
      case FLASH_TEST_ERR_ERASE:
        PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully erase the sector");
        break;
      case FLASH_TEST_ERR_STUCK_AT_HIGH:
        PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Address bit %d stuck at high", bitpos);
        break;
      case FLASH_TEST_ERR_STUCK_AT_LOW:
        PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Address bit %d stuck at low or shorted", bitpos);
        break;
      default:
        break;
    }
    return err_code;
  }
  
  return FLASH_TEST_SUCCESS;
}

static FlashTestErrorType prv_read_verify_halfword(uint32_t read_addr,
                                                   uint16_t expected_val,
                                                   FlashTestErrorType err_code,
                                                   bool disp_logs) {
  uint16_t read_buffer = 0;
  flash_read_bytes((uint8_t *)&read_buffer, read_addr, sizeof(read_buffer));

  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx16,
            read_addr, read_buffer);
  }

  if (read_buffer != expected_val) {
    switch (err_code) {
      case FLASH_TEST_ERR_ERASE:
        PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully erase the sector");
        break;
      case FLASH_TEST_ERR_DATA_WRITE:
        PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully write the data");
        break;
      default:
        break;
    }
    return err_code;
  }

  return FLASH_TEST_SUCCESS;
}

static FlashTestErrorType prv_write_read_verify_byte(uint32_t write_addr,
                                                     uint8_t write_val,
                                                     uint8_t expected_val,
                                                     bool disp_logs) {
  uint8_t write_buffer = write_val;
  uint8_t read_buffer = 0;
  // Write test pattern
  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Writing Addr 0x%"PRIx32" to value 0x%"PRIx8,
            write_addr, write_val);
  }
  flash_write_bytes((uint8_t *)&write_buffer, write_addr, sizeof(write_buffer));

  // Confirm write took place
  read_buffer = 0;
  flash_read_bytes((uint8_t*) &read_buffer, write_addr, sizeof(read_buffer));

  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx8,
            write_addr, read_buffer);
  }

  if (read_buffer != expected_val) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully write the data");
    return FLASH_TEST_ERR_DATA_WRITE;
  }

  return FLASH_TEST_SUCCESS;
}

static FlashTestErrorType prv_write_read_verify_halfword(uint32_t write_addr,
                                                         uint16_t write_val,
                                                         uint16_t expected_val,
                                                         bool disp_logs) {
  uint16_t write_buffer = write_val;
  uint16_t read_buffer = 0;

  // Write test pattern
  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Writing Addr 0x%"PRIx32" to value 0x%"PRIx16,
            write_addr, write_val);
  }

  flash_write_bytes((uint8_t *)&write_buffer, write_addr, sizeof(write_buffer));

  // Confirm write took place
  read_buffer = 0;
  flash_read_bytes((uint8_t*) &read_buffer, write_addr, sizeof(read_buffer));

  if (disp_logs) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx16,
            write_addr, read_buffer);
  }

  if (read_buffer != expected_val) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully write the data");
    return FLASH_TEST_ERR_DATA_WRITE;
  }

  return FLASH_TEST_SUCCESS;
}

#define VERIFY_TEST_STATUS(status)                       \
  do {                                                   \
    if (status != FLASH_TEST_SUCCESS) { return status; } \
  } while(0)

/***********************************************************/
/******************* DATA Test Functions *******************/
/***********************************************************/
static FlashTestErrorType prv_run_data_test(void) {
  FlashTestErrorType status = FLASH_TEST_SUCCESS;
  
  // Test each data bit using walking 1's method by toggling each data line
  PBL_LOG(LOG_LEVEL_DEBUG, ">START - DATA TEST 1: Data bus test");

  // Initialize region that is to be written
  uint16_t data_buffer = 0x0;
  uint8_t bitpos = 0;

  uint16_t read_buffer = 0x0;
  // Ensure within test data region and aligned to sector boundary
  uint32_t addr_region = (FLASH_TEST_ADDR_START + SECTOR_SIZE_BYTES) & SECTOR_ADDR_MASK;

  // Loop on each data bit - erase the sector, then write the next data value and verify that data
  // was written
  for (data_buffer = 1; data_buffer != 0; data_buffer <<= 1) {
    read_buffer = 0;
    flash_read_bytes((uint8_t*) &read_buffer, addr_region, sizeof(read_buffer));
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx16,
            addr_region, read_buffer);

    if (read_buffer != 0xFFFF) {
      // Erase sector only if necessary
      flash_erase_sector_blocking(addr_region);
      flash_read_bytes((uint8_t*) &read_buffer, addr_region, sizeof(read_buffer));
      PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx16,
              addr_region, read_buffer);
    }

    // All data should be set to 0xFFFF upon erase
    if (read_buffer != 0xFFFF) {
      PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Did not successfully erase the sector");
      return FLASH_TEST_ERR_ERASE;
    }

    // Read and compare data that was written
    status = prv_write_read_verify_halfword(addr_region, data_buffer, data_buffer, true);
    if (status != 0) {
      PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Data bit %d not returning correct data value", bitpos);
      return status;
    }

    bitpos++;
    addr_region += 4; // increment to the next address to avoid extra erases
  }

  PBL_LOG(LOG_LEVEL_DEBUG, ">PASS - DATA TEST 1: Data bus test");
  return status;
}

/***********************************************************/
/******************* Addr Test Functions *******************/
/***********************************************************/
// Write the test byte 0xAA at each power-of-two offset within the flash test range. If necessary,
// erase the sector that the byte resides in first.
// The base address always gets erased. If skip_base_addr is true, we leave it at 0xFF (erased)
// otherwise we write 0xAA to that address as well.
static FlashTestErrorType write_initial_pattern(bool display_logs, bool skip_base_addr,
                                                uint32_t* erase_addr) {
  FlashTestErrorType status = FLASH_TEST_SUCCESS;
  uint32_t base_addr = FLASH_TEST_ADDR_START;
  uint32_t test_addr = base_addr;
  uint32_t addr_mask = FLASH_TEST_ADDR_MSK;
  uint8_t read_buffer = 0;
  uint32_t bit_offset;

  if (display_logs) { PBL_LOG(LOG_LEVEL_DEBUG, ">>> Initializing data patterns..."); }
  if (display_logs) { PBL_LOG(LOG_LEVEL_DEBUG, ">>> Erasing sectors..."); }
  if (erase_addr) {
    // only erase the specific erase address
    if (display_logs) {
      PBL_LOG(LOG_LEVEL_DEBUG, ">> Erasing Addr 0x%"PRIx32, *erase_addr);
    }
    flash_erase_sector_blocking(*erase_addr);
  } else {
    // Erase the addresses within test range that we will touch. These are addresses with
    // power-of-two offsets
    for (bit_offset = 0; (bit_offset == 0) || (bit_offset & addr_mask); bit_offset <<= 1) {

      if (bit_offset > base_addr) {
        test_addr = bit_offset;
      } else {
        test_addr = base_addr + bit_offset; 
      }
      if (test_addr >= FLASH_TEST_ADDR_END) {
        break;
      }

      // skip erasing of unnecessary overlapping sectors
      if ((test_addr >= base_addr + SECTOR_SIZE_BYTES) ||
          (test_addr == base_addr + 1) || (test_addr == base_addr)) {
        // check if byte location is already 0xFF or default data pattern, then skip erase
        // Always erase base address

        flash_read_bytes((uint8_t*)&read_buffer, test_addr, sizeof(read_buffer));
        PBL_LOG(LOG_LEVEL_DEBUG, ">> Testing Addr 0x%"PRIx32", value:0x%x", test_addr, read_buffer);

        if (((read_buffer != 0xFF) && (read_buffer != 0xAA)) || (test_addr == base_addr)) {
          if (display_logs) {
            PBL_LOG(LOG_LEVEL_DEBUG, ">> Erasing Addr 0x%"PRIx32, test_addr);
          }
          flash_erase_sector_blocking(test_addr);

          // Verify data was erased
          status = prv_read_verify_byte(test_addr, 0xFF, FLASH_TEST_ERR_ERASE, 0, display_logs);
          VERIFY_TEST_STATUS(status);
        }
      }

      // After the base address, go up by power of 2's
      if (bit_offset == 0) {
        bit_offset = 1;
      }
    }
  }

  if (display_logs) { PBL_LOG(LOG_LEVEL_DEBUG, ">>> Erasing sectors...complete"); }

  // Write default data pattern to each power-of-two offset within the test region
  for (bit_offset = 1; (bit_offset & addr_mask) != 0; bit_offset <<= 1) {
    if (bit_offset > base_addr) {
      test_addr = bit_offset;
    } else {
      test_addr = base_addr + bit_offset;
    }
    if (test_addr >= FLASH_TEST_ADDR_END) {
      break;
    }

    // Write default data pattern to address if necessary
    status = prv_read_verify_byte(test_addr, data_pattern, FLASH_TEST_ERR_SKIP, 0, display_logs);
    if (status != FLASH_TEST_SUCCESS) {
      // Write default data pattern to address
      status = prv_write_read_verify_byte(test_addr, data_pattern, data_pattern, display_logs);
      VERIFY_TEST_STATUS(status);
    }
  }

  if (!skip_base_addr) {
    test_addr = base_addr;

    // Read initial value
    read_buffer = 0;
    flash_read_bytes((uint8_t*) &read_buffer, test_addr, sizeof(read_buffer));
    if (display_logs) {
      PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx8,
              test_addr, read_buffer);
    }

    // Write data pattern
    status = prv_write_read_verify_byte(test_addr, data_pattern, data_pattern, display_logs);
    VERIFY_TEST_STATUS(status);
  }

  if (display_logs) { PBL_LOG(LOG_LEVEL_DEBUG, ">>> Initializing data patterns...complete"); }

  return FLASH_TEST_SUCCESS;
}

static FlashTestErrorType prv_run_addr_test (void) {
  uint32_t base_addr = FLASH_TEST_ADDR_START;
  uint32_t test_addr = base_addr;
  uint32_t addr_mask = FLASH_TEST_ADDR_MSK;
  uint8_t read_buffer = 0;
  uint32_t bit_offset;
  uint32_t test_offset = 0;
  FlashTestErrorType status = FLASH_TEST_SUCCESS;

  ///////////////////////////////////////////////////
  /// Test 1: Check for address bits stuck at high
  ///////////////////////////////////////////////////
  PBL_LOG(LOG_LEVEL_DEBUG, ">START - ADDR TEST 1: Check for address bits stuck at high");

  // Write data pattern (0xAA) to each power-of-2 offset within the flash
  status = write_initial_pattern(true /*display_logs*/, false /*skip_base_addr*/,
                                 NULL /*erase_addr*/);
  VERIFY_TEST_STATUS(status);

  // offset of 0
  test_addr = base_addr + test_offset;

  // Read initial value
  read_buffer = 0;
  flash_read_bytes((uint8_t*) &read_buffer, test_addr, sizeof(read_buffer));
  PBL_LOG(LOG_LEVEL_DEBUG, ">> Reading Addr 0x%"PRIx32" value is 0x%"PRIx8,
          test_addr, read_buffer);

  // Write test pattern to address 0
  // After writing test pattern, data should be 0x00 since initial value was 0xAA and 0x55 was written
  status = prv_write_read_verify_byte(test_addr, test_pattern, 0x00, true /*display_logs*/);
  VERIFY_TEST_STATUS(status);


  // Check if any of the address bits are stuck at high. If they are, then the previous write to
  // address 0 would have trashed the data at one of the other addresses
  uint8_t base_addr_pos = 0;
  uint8_t bitpos;
  bool stuck_at_high = false;
  for (bit_offset = 1, bitpos = 0; bit_offset & addr_mask; bit_offset <<= 1, bitpos++) {
    if (bit_offset > base_addr) {
      test_addr = bit_offset;
    } else if (bit_offset == base_addr) {
      base_addr_pos = bitpos;
      // Skip base address check - that is done later
      PBL_LOG(LOG_LEVEL_DEBUG, "Skip base address bit position %d", bitpos);
      bitpos++;
      continue;
    } else {
      test_addr = base_addr + bit_offset; 
    }

    if (test_addr >= FLASH_TEST_ADDR_END) {
      PBL_LOG(LOG_LEVEL_DEBUG, "Skipping test address 0x%"PRIx32" which is out of range",
              test_addr);
      break;
    }


    // If test_pattern was written over the data_pattern, then return data should be 0 since
    // data cannot transition from 0 to 1 without an erase; else it will be initial data_pattern
    status = prv_read_verify_byte(test_addr, data_pattern, FLASH_TEST_ERR_STUCK_AT_HIGH, bitpos,
                                  true);
    if (status != FLASH_TEST_SUCCESS) {
      stuck_at_high = true;
    }
  }
  
  // Special case - test bit for base address 
  // - Use an address between FLASH_TEST_ADDR_START and base_addr
  PBL_LOG(LOG_LEVEL_DEBUG, ">> Testing special case for base address bit %d", base_addr_pos);
  // Read initial value
  test_addr = FLASH_REGION_FILESYSTEM_BEGIN;
  uint32_t special_case_addr = test_addr | base_addr;
  if ((test_addr >= base_addr) || (special_case_addr > FLASH_TEST_ADDR_END)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Cannot test address bit for base_addr");
    return FLASH_TEST_ERR_ADDR_RANGE;
  }

  // erase (base_addr | test_addr) and start of test space
  flash_erase_sector_blocking(test_addr);
  flash_erase_sector_blocking(special_case_addr);

  // Verify erase took place
  status = prv_read_verify_byte(test_addr, 0xFF, FLASH_TEST_ERR_ERASE, 0, true);
  VERIFY_TEST_STATUS(status);

  // Verify erase took place
  status = prv_read_verify_byte(special_case_addr, 0xFF, FLASH_TEST_ERR_ERASE, 0, true);
  VERIFY_TEST_STATUS(status);

  // Write test pattern to the test address
  // Data should be set to test_pattern since existing data should be 0xFF and we are writing
  // test_pattern
  status = prv_write_read_verify_byte(test_addr, test_pattern, test_pattern, true);
  VERIFY_TEST_STATUS(status);

  // Confirm write into base_addr did not take place
  // If test_pattern was written over the data_pattern, then return data should be 0 since 
  // data cannot transition from 0 to 1 without an erase
  status = prv_read_verify_byte(special_case_addr, 0xFF, FLASH_TEST_ERR_STUCK_AT_HIGH,
                                base_addr_pos, true);
  if (status != FLASH_TEST_SUCCESS) {
    stuck_at_high = true;
  }
  
  // If any bits are stuck at high, return error
  if (stuck_at_high) {
    return FLASH_TEST_ERR_STUCK_AT_HIGH; 
  }

  PBL_LOG(LOG_LEVEL_DEBUG, ">PASS - ADDR TEST 1: Check for address bits stuck at high");



  /////////////////////////////////////////////////////////////
  /// Test 2: Check for address bits stuck at low or shorted
  /////////////////////////////////////////////////////////////
  PBL_LOG(LOG_LEVEL_DEBUG, ">START - ADDR TEST 2: Check for address bits stuck at low or shorted");

  // NOTE that the previous test only modified the data at base_addr and left all other
  // power-of-2 addresses with the data pattern in them. The write_initial_pattern() method
  // will skip erasing a sector if all of the power of 2 addresses within it still have the
  // data pattern, so only the first sector will end up being re-erased.
  status = write_initial_pattern(true /*display_logs*/, false /*skip_base_addr*/,
                                 NULL /*erase_addr*/);
  VERIFY_TEST_STATUS(status);

  bool stuck_at_low = false;
  for (test_offset = 1, bitpos=0; test_offset & addr_mask; test_offset <<= 1, bitpos++) {

    if (test_offset >= base_addr) {
      test_addr = test_offset;
    } else {
      test_addr = base_addr + test_offset; 
    }
    if (test_addr >= FLASH_TEST_ADDR_END) {
      break;
    }

    // Skip base address
    if (test_addr == base_addr) {
      continue;
    }
    PBL_LOG(LOG_LEVEL_DEBUG, ">> Testing Stuck at Low at Addr 0x%"PRIx32, test_addr);

    // After we write test_pattern, data should be set to 0x00 since existing data should be 0xAA
    // and we are writing 0x55
    status = prv_write_read_verify_byte(test_addr, test_pattern, 0x00, false);
    VERIFY_TEST_STATUS(status);

    // read base address to insure that it wasn't modified due to a stuck at zero in an address
    // bit
    status = prv_read_verify_byte(base_addr, data_pattern, FLASH_TEST_ERR_STUCK_AT_LOW, bitpos,
                                  false);
    if (status != FLASH_TEST_SUCCESS) {
      stuck_at_low = true;
    }

    // Check if any other address bits are shorted with our test bit. If shorted, then we would
    // read 0 from the address bit which is shorted with the test one.
    // We only have to check shorts with higher address bits, since we've already checked for
    // shorts from the lower address bits to this one.
    uint8_t bitpos2 = bitpos+1;
    for (bit_offset = test_offset << 1; bit_offset & addr_mask; bit_offset <<= 1, bitpos2++) {
      // skip same offset
      if (bit_offset == test_offset) {
        continue;
      }

      uint32_t test_addr2;
      if (bit_offset >= base_addr) {
        test_addr2 = bit_offset;
      } else {
        test_addr2 = base_addr + bit_offset;
      }
      if (test_addr2 >= FLASH_TEST_ADDR_END) {
        break;
      }

      status = prv_read_verify_byte(test_addr2, data_pattern, FLASH_TEST_ERR_STUCK_AT_LOW, bitpos2,
                                    false /*display_logs*/);
      if (status != FLASH_TEST_SUCCESS) {
        stuck_at_low = true;
      }
    }

    if (stuck_at_low) {
      // Restore data back to original if stuck at low occurred
      status = write_initial_pattern(false /*display_logs*/, false /*skip_base_addr*/,
                                     NULL /*base_addr*/);
    }

    VERIFY_TEST_STATUS(status);
  }

  if (stuck_at_low) {
    return FLASH_TEST_ERR_STUCK_AT_LOW;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, ">PASS - ADDR TEST 2: Check for address bits stuck at low or shorted");

  return FLASH_TEST_SUCCESS;
}

/***********************************************************/
/******************* Stress Test Functions *****************/
/***********************************************************/
#define FLASH_TEST_STRESS_ADDR1 0x00A5A5A5
#define FLASH_TEST_STRESS_DATA1     0x5A5A
#define FLASH_TEST_STRESS_ADDR2 0x00CA5A5A
#define FLASH_TEST_STRESS_DATA2     0xA5A5
static FlashTestErrorType setup_stress_addr_test(void) {
  FlashTestErrorType status = FLASH_TEST_SUCCESS;

  // Read/Write from address 1
  uint32_t stress_addr1 = FLASH_TEST_STRESS_ADDR1;
  uint16_t stress_data1 = FLASH_TEST_STRESS_DATA1;

  // Read/Write from address 2
  uint32_t stress_addr2 = FLASH_TEST_STRESS_ADDR2;
  uint16_t stress_data2 = FLASH_TEST_STRESS_DATA2;

  if ((stress_addr1 < FLASH_REGION_FILESYSTEM_BEGIN) ||
      (stress_addr1 >= FLASH_REGION_FILESYSTEM_END)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Invalid range");
    return FLASH_TEST_ERR_ADDR_RANGE;
  }

  if ((stress_addr2 < FLASH_REGION_FILESYSTEM_BEGIN) ||
      (stress_addr2 >= FLASH_REGION_FILESYSTEM_END)) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Invalid range");
    return FLASH_TEST_ERR_ADDR_RANGE;
  }

  // Erase sectors
  flash_erase_sector_blocking(stress_addr1);
  status = prv_read_verify_halfword(stress_addr1, 0xFFFF, FLASH_TEST_ERR_ERASE, false);
  VERIFY_TEST_STATUS(status);
  flash_erase_sector_blocking(stress_addr2);
  status = prv_read_verify_halfword(stress_addr2, 0xFFFF, FLASH_TEST_ERR_ERASE, false);
  VERIFY_TEST_STATUS(status);

  // Write data to stress address locations
  PBL_LOG(LOG_LEVEL_DEBUG, ">> Writing Addr 0x%"PRIx32" to value 0x%"PRIx16,
          stress_addr1, stress_data1);
  flash_write_bytes((uint8_t *)&stress_data1, stress_addr1, sizeof(stress_data1));

  PBL_LOG(LOG_LEVEL_DEBUG, ">> Writing Addr 0x%"PRIx32" to value 0x%"PRIx16,
          stress_addr2, stress_data2);
  flash_write_bytes((uint8_t *)&stress_data2, stress_addr2, sizeof(stress_data2));

  return FLASH_TEST_SUCCESS;
}

// Run address read/write stess test - if iterations is 0, then stop only when button is pushed; 
//   else go until iterations hit
static FlashTestErrorType prv_run_stress_addr_test(uint32_t iterations) {
  PBL_LOG(LOG_LEVEL_DEBUG, ">START - STRESS TEST 1");

  uint16_t halfwordcount = 0;
  unsigned int iteration_count = 0;

  // Read/Write from address 1
  uint32_t stress_addr1 = FLASH_TEST_STRESS_ADDR1;
  uint16_t stress_data1 = FLASH_TEST_STRESS_DATA1;

  // Read/Write from address 2
  uint32_t stress_addr2 = FLASH_TEST_STRESS_ADDR2;
  uint16_t stress_data2 = FLASH_TEST_STRESS_DATA2;

  FlashTestErrorType status = setup_stress_addr_test();
  if (status != FLASH_TEST_SUCCESS) {
    return status;
  }

  // Keep going until DOWN button is pushed or iterations reached
  while(((iterations == 0) && enable_flash_test) || 
        ((iterations > 0) && (iteration_count < iterations))) {
    // Confirm write took place - data should now be set to stress_data1
    status = prv_read_verify_halfword(stress_addr1, stress_data1, FLASH_TEST_ERR_DATA_WRITE, false);
    VERIFY_TEST_STATUS(status);
    halfwordcount++;

    // Confirm write took place - data should now be set to stress_data2
    status = prv_read_verify_halfword(stress_addr2, stress_data2, FLASH_TEST_ERR_DATA_WRITE, false);
    VERIFY_TEST_STATUS(status);
    halfwordcount++;

    if (halfwordcount*2 % (256*1024) == 0) {
      // Reading flash words (which are 16 bits) hence double
      if (iterations) {
        PBL_LOG(LOG_LEVEL_DEBUG, ">> Read 256KB, iteration: %d of %"PRId32,
                iteration_count, iterations);
      } else {
        PBL_LOG(LOG_LEVEL_DEBUG, ">> Read 256KB, iteration: %d", iteration_count);
      }
    }

    iteration_count++;
  }

  PBL_LOG(LOG_LEVEL_DEBUG, "Ran %d iterations", iteration_count);
  PBL_LOG(LOG_LEVEL_DEBUG, ">PASS - STRESS TEST 1");

  return FLASH_TEST_SUCCESS;
}

/***********************************************************/
/******************* Perf Data Test Functions **************/
/***********************************************************/

#define COUNTER_START \
  uint32_t _start = *((volatile uint32_t *)0xE0001004);\
  uint32_t _tot = 0
#define COUNTER_STOP \
  uint32_t _end = *((volatile uint32_t *)0xE0001004)
#define COUNTER_PRINT(x)                   \
  do {                                     \
    if (_end > _start) {                   \
      _tot += (_end - _start);             \
    } else {                               \
     _tot += (UINT32_MAX - _start) + _end; \
    }                                      \
    PBL_LOG(LOG_LEVEL_DEBUG, "Read %lu bytes %lu ticks %lu us", x, _tot, _tot / 64); \
} while (0)

#define SWAP(a, b)     \
  do {                 \
    uint32_t temp = a; \
    a = b;             \
    b = temp;          \
  } while (0)
  
// Run performance test to measure data access times
#define DWT_CTRL_ADDR   0xE0001000
#define DWT_CYCCNT_ADDR 0xE0001004
#define MAX_READ_BUFF_SIZE 4096 // 4KB
static FlashTestErrorType prv_run_perf_data_test(void) {
  uint8_t *read_buffer = (uint8_t *) app_malloc(MAX_READ_BUFF_SIZE);
  if (!read_buffer) {
    PBL_LOG(LOG_LEVEL_DEBUG, "ERROR: Not enough memory to run test");
    return FLASH_TEST_ERR_OOM;
  }

  uint32_t addr = FLASH_TEST_ADDR_START;
  volatile uint32_t *ptr = (uint32_t *) DWT_CTRL_ADDR;
  for (uint32_t num_bytes = 1; num_bytes <= MAX_READ_BUFF_SIZE; num_bytes<<=1) {
    // Run test three times and print out the median throughput
    uint32_t ticks[3] = {0, 0, 0};
    for (uint8_t repeat = 0; repeat < 3; repeat++) {
      *ptr = *ptr & 0xFFFFFFFE;
      *((volatile uint32_t *)DWT_CYCCNT_ADDR) = 0;
      *ptr = *ptr | 0x1;
      COUNTER_START;
      flash_read_bytes((uint8_t *)&read_buffer[0], addr, num_bytes);
      COUNTER_STOP;
      COUNTER_PRINT(num_bytes);
      ticks[repeat] = _tot;
    }
    
    // Do a simple sort
    if (ticks[0] > ticks[1]) {
      SWAP(ticks[0], ticks[1]);
    }
    if (ticks[1] > ticks[2]) {
      SWAP(ticks[1], ticks[2]);
      if (ticks[0] > ticks[1]) {
        SWAP(ticks[0], ticks[1]);
      }
    }
    
    PBL_LOG(LOG_LEVEL_DEBUG, "Read %lu bytes, median throughput %lu KBps", num_bytes, (num_bytes * 1000 * 64 / ticks[1]));
  }
  
  app_free(read_buffer);
  return FLASH_TEST_SUCCESS;
}

/***********************************************************/
/******************* Wrapper Functions *********************/
/***********************************************************/
void stop_flash_test_case( void ) {
  enable_flash_test = false;
}

FlashTestErrorType run_flash_test_case(FlashTestCaseType test_case_num, uint32_t iterations) {
  FlashTestErrorType status = FLASH_TEST_SUCCESS;
  
  // Disable watchdog if enabled
  bool previous_task_watchdog_state = task_watchdog_mask_get(pebble_task_get_current());
  if (previous_task_watchdog_state) {
    task_watchdog_mask_clear(pebble_task_get_current());
  }
  
  enable_flash_test = true;

  // Schedule test to run
  switch (test_case_num) {
    case FLASH_TEST_CASE_RUN_DATA_TEST:
      status = prv_run_data_test();
      break;
    case FLASH_TEST_CASE_RUN_ADDR_TEST:
      status = prv_run_addr_test();
      break;
    case FLASH_TEST_CASE_RUN_STRESS_ADDR_TEST:
      status = prv_run_stress_addr_test(iterations);
      break;
    case FLASH_TEST_CASE_RUN_PERF_DATA_TEST:
      status = prv_run_perf_data_test();
      break;
    case FLASH_TEST_CASE_RUN_SWITCH_MODE_ASYNC:
    case FLASH_TEST_CASE_RUN_SWITCH_MODE_SYNC_BURST:
      flash_switch_mode(test_case_num - FLASH_TEST_CASE_RUN_SWITCH_MODE_ASYNC);
      status = FLASH_TEST_SUCCESS;
      break;
    default:
      status = FLASH_TEST_ERR_UNSUPPORTED;
      break;
  }

  enable_flash_test = false;

  if (status == FLASH_TEST_SUCCESS) {
    PBL_LOG(LOG_LEVEL_DEBUG, ">>>>>PASS FLASH TEST CASE %d<<<<<", test_case_num);
  }
  else {
    PBL_LOG(LOG_LEVEL_DEBUG, ">>>>>FAIL FLASH TEST CASE %d, Status: %d<<<<<", test_case_num, status);
  }
  
  // Re-enable watchdog state if previously enabled
  if (previous_task_watchdog_state) {
    task_watchdog_bit_set(pebble_task_get_current());
    task_watchdog_mask_set(pebble_task_get_current());
  }
  
  return status;
}

#endif // CAPABILITY_USE_PARALLEL_FLASH
