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

#include <stdint.h>
#include <string.h>

#include "applib/accel_service.h"
#include "services/common/hrm/hrm_manager_private.h"
#include "services/normal/activity/activity_algorithm.h"
#include "services/normal/activity/kraepelin/activity_algorithm_kraepelin.h"
#include "services/normal/activity/kraepelin/kraepelin_algorithm.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/list.h"
#include "util/math.h"
#include "util/size.h"
#include "util/time/time.h"

#include "clar.h"

// Stubs
#include "stubs_hexdump.h"
#include "stubs_logging.h"
#include "stubs_passert.h"
#include "stubs_pbl_malloc.h"
#include "stubs_prompt.h"
#include "stubs_sleep.h"

// Fakes
#include "fake_rtc.h"

HRMSessionRef s_hrm_next_session_ref = 1;
HRMSessionRef hrm_manager_subscribe_with_callback(AppInstallId app_id, uint32_t update_interval_s,
                                                  uint16_t expire_s, HRMFeature features,
                                                  HRMSubscriberCallback callback, void *context) {
  return s_hrm_next_session_ref++;
}

bool sys_hrm_manager_unsubscribe(HRMSessionRef session) {
  cl_assert(session < s_hrm_next_session_ref);
  return true;
}

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

extern char *strdup(const char *s);

// Define this to save stats to a file at the end of unit tests
// #define STATS_FILE_NAME "/Users/ronmarianetti/Temp/stats.csv"

// Define this to run only 1 of the step tests
// #define STEP_TEST_ONLY "walk_100_pbl_25296_1"

// Define this to run only 1 of the step tests
// #define SLEEP_TEST_ONLY "sleep_1_end_deep"

// Define this to run only 1 of the activity tests
// #define ACTIVITY_TEST_ONLY "walk_activities_0"

// Implemented in activity/step_samples.c
AccelRawData *activity_sample_30_steps(int *len);
AccelRawData *activity_sample_working_at_desk(int *len);
AccelRawData *activity_sample_not_moving(int *len);

// Implemented in activity/sleep_samples_v1.c
AlgMinuteFileSampleV5 *activity_sample_sleep_v1_1(int *len);

typedef AccelRawData *AccelRawDataPtr;
typedef AccelRawDataPtr (*ActivitySamplesFunc)(int *len);

// These are the values we capture and compare against for every minute of accel data
typedef struct {
  uint8_t steps;                    // # of steps in this minute
  uint8_t orientation;              // average orientation of the watch
  uint16_t vmc;                     // VMC (Vector Magnitude Counts) for this minute
} TestMinuteData;

// ---------------------------------------------------------------------------------------------
// Structure holding the samples and expected values for a step sample discovered on the file
// system
typedef struct {
  char name[256];
  AccelRawData *samples;
  int num_samples;
  int exp_steps;
  int exp_steps_min;
  int exp_steps_max;
  float weight;               // Weight percent error by this factor
  int test_idx;               // used after we run the test, for sorting
} StepFileTestEntry;


// ---------------------------------------------------------------------------------------------
// Array of samples and expected results for sleep tests
typedef AlgMinuteFileSample *AlgSleepMinuteDataPtr;
typedef AlgSleepMinuteDataPtr (*ActivityMinuteFunc)(int *len);
typedef struct {
  int value;
  int min;
  int max;
} ExpectedValue;

typedef struct {
  int value;
  bool passed;
} ActualValue;

typedef struct {
  char name[256];
  AlgMinuteFileSample *samples;
  int num_samples;
  int version;

  ExpectedValue total;
  ExpectedValue deep;
  ExpectedValue start_at;
  ExpectedValue end_at;
  ExpectedValue cur_state_elapsed;
  ExpectedValue in_sleep;
  ExpectedValue in_deep_sleep;

  float weight;               // Weight percent error by this factor
  int test_idx;
  int force_shut_down_at;
} SleepFileTestEntry;

typedef struct {
  ActualValue total;
  ActualValue deep;
  ActualValue start_at;
  ActualValue end_at;
  ActualValue cur_state_elapsed;
  ActualValue in_sleep;
  ActualValue in_deep_sleep;

  float weighted_err;
  bool all_passed;
} SleepTestResults;


typedef struct {
  char name[256];
  AlgMinuteFileSample *samples;
  int num_samples;
  int version;

  ExpectedValue activity_type;
  ExpectedValue len;
  ExpectedValue start_at;

  float weight;               // Weight percent error by this factor
  int test_idx;
  int force_shut_down_at;
} ActivityFileTestEntry;

typedef struct {
  ActualValue activity_type;
  ActualValue len;
  ActualValue start_at;

  float weighted_err;
  bool all_passed;
} ActivityTestResults;


typedef struct {
  KAlgActivityType activity;
  time_t start_utc;
  uint16_t len_minutes;
  bool ongoing;
  uint16_t steps;
  uint32_t resting_calories;
  uint32_t active_calories;
  uint32_t distance_mm;
} KAlgTestActivitySession;


// ---------------------------------------------------------------------------------------------
// Globals
void *s_kalg_state;


// ==================================================================================
// Assertion utilities
// Assert that a particular activity session is present in the sessions list
static void prv_assert_activity_present(KAlgTestActivitySession *sessions, int num_sessions,
                                        KAlgTestActivitySession *exp_session,
                                        char *file, int line) {
  for (int i = 0; i < num_sessions; i++) {
    if (sessions[i].activity == exp_session->activity
        && sessions[i].start_utc == exp_session->start_utc
        && sessions[i].len_minutes == exp_session->len_minutes
        && sessions[i].active_calories == exp_session->active_calories
        && sessions[i].resting_calories == exp_session->resting_calories
        && sessions[i].steps == exp_session->steps) {
      return;
    }
  }
  printf("\nFound activities:");
  for (int i = 0; i < num_sessions; i++) {
    printf("\nFound:       type: %d, start_utc: %d, len: %"PRIu16", steps: %"PRIu16", "
           "rest_cal: %"PRIu32", active_cal: %"PRIu32", dist: %"PRIu32" ",
           (int)sessions[i].activity, (int)sessions[i].start_utc, sessions[i].len_minutes,
           sessions[i].steps, sessions[i].resting_calories, sessions[i].active_calories,
           sessions[i].distance_mm);
  }
  printf("\nLooking for: type: %d, start_utc: %d, len: %"PRIu16", steps: %"PRIu16", "
         "rest_cal: %"PRIu32", active_cal: %"PRIu32", dist: %"PRIu32" ",
         (int)exp_session->activity, (int)exp_session->start_utc,
         exp_session->len_minutes, exp_session->steps, exp_session->resting_calories,
         exp_session->active_calories, exp_session->distance_mm);
  clar__assert(false, file, line, "Missing activity record", "", true);
}

#define ASSERT_ACTIVITY_SESSION_PRESENT(sessions, num_sessions, session) \
        prv_assert_activity_present((sessions), (num_sessions), (session), __FILE__, __LINE__)



// ==================================================================================
// Functions used for collecting stats and writing them out to a csv
static const int k_stats_max_columns = 32;
typedef struct {
  ListNode node;
  uint32_t values[k_stats_max_columns];
} StatsRow;
typedef enum {
  StatsEpochTypeNonStepping = 0,
  StatsEpochTypePartialStepping = 1,   // First or last epoch in a test
  StatsEpochTypeStepping = 2,   // First or last epoch in a test
} StatsEpochType;

static int s_stats_num_columns = 0;
static char *s_stats_column_names[k_stats_max_columns];
static StatsRow *s_stat_rows;


// ---------------------------------------------------------------------------------------
static void prv_stats_reinit(void) {
  // Delete stuff from prior stats run
  cl_assert(s_stats_num_columns < k_stats_max_columns);
  for (int i = 0; i < s_stats_num_columns; i++) {
    free(s_stats_column_names[i]);
    s_stats_column_names[i] = NULL;
  }

  // Free the rows
  StatsRow *next = s_stat_rows;
  while (next) {
    StatsRow *to_free = next;
    next = (StatsRow *)next->node.next;
    free(to_free);
  }
  s_stat_rows = NULL;
  s_stats_num_columns = 0;
}


// ---------------------------------------------------------------------------------------
// Callback called by the algorithm. This collects stats from an epoch
static void prv_stats_cb(uint32_t num_stats, const char **names, int32_t *values) {
  if (s_stats_num_columns == 0) {
    // Save the column names if this is the first row
    s_stats_num_columns = num_stats;
    for (int i = 0; i < num_stats; i++) {
      s_stats_column_names[i] = strdup(names[i]);
    }
  }

  cl_assert_equal_i(num_stats, s_stats_num_columns);

  // Create a new row of stats
  StatsRow *stats = malloc(sizeof(StatsRow));
  memset(stats, 0, sizeof(*stats));

  // Collect the stats and also print them out
  for (int i = 0; i < num_stats; i++) {
    printf("%s: %d, ", names[i], values[i]);
    stats->values[i] = values[i];
  }
  printf("\n");

  // Append to the list
  if (s_stat_rows) {
    list_append(&s_stat_rows->node, &stats->node);
  } else {
    s_stat_rows = stats;
  }
}


// ---------------------------------------------------------------------------------------
// Set a specific column in the last row by name
static void prv_stats_set_last_row_value(const char* name, uint32_t value) {
  if (s_stat_rows == NULL) {
    return;
  }
  StatsRow *stats = (StatsRow *)list_get_tail(&s_stat_rows->node);
  cl_assert(stats != NULL);
  bool found = 0;
  for (int i = 0; i < s_stats_num_columns; i++) {
    if (strcmp(s_stats_column_names[i], name) == 0) {
      found = true;
      stats->values[i] = value;
      break;
    }
  }
  cl_assert(found);
}


// ---------------------------------------------------------------------------------------
// Write out accumulated stats to a csv file
static void prv_stats_write(const char* filename, bool create, const char *test_name,
                            bool is_stepping) {
  if (!s_stat_rows) {
    return;
  }

  FILE *file = NULL;
  if (create) {
    // Write the column names when creating the file
    file = fopen(filename, "w");
    cl_assert(file);

    fprintf(file, "test, epoch_type, epoch_idx");
    for (int i = 0; i < s_stats_num_columns; i++) {
      fprintf(file, ", %s", s_stats_column_names[i]);
    }
    fprintf(file, "\n");
  } else {
    file = fopen(filename, "a");
    cl_assert(file);
  }

  // Write out the column values for each row
  StatsRow *row = s_stat_rows;
  int row_idx = 0;
  for (; row != NULL; row = (StatsRow *)row->node.next, row_idx++) {
    fprintf(file, "\"%s\"", test_name);
    StatsEpochType epoch_type;
    if (!is_stepping) {
      epoch_type = StatsEpochTypeNonStepping;
    } else if (row_idx == 0 || !row->node.next) {
      // We consider the first and last epoch of each sample as a "partial stepping" epoch.
      epoch_type = StatsEpochTypePartialStepping;
    } else {
      epoch_type = StatsEpochTypeStepping;
    }
    fprintf(file, " ,%d, %d", (int)epoch_type, row_idx);
    for (int i = 0; i < s_stats_num_columns; i++) {
      fprintf(file, " ,%d", row->values[i]);
    }
    fprintf(file, "\n");
  }

  cl_assert_equal_i(0, fclose(file));
  printf("Stats written to file: %s", filename);
}


// ---------------------------------------------------------------------------------------
// Run samples through the algorithm integrated into the firmware
// @param[in] data array of samples
// @param[in] num_samples size of data array
// @param[in] minute_data array to return minute data in
// @param[in,out] minute_data_len size of minute_data array on entry, # of filled entries
//                 on exit
// @param return number of steps computed.
static uint32_t prv_feed_kalg_samples(AccelRawData *data, int num_samples,
                                      TestMinuteData *minute_data_array, int *minute_data_len) {
  uint32_t total_steps = 0;
  uint32_t minute_steps = 0;
  int num_minutes_captured = 0;
  int num_samples_left = num_samples;

  // Init  state
  s_kalg_state = kernel_zalloc(kalg_state_size());
  kalg_init(s_kalg_state, prv_stats_cb);

  // Run some data through it, 1 minute at a time
  while (num_samples_left) {
    int chunk_size = MIN(num_samples_left, KALG_SAMPLE_HZ * SECONDS_PER_MINUTE);
    uint32_t steps;
    uint32_t consumed_samples;
    steps = kalg_analyze_samples(s_kalg_state, data, chunk_size, &consumed_samples);
    minute_steps += steps;
    total_steps += steps;

    if (chunk_size == KALG_SAMPLE_HZ * SECONDS_PER_MINUTE) {
      // Capture the minute data for each minute
      TestMinuteData minute_data = {
        .steps = minute_steps,
      };
      bool still;
      kalg_minute_stats(s_kalg_state, &minute_data.vmc, &minute_data.orientation, &still);

      PBL_ASSERTN(num_minutes_captured < *minute_data_len);
      minute_data_array[num_minutes_captured++] = minute_data;
      minute_steps = 0;
    }
    num_samples_left -= chunk_size;
    data += chunk_size;
  }

  // -------------------------------------------------------
  // Leftover data in epoch, if any
  total_steps += kalg_analyze_finish_epoch(s_kalg_state);

  TestMinuteData minute_data = {
    .steps = minute_steps,
  };
  bool still;
  kalg_minute_stats(s_kalg_state, &minute_data.vmc, &minute_data.orientation, &still);
  PBL_ASSERTN(num_minutes_captured < *minute_data_len);
  minute_data_array[num_minutes_captured++] = minute_data;

  // ----------------------------------------------------
  // Free state
  kernel_free(s_kalg_state);
  s_kalg_state = NULL;

  *minute_data_len = num_minutes_captured;
  return total_steps;
}


// ---------------------------------------------------------------------------------------
// Run samples through the reference algorithm.
static uint32_t prv_feed_reference_samples(AccelRawData *data, int num_samples) {
  extern int ref_accel_data_handler(AccelData *data, uint32_t num_samples );
  extern void ref_init(void);
  extern int ref_finish_epoch(void);
  extern void ref_minute_stats(uint8_t *orientation, uint8_t *vmc);

  int steps = 0;
  uint8_t orientation, vmc;

  ref_init();
  AccelData accel_buf[KALG_SAMPLE_HZ];

  int chunk_size = 0;
  int samples_in_minute = 0;
  for (uint32_t i = 0; i < num_samples; i++) {
    accel_buf[chunk_size++] = (AccelData) {
      .x = data[i].x,
      .y = data[i].y,
      .z = data[i].z
    };
    samples_in_minute++;
    if (chunk_size == KALG_SAMPLE_HZ) {
      steps = ref_accel_data_handler(accel_buf, chunk_size);
      chunk_size = 0;
    }
    if (samples_in_minute >= KALG_SAMPLE_HZ * SECONDS_PER_MINUTE) {
      ref_minute_stats(&orientation, &vmc);
      samples_in_minute = 0;
    }
  }

  // leftover data, if any
  if (chunk_size > 0) {
    steps = ref_accel_data_handler(accel_buf, chunk_size);
  }
  steps = ref_finish_epoch();
  ref_minute_stats(&orientation, &vmc);

  PBL_LOG(LOG_LEVEL_DEBUG, "processed %d samples (%d seconds) of data: %d steps",
          num_samples, num_samples / KALG_SAMPLE_HZ, steps);
  return steps;
}


// ----------------------------------------------------------------------------------
// The file discovery state definitions
typedef enum {
  SampleFileType_AccelSamples,
  SampleFileType_MinuteSamples,
} SampleFileType;

typedef struct {
  char *res_path;                 // path to directory containing sample files
  DIR *dp;                        // Directory pointer
  struct dirent *ep;              // Entry we are currently processing
  FILE *file;                     // File we currently have open
  SampleFileType type;            // type of samples
} SampleDiscoveryState;

#define ACCEL_SAMPLES_DISCOVERY_MAX_SAMPLES (12 * SECONDS_PER_MINUTE * KALG_SAMPLE_HZ)
typedef struct {
  SampleDiscoveryState common;
  AccelRawData samples[ACCEL_SAMPLES_DISCOVERY_MAX_SAMPLES];
  StepFileTestEntry test_entry;
} AccelSampleDiscoveryState;
static AccelSampleDiscoveryState s_accel_sample_discovery_state;

#define SLEEP_SAMPLES_DISCOVERY_MAX_SAMPLES (40 * MINUTES_PER_HOUR)
typedef struct {
  SampleDiscoveryState common;
  AlgMinuteFileSample samples[SLEEP_SAMPLES_DISCOVERY_MAX_SAMPLES];
  SleepFileTestEntry test_entry;
} SleepSampleDiscoveryState;
static SleepSampleDiscoveryState s_sleep_sample_discovery_state;

#define ACTIVITY_SAMPLES_DISCOVERY_MAX_SAMPLES (40 * MINUTES_PER_HOUR)
typedef struct {
  SampleDiscoveryState common;
  AlgMinuteFileSample samples[ACTIVITY_SAMPLES_DISCOVERY_MAX_SAMPLES];
  ActivityFileTestEntry test_entry;
} ActivitySampleDiscoveryState;
static ActivitySampleDiscoveryState s_activity_sample_discovery_state;



// ---------------------------------------------------------------------------------------
static bool prv_parse_accel_samples_file(AccelSampleDiscoveryState *state) {
  // Init for next set of samples
  state->test_entry = (StepFileTestEntry) {
    .samples = state->samples,
    .exp_steps = -1,
    .exp_steps_min = -1,
    .exp_steps_max = -1,
    .weight = 1.0,
  };

  char line_buf[256];
  while (true) {
    char *line = fgets(line_buf, sizeof(line_buf), state->common.file);
    if (!line) {
      // EOF
      break;
    }
    //printf("\nGot line: %s", line);

    // Find first token
    char *token = strtok(line, " \t\n");
    if (!token) {
      continue;
    }

    // If this is a pre-processor directive, skip it
    if (token[0] == '#') {
      continue;
    }

    // If this is a comment skip it
    if (strcmp(token, "//") == 0) {
      continue;
    }

    // If this is an AccelRawData line, get the name
    if (strcmp(token, "AccelRawData") == 0) {
      PBL_ASSERT(state->test_entry.name[0] == 0, "Unexpected start of new samples");

      token = strtok(NULL, "(");
      // Copy starting from token + 1 to skip the '*' at the front
      strncpy(state->test_entry.name, token + 1, sizeof(state->test_entry.name));
      printf("\nParsing function samples: %s", state->test_entry.name);
      continue;
    }

    // Look for and parse the expected values
    if (strcmp(token, "//>") == 0) {
      token = strtok(NULL, " \t\n");
      if (strcmp(token, "TEST_EXPECTED") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.exp_steps);
      } else if (strcmp(token, "TEST_EXPECTED_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.exp_steps_min);
      } else if (strcmp(token, "TEST_EXPECTED_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.exp_steps_max);
      } else if (strcmp(token, "TEST_WEIGHT") == 0) {
        sscanf(token + strlen(token) + 1, "%f", &state->test_entry.weight);
      } else if (strcmp(token, "TEST_NAME") == 0) {
        sscanf(token + strlen(token) + 1, "%s", state->test_entry.name);
      }
    }

    // If this is a "static AccelRawData samples[] = {" line, skip it
    if (strcmp(token, "static") == 0) {
      continue;
    }

    // Grab a sample
    if (strcmp(token, "{") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      int x, y, z;
      sscanf(token + strlen(token) + 1, "%d, %d, %d", &x, &y, &z);
      fflush(stdout);

      PBL_ASSERTN(state->test_entry.num_samples < ACCEL_SAMPLES_DISCOVERY_MAX_SAMPLES);
      state->samples[state->test_entry.num_samples++] = (AccelRawData) {
        .x = x,
        .y = y,
        .z = z,
      };
      continue;
    }

    // End of a sample
    if (strcmp(token, "}") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      break;
    }
  }

  // Did we get samples?
  if (state->test_entry.num_samples > 0) {
    return true;
  } else {
    return false;
  }
}


// ---------------------------------------------------------------------------------------
static bool prv_parse_sleep_samples_file(SleepSampleDiscoveryState *state) {
  // Init for next set of samples
  state->test_entry = (SleepFileTestEntry) {
    .samples = state->samples,
    .version = 1,
    .total = {-1, -1, -1},
    .deep = {-1, -1, -1},
    .start_at = {-1, -1, -1},
    .end_at = {-1, -1, -1},
    .cur_state_elapsed = {-1, -1, -1},
    .in_sleep = {-1, -1, -1},
    .in_deep_sleep = {-1, -1, -1},
    .weight = 1.0,
    .force_shut_down_at = -1,
  };

  char line_buf[256];
  while (true) {
    char *line = fgets(line_buf, sizeof(line_buf), state->common.file);
    if (!line) {
      // EOF
      break;
    }
    //printf("\nGot line: %s", line);

    // Find first token
    char *token = strtok(line, " \t\n");
    if (!token) {
      continue;
    }

    // If this is a pre-processor directive, skip it
    if (token[0] == '#') {
      continue;
    }

    // If this is a comment skip it
    if (strcmp(token, "//") == 0) {
      continue;
    }

    // If this is an AlgDlsMinuteData line, get the name
    if (strcmp(token, "AlgDlsMinuteData") == 0) {
      PBL_ASSERT(state->test_entry.name[0] == 0, "Unexpected start of new samples");

      token = strtok(NULL, "(");
      // Copy starting from token + 1 to skip the '*' at the front
      strncpy(state->test_entry.name, token + 1, sizeof(state->test_entry.name));
      printf("\nParsing function samples: %s", state->test_entry.name);
      continue;
    }

    // Look for and parse the expected values
    if (strcmp(token, "//>") == 0) {
      token = strtok(NULL, " \t\n");
      if (strcmp(token, "TEST_VERSION") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.version);

      } else if (strcmp(token, "TEST_TOTAL") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.total.value);
      } else if (strcmp(token, "TEST_TOTAL_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.total.min);
      } else if (strcmp(token, "TEST_TOTAL_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.total.max);

      } else if (strcmp(token, "TEST_DEEP") == 0) {
          sscanf(token + strlen(token) + 1, "%d", &state->test_entry.deep.value);
      } else if (strcmp(token, "TEST_DEEP_MIN") == 0) {
          sscanf(token + strlen(token) + 1, "%d", &state->test_entry.deep.min);
      } else if (strcmp(token, "TEST_DEEP_MAX") == 0) {
          sscanf(token + strlen(token) + 1, "%d", &state->test_entry.deep.max);

      } else if (strcmp(token, "TEST_START_AT") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.value);
      } else if (strcmp(token, "TEST_START_AT_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.min);
      } else if (strcmp(token, "TEST_START_AT_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.max);

      } else if (strcmp(token, "TEST_END_AT") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.end_at.value);
      } else if (strcmp(token, "TEST_END_AT_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.end_at.min);
      } else if (strcmp(token, "TEST_END_AT_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.end_at.max);


      } else if (strcmp(token, "TEST_CUR_STATE_ELAPSED") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.cur_state_elapsed.value);
      } else if (strcmp(token, "TEST_CUR_STATE_ELAPSED_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.cur_state_elapsed.min);
      } else if (strcmp(token, "TEST_CUR_STATE_ELAPSED_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.cur_state_elapsed.max);


      } else if (strcmp(token, "TEST_IN_SLEEP") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_sleep.value);
      } else if (strcmp(token, "TEST_IN_SLEEP_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_sleep.min);
      } else if (strcmp(token, "TEST_IN_SLEEP_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_sleep.max);


      } else if (strcmp(token, "TEST_IN_DEEP_SLEEP") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_deep_sleep.value);
      } else if (strcmp(token, "TEST_IN_DEEP_SLEEP_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_deep_sleep.min);
      } else if (strcmp(token, "TEST_IN_DEEP_SLEEP_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.in_deep_sleep.max);

      } else if (strcmp(token, "TEST_FORCE_SHUT_DOWN_AT") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.force_shut_down_at);

      } else if (strcmp(token, "TEST_WEIGHT") == 0) {
        sscanf(token + strlen(token) + 1, "%f", &state->test_entry.weight);
      } else if (strcmp(token, "TEST_NAME") == 0) {
        sscanf(token + strlen(token) + 1, "%s", state->test_entry.name);
      }
    }

    // If this is a "static AlgDlsMinuteData samples[] = {" line, skip it
    if (strcmp(token, "static") == 0) {
      continue;
    }

    // Grab a sample
    if (strcmp(token, "{") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      int steps = 0;
      int orientation = 0;
      int vmc = 0;
      int light = 0;
      int plugged_in = 0;
      if (state->test_entry.version == 1) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d", &steps, &orientation, &vmc);
      } else if (state->test_entry.version == 2) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d, %d", &steps, &orientation, &vmc, &light);
      } else if (state->test_entry.version == 3) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d, %d, %d", &steps, &orientation, &vmc,
               &light, &plugged_in);
      } else {
        cl_assert(false);
      }
      fflush(stdout);

      PBL_ASSERTN(state->test_entry.num_samples < SLEEP_SAMPLES_DISCOVERY_MAX_SAMPLES);
      state->samples[state->test_entry.num_samples++] = (AlgMinuteFileSample) {
        .v5_fields = {
          .steps = steps,
          .orientation = orientation,
          .vmc = vmc,
          .light = light,
          .plugged_in = plugged_in,
        },
      };
      continue;
    }

    // End of a sample
    if (strcmp(token, "}") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      break;
    }
  }

  // Did we get samples?
  if (state->test_entry.num_samples > 0) {
    return true;
  } else {
    return false;
  }
}


// ---------------------------------------------------------------------------------------
static bool prv_parse_activity_samples_file(ActivitySampleDiscoveryState *state) {
  // Init for next set of samples
  state->test_entry = (ActivityFileTestEntry) {
    .samples = state->samples,
    .version = 1,
    .activity_type = {-1, -1, -1},
    .len = {-1, -1, -1},
    .start_at = {-1, -1, -1},
    .weight = 1.0,
    .force_shut_down_at = -1,
  };

  char line_buf[256];
  while (true) {
    char *line = fgets(line_buf, sizeof(line_buf), state->common.file);
    if (!line) {
      // EOF
      break;
    }
    // printf("\nGot line: %s", line);

    // Find first token
    char *token = strtok(line, " \t\n");
    if (!token) {
      continue;
    }

    // If this is a pre-processor directive, skip it
    if (token[0] == '#') {
      continue;
    }

    // If this is a comment skip it
    if (strcmp(token, "//") == 0) {
      continue;
    }

    // If this is an AlgDlsMinuteData line, get the name
    if (strcmp(token, "AlgDlsMinuteData") == 0) {
      PBL_ASSERT(state->test_entry.name[0] == 0, "Unexpected start of new samples");

      token = strtok(NULL, "(");
      // Copy starting from token + 1 to skip the '*' at the front
      strncpy(state->test_entry.name, token + 1, sizeof(state->test_entry.name));
      printf("\nParsing function samples: %s", state->test_entry.name);
      continue;
    }

    // Look for and parse the expected values
    if (strcmp(token, "//>") == 0) {
      token = strtok(NULL, " \t\n");
      if (strcmp(token, "TEST_VERSION") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.version);

      } else if (strcmp(token, "TEST_ACTIVITY_TYPE") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.activity_type.value);
      } else if (strcmp(token, "TEST_ACTIVITY_TYPE_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.activity_type.min);
      } else if (strcmp(token, "TEST_ACTIVITY_TYPE_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.activity_type.max);

      } else if (strcmp(token, "TEST_LEN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.len.value);
      } else if (strcmp(token, "TEST_LEN_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.len.min);
      } else if (strcmp(token, "TEST_LEN_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.len.max);

      } else if (strcmp(token, "TEST_START_AT") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.value);
      } else if (strcmp(token, "TEST_START_AT_MIN") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.min);
      } else if (strcmp(token, "TEST_START_AT_MAX") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.start_at.max);
      } else if (strcmp(token, "TEST_FORCE_SHUT_DOWN_AT") == 0) {
        sscanf(token + strlen(token) + 1, "%d", &state->test_entry.force_shut_down_at);

      } else if (strcmp(token, "TEST_WEIGHT") == 0) {
        sscanf(token + strlen(token) + 1, "%f", &state->test_entry.weight);
      } else if (strcmp(token, "TEST_NAME") == 0) {
        sscanf(token + strlen(token) + 1, "%s", state->test_entry.name);
      }
    }

    // If this is a "static AlgDlsMinuteData samples[] = {" line, skip it
    if (strcmp(token, "static") == 0) {
      continue;
    }

    // Grab a sample
    if (strcmp(token, "{") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      int steps = 0;
      int orientation = 0;
      int vmc = 0;
      int light = 0;
      int plugged_in = 0;
      if (state->test_entry.version == 1) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d", &steps, &orientation, &vmc);
      } else if (state->test_entry.version == 2) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d, %d", &steps, &orientation, &vmc, &light);
      } else if (state->test_entry.version == 3) {
        sscanf(token + strlen(token) + 1, "%d, 0x%x, %d, %d, %d", &steps, &orientation, &vmc,
               &light, &plugged_in);
      } else {
        cl_assert(false);
      }
      fflush(stdout);

      PBL_ASSERTN(state->test_entry.num_samples < SLEEP_SAMPLES_DISCOVERY_MAX_SAMPLES);
      state->samples[state->test_entry.num_samples++] = (AlgMinuteFileSample) {
        .v5_fields = {
          .steps = steps,
          .orientation = orientation,
          .vmc = vmc,
          .light = light,
          .plugged_in = plugged_in,
        },
      };
      continue;
    }

    // End of a sample
    if (strcmp(token, "}") == 0) {
      PBL_ASSERTN(state->test_entry.name[0] != 0);
      break;
    }
  }

  // Did we get samples?
  if (state->test_entry.num_samples > 0) {
    return true;
  } else {
    return false;
  }
}

// ---------------------------------------------------------------------------------------
// Init the sample discovery iterator
static bool prv_sample_discovery_init(SampleDiscoveryState *state, SampleFileType samples_type,
                                      const char *test_files_path) {

  // Free prior one
  if (state->dp) {
    if (state->file) {
      fclose(state->file);
      state->file = NULL;
    }
    closedir(state->dp);
    state->dp = NULL;
    free(state->res_path);
  }

  // Open up the directory
  state->res_path = malloc(strlen(CLAR_FIXTURE_PATH) + strlen(test_files_path) + 2);
  sprintf(state->res_path, "%s/%s", CLAR_FIXTURE_PATH, test_files_path);
  state->dp = opendir(state->res_path);

  if (state->dp == NULL) {
    printf("\nCould not open directory %s", state->res_path);
    return false;
  }

  state->type = samples_type;
  return true;
}


// ---------------------------------------------------------------------------------------
// Advance to the next file in the directory. Return true if successful
static bool prv_sample_discovery_next_file(SampleDiscoveryState *state) {
  if (state->dp == NULL) {
    return false;
  }

  while (!state->file) {
    state->ep = readdir(state->dp);
    if (!state->ep) {
      // No more files
      return false;
    }

    // See if it's the right extension
    int name_len = strlen(state->ep->d_name);
    if (name_len < 3 || (strcmp(state->ep->d_name + name_len - 2, ".c") != 0)) {
      continue;
    }

    // Open up the file
    printf("\n\n\n\nParsing file: %s", state->ep->d_name);
    char file_path[strlen(state->res_path) + strlen(state->ep->d_name) + 1];
    sprintf(file_path, "%s/%s", state->res_path, state->ep->d_name);

    state->file = fopen(file_path, "r");
    if (!state->file) {
      printf("\nFile %s could not be opened", file_path);
      continue;
    }
  }
  return true;
}


// ---------------------------------------------------------------------------------------
// Return info on the next set of samples
static bool prv_accel_sample_discovery_next(StepFileTestEntry *entry) {
  AccelSampleDiscoveryState *state = &s_accel_sample_discovery_state;

  while (true) {
    // Read next entry if necessary
    if (!state->common.file) {
      if (!prv_sample_discovery_next_file(&state->common)) {
        return false;
      }
    }

    // Parse next set of samples in the current file
    bool success = prv_parse_accel_samples_file(state);
    if (success) {
      *entry = state->test_entry;
      return true;
    } else {
      // No more in this file
      fclose(state->common.file);
      state->common.file = NULL;
    }
  }
}


// ---------------------------------------------------------------------------------------
// Return info on the next set of samples
static bool prv_sleep_sample_discovery_next(SleepFileTestEntry *entry) {
  SleepSampleDiscoveryState *state = &s_sleep_sample_discovery_state;

  while (true) {
    // Read next entry if necessary
    if (!state->common.file) {
      if (!prv_sample_discovery_next_file(&state->common)) {
        return false;
      }
    }

    // Parse next set of samples in the current file
    bool success = prv_parse_sleep_samples_file(state);
    if (success) {
      *entry = state->test_entry;
      return true;
    } else {
      // No more in this file
      fclose(state->common.file);
      state->common.file = NULL;
    }
  }
}


// ---------------------------------------------------------------------------------------
// Return info on the next set of samples
static bool prv_activity_sample_discovery_next(ActivityFileTestEntry *entry) {
  ActivitySampleDiscoveryState *state = &s_activity_sample_discovery_state;

  while (true) {
    // Read next entry if necessary
    if (!state->common.file) {
      if (!prv_sample_discovery_next_file(&state->common)) {
        return false;
      }
    }

    // Parse next set of samples in the current file
    bool success = prv_parse_activity_samples_file(state);
    if (success) {
      *entry = state->test_entry;
      return true;
    } else {
      // No more in this file
      fclose(state->common.file);
      state->common.file = NULL;
    }
  }
}


// --------------------------------------------------------------------------------------
// Callback provided to the qsort() routine for sorting step tests by name
int prv_qsort_step_test_entry_cb(const void *a, const void *b) {
  StepFileTestEntry *entry_a = (StepFileTestEntry *)a;
  StepFileTestEntry *entry_b = (StepFileTestEntry *)b;

  // Put the non-walking samples at the end
  if (entry_a->exp_steps > 0 && entry_b->exp_steps == 0) {
    return -1;
  } else if (entry_a->exp_steps == 0 && entry_b->exp_steps > 0) {
    return +1;
  } else {
    return strcmp(entry_a->name, entry_b->name);
  }
}


// --------------------------------------------------------------------------------------
// Callback provided to the qsort() routine for sorting sleep tests by name
int prv_qsort_sleep_test_entry_cb(const void *a, const void *b) {
  SleepFileTestEntry *entry_a = (SleepFileTestEntry *)a;
  SleepFileTestEntry *entry_b = (SleepFileTestEntry *)b;

  return strcmp(entry_a->name, entry_b->name);
}

// --------------------------------------------------------------------------------------
// Callback provided to the qsort() routine for sorting activity tests by name
int prv_qsort_activity_test_entry_cb(const void *a, const void *b) {
  ActivityFileTestEntry *entry_a = (ActivityFileTestEntry *)a;
  ActivityFileTestEntry *entry_b = (ActivityFileTestEntry *)b;

  return strcmp(entry_a->name, entry_b->name);
}



// =============================================================================================
// Support for capturing activity sessions detected by the algorithm
typedef struct {
  uint16_t steps;
  uint32_t resting_calories;
  uint32_t active_calories;
  uint32_t distance_mm;
} KAlgTestActivityMinute;

#define MAX_CAPTURED_SESSIONS 32
KAlgTestActivitySession s_captured_activity_sessions[MAX_CAPTURED_SESSIONS];
int s_num_captured_activity_sessions;
void prv_activity_session_callback(void *context, KAlgActivityType activity_type,
                                   time_t start_utc, uint32_t len_sec, bool ongoing, bool delete,
                                   uint32_t steps, uint32_t resting_calories,
                                   uint32_t active_calories, uint32_t distance_mm) {
  int entry_idx = s_num_captured_activity_sessions;
  // Ignore sleep activities for this test
  if ((activity_type == KAlgActivityType_Sleep)
      || (activity_type == KAlgActivityType_RestfulSleep)) {
    return;
  }

  // If this activity already exists, update it
  KAlgTestActivitySession *session = s_captured_activity_sessions;
  for (int i = 0; i < s_num_captured_activity_sessions; i++, session++) {
    if (session->start_utc == start_utc && session->activity == activity_type) {
      entry_idx = i;
      break;
    }
  }

  if (delete && (s_num_captured_activity_sessions > 0)) {
    if (entry_idx == s_num_captured_activity_sessions) {
      return;
    }
    int num_to_move = s_num_captured_activity_sessions - entry_idx - 1;
    memmove(&s_captured_activity_sessions[entry_idx], &s_captured_activity_sessions[entry_idx + 1],
            num_to_move * sizeof(KAlgTestActivitySession));
    s_num_captured_activity_sessions--;
  }

  cl_assert(entry_idx < MAX_CAPTURED_SESSIONS);
  s_captured_activity_sessions[entry_idx] = (KAlgTestActivitySession) {
    .activity = activity_type,
    .len_minutes = len_sec / SECONDS_PER_MINUTE,
    .start_utc = start_utc,
    .ongoing = ongoing,
    .steps = steps,
    .active_calories = active_calories,
    .resting_calories = resting_calories,
    .distance_mm = distance_mm,
  };

  printf("\nAdded new activity: %d, start_utc: %d, len_m: %d", (int)activity_type,
         (int)start_utc, (int)len_sec / SECONDS_PER_MINUTE);
  if (entry_idx == s_num_captured_activity_sessions) {
    s_num_captured_activity_sessions++;
  }
}



// ----------------------------------------------------------------------------------------
// Print a timestamp in a format useful for log messages (for debugging). This only prints
// the hour and minute: HH:MM
static const char* prv_log_time(time_t utc) {
  static char time_str[8];
  int minutes = (utc / SECONDS_PER_MINUTE) % MINUTES_PER_HOUR;
  int hours = (utc / SECONDS_PER_HOUR) % HOURS_PER_DAY;

  snprintf(time_str, sizeof(time_str), "%02d:%02d", hours, minutes);
  return time_str;
}


// =============================================================================================
// Start of unit tests
void test_kraepelin_algorithm__initialize(void) {
}


// ---------------------------------------------------------------------------------------
void test_kraepelin_algorithm__cleanup(void) {
}


// ---------------------------------------------------------------------------------------
void test_kraepelin_algorithm__step_tests(void) {
  bool success = prv_sample_discovery_init(&s_accel_sample_discovery_state.common,
                                           SampleFileType_AccelSamples, "activity/step_samples");
  cl_assert(success);

  const uint32_t k_max_tests = 1000;
  uint32_t num_tests = 0;

  // Results
  typedef struct {
    int steps;
    int ref_steps;
    uint32_t test_idx;
  } StepTestResults;
  StepTestResults test_results[k_max_tests];
  StepFileTestEntry test_entry[k_max_tests];

  while (prv_accel_sample_discovery_next(&test_entry[num_tests])) {
    StepFileTestEntry *entry = &test_entry[num_tests];

#ifdef STEP_TEST_ONLY
    if (strcmp(entry->name, STEP_TEST_ONLY)) {
      continue;
    }
#endif
    entry->test_idx = num_tests;

    printf("\n\n========================================================");
    printf("\nRunning sample set: \"%s\"\n", entry->name);

    // Run the step algorithm
    int minute_data_len = 100;
    TestMinuteData minute_data[minute_data_len];
    prv_stats_reinit();
    int steps = prv_feed_kalg_samples(entry->samples, entry->num_samples, minute_data,
                                      &minute_data_len);
    // Save stats to file
#ifdef STATS_FILE_NAME
    prv_stats_write(STATS_FILE_NAME, (num_tests == 0) /*create*/, entry->name,
                    (entry->exp_steps != 0) /*stepping*/);
#endif


    // Run through reference code
    int ref_steps = prv_feed_reference_samples(entry->samples, entry->num_samples);
    //int ref_steps = -1;

    int error = abs(steps - entry->exp_steps);
    float weighted_error = (float)error * entry->weight;
    printf("\nRESULTS: exp_steps: %d, act_steps: %d, ref_steps: %d, error: %d, weighted_error: %f",
           entry->exp_steps, (int)steps, (int)ref_steps, (int)error, weighted_error);
    printf("\n         min: (steps, vmc, orientation)");
    for (int j = 0; j < minute_data_len; j++) {
      printf("\n                %-4d  %-4d  0x%-4x", (int)minute_data[j].steps,
             (int)minute_data[j].vmc, (int)minute_data[j].orientation);
    }
    test_results[num_tests] = (StepTestResults) {
      .steps = steps,
      .ref_steps = ref_steps,
      .test_idx = num_tests,
    };

    num_tests++;
    if (num_tests >= k_max_tests) {
      printf("RAN INTO MAX NUMBER OF TESTS WE SUPPORT");
      break;
    }
  }

  // Make sure we discovered at least 1 test
  cl_assert(num_tests > 0);

  // Let's sort the tests by name
  qsort(&test_entry[0], num_tests, sizeof(test_entry[0]), prv_qsort_step_test_entry_cb);

  // ------------------------------------------------------------------------------------------
  // Print summery of results
  printf("\n\n");
  printf("\n%-40s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s",
         "name",
         "exp_steps",
         "act_steps",
         "error",
         "min",
         "max",
         "ref_steps",
         "weight_err",
         "status");
  printf("\n---------------------------------------------------------------------------------"
         "-----------------------------");

  float weighted_sum = 0.0;
  int pass_count = 0;
  int fail_count = 0;
  StepFileTestEntry *entry = &test_entry[0];
  StepTestResults *results;
  for (int i = 0; i < num_tests; i++, entry++) {
    results = &test_results[entry->test_idx];
    cl_assert_equal_i(results->test_idx, entry->test_idx);
    int error = results->steps - entry->exp_steps;
    float weighted_error = (float)abs(error) * entry->weight;
    weighted_sum += weighted_error;
    char *status;
    if (results->steps < entry->exp_steps_min || results->steps > entry->exp_steps_max) {
      status = "FAIL";
      fail_count++;
    } else {
      status = "pass";
      pass_count++;
    }
    printf("\n%-40s %-10d %-10d %-10d %-10d %-10d %-10d %-10.2f %-10s",
           entry->name,
           entry->exp_steps,
           results->steps,
           error,
           entry->exp_steps_min,
           entry->exp_steps_max,
           results->ref_steps,
           weighted_error,
           status);
  }

  if (fail_count) {
    printf("\n\ntest FAILED: %d failures, Avg weighted error: %.2f", fail_count,
           weighted_sum / num_tests);
  } else {
    printf("\n\ntest PASSED! Avg weighted error: %.2f", weighted_sum / num_tests);
  }

  cl_assert_equal_i(fail_count, 0);
}


// ---------------------------------------------------------------------------------------
static const char *prv_status_str(bool passed) {
    if (!passed) {
      return "FAIL";
    } else {
      return "pass";
    }
}


// ---------------------------------------------------------------------------------------
// Returns the weighted error of this test
static float prv_compute_test_error(const char *name, ExpectedValue *exp, ActualValue *act,
                                    float weight, bool *all_passed) {
  int error = 0;
  float weighted_error = 0.0;

  if (exp->value != -1) {
    error = abs(act->value - exp->value);
    weighted_error = (float)error * weight;

    act->passed = (act->value >= exp->min && act->value <= exp->max);
    if (!act->passed) {
      *all_passed = false;
    }
    printf("\nRESULTS for %s: exp: (%d,%d), act: %d, error: %d, weighted_error: %f, %s",
         name, (int)exp->min, (int)exp->max, (int)act->value, (int)error, weighted_error,
         prv_status_str(act->passed));
  } else {
    act->passed = true;
    printf("\nRESULTS for %s: exp: (NA), act: %d, error: NA, weighted_error: NA",
         name, (int)act->value);
  }

  return weighted_error;
}


// =========================================================================================
// Support for capturing sleep sessions detected by the algorithm
typedef struct {
  KAlgActivityType activity;
  time_t start_utc;
  uint16_t len_m;
} KAlgTestSleepSession;

KAlgTestSleepSession s_captured_sleep_sessions[MAX_CAPTURED_SESSIONS];
int s_num_captured_sleep_sessions;
void prv_sleep_session_callback(void *context, KAlgActivityType activity_type,
                                time_t start_utc, uint32_t len_sec, bool ongoing, bool delete,
                                uint32_t steps, uint32_t resting_calories, uint32_t active_calories,
                                uint32_t distance_mm) {

  int entry_idx = s_num_captured_sleep_sessions;

  // If not a sleep session, ignore it
  if (activity_type != KAlgActivityType_Sleep && activity_type != KAlgActivityType_RestfulSleep) {
    return;
  }

  // Look for an existing session
  KAlgTestSleepSession *session = s_captured_sleep_sessions;
  for (int i = 0; i < s_num_captured_sleep_sessions; i++, session++) {
    if (session->start_utc == start_utc && session->activity == activity_type) {
      entry_idx = i;
      break;
    }
  }
  cl_assert(entry_idx < MAX_CAPTURED_SESSIONS);

  // Deleting?
  if (delete) {
    if (entry_idx < s_num_captured_sleep_sessions) {
      int num_to_move = s_num_captured_sleep_sessions - entry_idx - 1;
      cl_assert(num_to_move >= 0);
      memmove(&s_captured_sleep_sessions[entry_idx], &s_captured_sleep_sessions[entry_idx + 1],
              num_to_move * sizeof(KAlgTestSleepSession));
      s_num_captured_sleep_sessions--;
    }
    return;
  }

  // Update/add session
  s_captured_sleep_sessions[entry_idx] = (KAlgTestSleepSession) {
    .activity = activity_type,
    .len_m = len_sec / SECONDS_PER_MINUTE,
    .start_utc = start_utc,
  };

  if (entry_idx == s_num_captured_sleep_sessions) {
    s_num_captured_sleep_sessions++;
  }
}


// --------------------------------------------------------------------------------------------
// Collect summary sleep information from a collection of sessions
static void prv_get_sleep_summary(SleepTestResults *results, time_t test_start_utc,
                                  time_t test_end_utc, time_t last_processed_utc) {
  *results = (SleepTestResults) { };

  // Iterate through the sleep sessions
  KAlgTestSleepSession *session = s_captured_sleep_sessions;
  time_t enter_utc = 0;
  time_t exit_utc = 0;
  time_t deep_exit_utc = 0;
  uint16_t last_session_len_m = 0;
  uint16_t last_deep_session_len_m = 0;
  bool first_container = true;
  KAlgTestSleepSession *container_session = NULL;
  for (uint32_t i = 0; i < s_num_captured_sleep_sessions; i++, session++) {

    // Get info on this session
    time_t session_exit_utc = session->start_utc + session->len_m * SECONDS_PER_MINUTE;

    // Skip if not a sleep session
    bool is_restful = false;
    switch (session->activity) {
      case KAlgActivityType_Sleep:
        break;
      case KAlgActivityType_RestfulSleep:
        is_restful = true;
        break;
      default:
        continue;
    }

    const char *desc = is_restful ? " restful" : "sleep";
    printf("\nfound %s session: len: %"PRIu16" min., start: %s", desc, session->len_m,
           prv_log_time(session->start_utc));

    if (!is_restful) {
      container_session = session;
      last_session_len_m = session->len_m;

      // Accumulate sleep container stats
      results->total.value += session->len_m;
      if (first_container || session->start_utc < enter_utc) {
        enter_utc = session->start_utc;
      }
      if (first_container || session_exit_utc > exit_utc) {
        exit_utc = session_exit_utc;
      }
      first_container = false;

    } else {
      // Insure that restful sessions are inside the previous container
      cl_assert(container_session != NULL);
      cl_assert(session->start_utc >= container_session->start_utc);
      cl_assert(session->start_utc < container_session->start_utc
                                     + container_session->len_m * SECONDS_PER_MINUTE);
      last_deep_session_len_m = session->len_m;
      // Accumulate restful sleep stats
      results->deep.value += session->len_m;
      if (deep_exit_utc == 0 || session_exit_utc > deep_exit_utc) {
        deep_exit_utc = session_exit_utc;
      }
    }
  }

  // Fill in the rest of the sleep data metrics
  if (enter_utc != 0) {
    results->start_at.value = (enter_utc - test_start_utc) / SECONDS_PER_MINUTE;
  }
  if (exit_utc != 0) {
    results->end_at.value = (exit_utc - test_start_utc) / SECONDS_PER_MINUTE;
  }


  // Figure out our current state
  if (exit_utc >= last_processed_utc - SECONDS_PER_MINUTE) {
    // We are sleeping
    results->in_sleep.value = true;
    int unprocessed_m = (test_end_utc - last_processed_utc) / SECONDS_PER_MINUTE;
    if (exit_utc == deep_exit_utc) {
      results->in_deep_sleep.value = true;
      results->cur_state_elapsed.value = last_deep_session_len_m + unprocessed_m;
    } else {
      results->cur_state_elapsed.value = last_session_len_m + unprocessed_m;
    }
  } else {
    if (exit_utc != 0) {
      results->cur_state_elapsed.value = (test_end_utc - exit_utc) / SECONDS_PER_MINUTE;
    } else {
      results->cur_state_elapsed.value = (test_end_utc - test_start_utc) / SECONDS_PER_MINUTE;
    }
  }
}


// --------------------------------------------------------------------------------------
// Run a set of samples through and verify that we got the right minute data
static void prv_test_minute_data(AccelRawData *samples, int num_samples,
                                 TestMinuteData *exp_minutes, int exp_num_minutes) {
  int minute_data_len = 100;
  TestMinuteData minute_data[minute_data_len];

  // Run the step algorithm
  prv_feed_kalg_samples(samples, num_samples, minute_data, &minute_data_len);

  for (int i = 0; i < minute_data_len; i++) {
    printf("\n  %-4d  0x%-4x %-4d", (int)minute_data[i].steps,
           (int)minute_data[i].orientation, (int)minute_data[i].vmc);
  }
  printf("\n");

  // Verify that we got the expected minute data
  cl_assert_equal_i(minute_data_len, exp_num_minutes);
  for (int j = 0; j < minute_data_len; j++) {
    cl_assert_equal_i(minute_data[j].steps, exp_minutes[j].steps);
    cl_assert_equal_i(minute_data[j].orientation, exp_minutes[j].orientation);
    cl_assert_equal_i(minute_data[j].vmc, exp_minutes[j].vmc);
  }

}


// ---------------------------------------------------------------------------------------
void test_kraepelin_algorithm__sleep_tests(void) {
  bool success = prv_sample_discovery_init(&s_sleep_sample_discovery_state.common,
                                           SampleFileType_MinuteSamples,
                                           "activity/sleep_samples");
  cl_assert(success);

  // Init algorithm state
  s_kalg_state = kernel_zalloc(kalg_state_size());

  const uint32_t k_max_tests = 1000;
  uint32_t num_tests = 0;

  // Results
  SleepTestResults test_results[k_max_tests];
  memset(test_results, 0, sizeof(test_results));

  // List of metrics we measure for each test
  // IMPORTANT: This order must match the order in the SleepTestEntry and the SleepTestResults
  const char *metrics[] = {"total", "deep", "start", "end", "elapsed", "insleep",
                           "indeep"};

  SleepFileTestEntry test_entry[k_max_tests];
  memset(test_entry, 0, sizeof(test_entry));
  while (prv_sleep_sample_discovery_next(&test_entry[num_tests])) {
    SleepFileTestEntry *entry = &test_entry[num_tests];

#ifdef SLEEP_TEST_ONLY
    if (strcmp(entry->name, SLEEP_TEST_ONLY)) {
      continue;
    }
#endif
    entry->test_idx = num_tests;

    printf("\n\n========================================================");
    printf("\nRunning sleep sample set: \"%s\"\n", entry->name);

    // It's easier to understand the algorithm log messages if we start at 0 time
    rtc_set_time(0);
    memset(s_kalg_state, 0, kalg_state_size());
    kalg_init(s_kalg_state, prv_stats_cb);
    s_num_captured_sleep_sessions = 0;

    // Run samples through the activity detector
    time_t now = rtc_get_time();
    time_t test_start_utc = now;
    for (int i = 0; i < entry->num_samples; i++) {
      uint16_t vmc = entry->samples[i].v5_fields.vmc;
      if (entry->version == 1) {
        // Convert from the old compressed VMC to the new uncompressed one
        vmc = vmc * vmc * 1850 / 1250;
      }
      const bool shutting_down = (entry->force_shut_down_at == i);
      kalg_activities_update(s_kalg_state, now, entry->samples[i].v5_fields.steps, vmc,
                             entry->samples[i].v5_fields.orientation,
                             entry->samples[i].v5_fields.plugged_in,
                             0 /*rest_cals*/, 0 /*active_cals*/, 0 /*distance*/, shutting_down,
                             prv_sleep_session_callback, NULL);
      if (shutting_down) {
        break;
      }

      now += SECONDS_PER_MINUTE;
      rtc_set_time(now);
    }
    time_t test_end_utc = now;
    time_t last_processed_utc = kalg_activity_last_processed_time(s_kalg_state,
                                                                  KAlgActivityType_Sleep);

    // Get summary of the sleep
    SleepTestResults result = { };
    prv_get_sleep_summary(&result, test_start_utc, test_end_utc, last_processed_utc);
    result.weighted_err = 0.0;
    result.all_passed = true;

    ActualValue *actual = &result.total;
    ExpectedValue *expected = &entry->total;
    for (int j = 0; j < ARRAY_LENGTH(metrics); j++, actual++, expected++) {
      result.weighted_err += prv_compute_test_error(
        metrics[j], expected, actual, entry->weight, &result.all_passed);
    }

    test_results[num_tests] = result;
    num_tests++;
    if (num_tests >= k_max_tests) {
      printf("RAN INTO MAX NUMBER OF TESTS WE SUPPORT");
      break;
    }
  }

  // Make sure we discovered at least 1 test
  cl_assert(num_tests > 0);

  // Let's sort the tests by name
  qsort(&test_entry[0], num_tests, sizeof(test_entry[0]), prv_qsort_sleep_test_entry_cb);

  // ---------------------------------------------------------------------------------
  // Print results in a table
  printf("\n\n");
  printf("\n%-24s", "name");

  // Print header line
  for (int i = 0; i < ARRAY_LENGTH(metrics); i++) {
    printf(" exp_%-8s act_%-7s", metrics[i], metrics[i]);
  }
  printf(" %-10s %-10s", "weight_err", "status");

  printf("\n------------------------");
  for (int i = 0; i < ARRAY_LENGTH(metrics); i++) {
    printf("| ---------------------- ");
  }

  float weighted_sum = 0.0;
  int pass_count = 0;
  int fail_count = 0;
  SleepFileTestEntry *entry = &test_entry[0];
  SleepTestResults *results;
  for (int i = 0; i < num_tests; i++, entry++, results++) {
    results = &test_results[entry->test_idx];

    // Generate the status string
    const char *status = prv_status_str(results->all_passed);
    if (results->all_passed) {
      pass_count++;
    } else {
      fail_count++;
    }

    // Print name of test
    printf("\n%-24s", entry->name);

    // Print each metric for this test
    ActualValue *actual = &results->total;
    ExpectedValue *expected = &entry->total;
    for (int j = 0; j < ARRAY_LENGTH(metrics); j++, actual++, expected++) {
      char *indicator;
      if (actual->passed) {
        indicator = "  ";
      } else {
        indicator = "**";
      }
      if (expected->value != -1) {
        int delta = actual->value - expected->value;
        printf(" (%3d,%3d)  %s%3d (%+4d) ", expected->min, expected->max, indicator, actual->value,
               delta);
      } else {
        printf(" (NA, NA )  %s%3d        ", indicator, actual->value);
      }
    }

    printf(" %-10.2f %-10s", results->weighted_err, status);
    weighted_sum += results->weighted_err;
  }

  // ---------------------------------------------------------------------------------
  // Overall Summary
  if (fail_count) {
    printf("\n\ntest FAILED: %d failures, Avg weighted error: %.2f", fail_count,
           weighted_sum / num_tests);
  } else {
    printf("\n\ntest PASSED! Avg weighted error: %.2f", weighted_sum / num_tests);
  }

  cl_assert_equal_i(fail_count, 0);
  kernel_free(s_kalg_state);
  s_kalg_state = NULL;
}


// ---------------------------------------------------------------------------------------
void test_kraepelin_algorithm__activity_tests(void) {
  bool success = prv_sample_discovery_init(&s_activity_sample_discovery_state.common,
                                           SampleFileType_MinuteSamples,
                                           "activity/activity_samples");
  cl_assert(success);

  // Init algorithm state
  s_kalg_state = kernel_zalloc(kalg_state_size());

  const uint32_t k_max_tests = 1000;
  uint32_t num_tests = 0;

  // Results
  ActivityTestResults test_results[k_max_tests];
  memset(test_results, 0, sizeof(test_results));

  // List of metrics we measure for each test
  // IMPORTANT: This order must match the order in the SleepTestEntry and the SleepTestResults
  const char *metrics[] = {"type", "len", "start"};

  ActivityFileTestEntry test_entry[k_max_tests];
  memset(test_entry, 0, sizeof(test_entry));
  while (prv_activity_sample_discovery_next(&test_entry[num_tests])) {
    ActivityFileTestEntry *entry = &test_entry[num_tests];

#ifdef ACTIVITY_TEST_ONLY
    if (strcmp(entry->name, ACTIVITY_TEST_ONLY)) {
      continue;
    }
#endif
    entry->test_idx = num_tests;

    printf("\n\n========================================================");
    printf("\nRunning activity sample set: \"%s\"\n", entry->name);

    memset(s_kalg_state, 0, kalg_state_size());
    kalg_init(s_kalg_state, prv_stats_cb);
    s_num_captured_activity_sessions = 0;

    // Run samples through the activity detector
    time_t now = rtc_get_time();
    time_t test_start_utc = now;
    for (int i = 0; i < entry->num_samples; i++) {
      const bool shutting_down = (entry->force_shut_down_at == i);
      kalg_activities_update(s_kalg_state, now, entry->samples[i].v5_fields.steps, 0 /*vmc*/,
                             0 /*orientation*/, false /*plugged_in*/, 0 /*rest_cals*/,
                             0 /*active_cals*/, 0 /*distance*/, shutting_down,
                             prv_activity_session_callback, NULL);
      if (shutting_down) {
        break;
      }

      now += SECONDS_PER_MINUTE;
      rtc_set_time(now);
    }

    // Get summary of the activity
    ActivityTestResults result = { };
    KAlgTestActivitySession *session = s_captured_activity_sessions;
    bool found_activity = false;
    for (uint32_t i = 0; i < s_num_captured_activity_sessions; i++, session++) {
      // Skip if this is a sleep session
      char *desc = "";
      switch (session->activity) {
        case KAlgActivityType_Sleep:
        case KAlgActivityType_RestfulSleep:
          continue;
        case KAlgActivityType_Walk:
          desc = "walk";
          break;
        case KAlgActivityType_Run:
          desc = "run";
          break;
        case KAlgActivityTypeCount:
          WTF;
          break;
      }

      int start_idx = (session->start_utc - test_start_utc) / SECONDS_PER_MINUTE;
      printf("\nfound %s len: %d, start: %d, ", desc, (int) session->len_minutes,
             start_idx);

      // Only compare the first activity found
      if (!found_activity) {
        result.activity_type.value = (int)session->activity;
        result.len.value = (int)session->len_minutes;
        result.start_at.value = start_idx;
        found_activity = true;
      }
    }

    result.weighted_err = 0.0;
    result.all_passed = true;

    ActualValue *actual = &result.activity_type;
    ExpectedValue *expected = &entry->activity_type;
    for (int j = 0; j < ARRAY_LENGTH(metrics); j++, actual++, expected++) {
      result.weighted_err += prv_compute_test_error(
        metrics[j], expected, actual, entry->weight, &result.all_passed);
    }

    test_results[num_tests] = result;
    num_tests++;
    if (num_tests >= k_max_tests) {
      printf("RAN INTO MAX NUMBER OF TESTS WE SUPPORT");
      break;
    }
  }

  // Make sure we discovered at least 1 test
  cl_assert(num_tests > 0);

  // Let's sort the tests by name
  qsort(&test_entry[0], num_tests, sizeof(test_entry[0]), prv_qsort_activity_test_entry_cb);

  // ---------------------------------------------------------------------------------
  // Print results in a table
  printf("\n\n");
  printf("\n%-24s", "name");

  // Print header line
  for (int i = 0; i < ARRAY_LENGTH(metrics); i++) {
    printf(" exp_%-8s act_%-7s", metrics[i], metrics[i]);
  }
  printf(" %-10s %-10s", "weight_err", "status");

  printf("\n------------------------");
  for (int i = 0; i < ARRAY_LENGTH(metrics); i++) {
    printf("| ---------------------- ");
  }

  float weighted_sum = 0.0;
  int pass_count = 0;
  int fail_count = 0;
  ActivityFileTestEntry *entry = &test_entry[0];
  ActivityTestResults *results;
  for (int i = 0; i < num_tests; i++, entry++, results++) {
    results = &test_results[entry->test_idx];

    // Generate the status string
    const char *status = prv_status_str(results->all_passed);
    if (results->all_passed) {
      pass_count++;
    } else {
      fail_count++;
    }

    // Print name of test
    printf("\n%-24s", entry->name);

    // Print each metric for this test
    ActualValue *actual = &results->activity_type;
    ExpectedValue *expected = &entry->activity_type;
    for (int j = 0; j < ARRAY_LENGTH(metrics); j++, actual++, expected++) {
      char *indicator;
      if (actual->passed) {
        indicator = "  ";
      } else {
        indicator = "**";
      }
      if (expected->value != -1) {
        int delta = actual->value - expected->value;
        printf(" (%3d,%3d)  %s%3d (%+4d) ", expected->min, expected->max, indicator, actual->value,
               delta);
      } else {
        printf(" (NA, NA )  %s%3d        ", indicator, actual->value);
      }
    }

    printf(" %-10.2f %-10s", results->weighted_err, status);
    weighted_sum += results->weighted_err;
  }

  // ---------------------------------------------------------------------------------
  // Overall Summary
  if (fail_count) {
    printf("\n\ntest FAILED: %d failures, Avg weighted error: %.2f", fail_count,
           weighted_sum / num_tests);
  } else {
    printf("\n\ntest PASSED! Avg weighted error: %.2f", weighted_sum / num_tests);
  }

  cl_assert_equal_i(fail_count, 0);
  kernel_free(s_kalg_state);
  s_kalg_state = NULL;
}


// ---------------------------------------------------------------------------------------
// Test that we generate the right minute statistics
void test_kraepelin_algorithm__minute_stats(void) {

  // Run the 30 step sample.
  // The expected results were obtained empirically on a known good commit
  {
    int num_samples;
    AccelRawData *samples = activity_sample_30_steps(&num_samples);
    TestMinuteData exp_minutes[] = {
      {
        .steps = 28,
        .orientation = 0x47,
        .vmc = 1205,
      },
    };
    prv_test_minute_data(samples, num_samples, exp_minutes, ARRAY_LENGTH(exp_minutes));
  }

  // Run the working at desk sample
  // The expected results were obtained empirically on a known good commit
  {
    int num_samples;
    AccelRawData *samples = activity_sample_working_at_desk(&num_samples);
    TestMinuteData exp_minutes[] = {
      {
        .steps = 0,
        .orientation = 0x72,
        .vmc = 1787,
      },
    };
    prv_test_minute_data(samples, num_samples, exp_minutes, ARRAY_LENGTH(exp_minutes));
  }

  // Run the not moving sample
  // The expected results were obtained empirically on a known good commit
  {
    int num_samples;
    AccelRawData *samples = activity_sample_not_moving(&num_samples);
    TestMinuteData exp_minutes[2] = {
      {
        .steps = 0,
        .orientation = 0x81,
        .vmc = 181,
      },
      {
        .steps = 0,
        .orientation = 0x81,
        .vmc = 0,
      },
    };
    prv_test_minute_data(samples, num_samples, exp_minutes, ARRAY_LENGTH(exp_minutes));
  }
}


// ---------------------------------------------------------------------------------------
// Utility for feeding in artificial walk/run activity samples into the algorithm's
// activity detector logic
static void prv_insert_artificial_activity_session(KAlgTestActivityMinute *samples, int samples_len,
                                                   KAlgTestActivitySession *session) {
  time_t now = rtc_get_time();
  int start_idx = ((session->start_utc - now) / SECONDS_PER_MINUTE) + 1;
  int len = session->len_minutes;

  cl_assert(start_idx + len < samples_len);

  for (int i = start_idx; i < start_idx + len; i++) {
    samples[i] = (KAlgTestActivityMinute) {
      .steps = session->steps / len,
      .active_calories = session->active_calories / len,
      .resting_calories = session->resting_calories / len,
      .distance_mm = session->distance_mm / len,
    };
  }
}


// -----------------------------------------------------------------------------------
// Feed activity minute data into the kalg_activities_update method
static void prv_feed_activity_minutes(KAlgTestActivityMinute *samples, int samples_len) {
  time_t now = rtc_get_time();
  for (int i = 0; i < samples_len; i++) {
    // NOTE: We feed in a significant VMC to simulate activity so that the sleep algorithm
    // doesn't think we're sleeping
    kalg_activities_update(s_kalg_state, now, samples[i].steps, 7000 /*vmc*/, 0 /*orientation*/,
                           true /*plugged_in*/, samples[i].resting_calories,
                           samples[i].active_calories, samples[i].distance_mm, false /* shutting_down */,
                           prv_activity_session_callback, NULL);
    now += SECONDS_PER_MINUTE;
    rtc_set_time(now);
  }
}

// ---------------------------------------------------------------------------------------
// Test that we correctly recognize walk and run activities
void test_kraepelin_algorithm__walks_and_runs(void) {
  const int k_minute_data_len = 60;
  const int k_minute_data_bytes = k_minute_data_len * sizeof(KAlgTestActivityMinute);

  // Init  state
  s_kalg_state = kernel_zalloc(kalg_state_size());
  kalg_init(s_kalg_state, prv_stats_cb);

  KAlgTestActivityMinute minute_raw_data[k_minute_data_len];


  // --------------------------------------------------------------------------------------
  // Test a walk session of 20 minutes long that starts 10 minutes in
  {
    memset(minute_raw_data, 0, k_minute_data_bytes);
    s_num_captured_activity_sessions = 0;
    time_t now = rtc_get_time();

    int len = 20;
    KAlgTestActivitySession exp_session = {
      .activity = KAlgActivityType_Walk,
      .start_utc = now + 10 * SECONDS_PER_MINUTE,
      .steps = len * 80, // 80 steps/min
      .len_minutes = len,
      .resting_calories = len * 100,
      .active_calories = len * 200,
      .distance_mm = len * 1000,
    };

    prv_insert_artificial_activity_session(minute_raw_data, k_minute_data_len, &exp_session);
    prv_feed_activity_minutes(minute_raw_data, k_minute_data_len);
    cl_assert_equal_i(s_num_captured_activity_sessions, 1);
    ASSERT_ACTIVITY_SESSION_PRESENT(s_captured_activity_sessions, s_num_captured_activity_sessions, &exp_session);
  }

  // --------------------------------------------------------------------------------------
  // Test a run session of 30 minutes long that starts 10 minutes in that has a 2 minute
  //  gap in the middle
  {
    memset(minute_raw_data, 0, k_minute_data_bytes);
    s_num_captured_activity_sessions = 0;
    time_t now = rtc_get_time();

    int len = 30;
    KAlgTestActivitySession exp_session = {
      .activity = KAlgActivityType_Run,
      .start_utc = now + 10 * SECONDS_PER_MINUTE,
      .steps = len * 150, // 150 steps/min
      .len_minutes = len,
      .resting_calories = len * 100,
      .active_calories = len * 200,
      .distance_mm = len * 1000,
    };

    prv_insert_artificial_activity_session(minute_raw_data, k_minute_data_len, &exp_session);
    // Insert a 3 minute rest period in the middle
    for (int i = 20; i < 23; i++) {
      minute_raw_data[i] = (KAlgTestActivityMinute) { };
    }
    exp_session.steps -= 3 * 150;
    exp_session.resting_calories -= 3 * 100;
    exp_session.active_calories -= 3 * 200;
    exp_session.distance_mm -= 3 * 1000;
    prv_feed_activity_minutes(minute_raw_data, k_minute_data_len);

    cl_assert_equal_i(s_num_captured_activity_sessions, 1);
    ASSERT_ACTIVITY_SESSION_PRESENT(s_captured_activity_sessions, s_num_captured_activity_sessions, &exp_session);
  }

  // --------------------------------------------------------------------------------------
  // Test a short walk that should not register
  {
    memset(minute_raw_data, 0, k_minute_data_bytes);
    s_num_captured_activity_sessions = 0;
    time_t now = rtc_get_time();

    int len = 5;
    KAlgTestActivitySession exp_session = {
      .activity = KAlgActivityType_Walk,
      .start_utc = now + 10 * SECONDS_PER_MINUTE,
      .steps = len * 80, // 80 steps/min
      .len_minutes = len,
      .resting_calories = len * 100,
      .active_calories = len * 200,
      .distance_mm = len * 1000,
    };

    prv_insert_artificial_activity_session(minute_raw_data, k_minute_data_len, &exp_session);
    prv_feed_activity_minutes(minute_raw_data, k_minute_data_len);
    cl_assert_equal_i(s_num_captured_activity_sessions, 0);
  }

  // --------------------------------------------------------------------------------------
  // Test a walk of 20 minutes followed by a run of 20 minutes
  {
    memset(minute_raw_data, 0, k_minute_data_bytes);
    s_num_captured_activity_sessions = 0;
    time_t now = rtc_get_time();

    int walk_len = 15;
    KAlgTestActivitySession exp_session_walk = {
      .activity = KAlgActivityType_Walk,
      .start_utc = now + 5 * SECONDS_PER_MINUTE,
      .steps = walk_len * 80, // 80 steps/min
      .len_minutes = walk_len,
      .resting_calories = walk_len * 100,
      .active_calories = walk_len * 200,
      .distance_mm = walk_len * 1000,
    };

    int run_len = 15;
    KAlgTestActivitySession exp_session_run = {
      .activity = KAlgActivityType_Run,
      .start_utc = now + 30 * SECONDS_PER_MINUTE,
      .steps = run_len * 150, // 150 steps/min
      .len_minutes = run_len,
      .resting_calories = run_len * 100,
      .active_calories = run_len * 200,
      .distance_mm = run_len * 1000,
    };

    prv_insert_artificial_activity_session(minute_raw_data, k_minute_data_len, &exp_session_walk);
    prv_insert_artificial_activity_session(minute_raw_data, k_minute_data_len, &exp_session_run);

    prv_feed_activity_minutes(minute_raw_data, k_minute_data_len);
    cl_assert_equal_i(s_num_captured_activity_sessions, 2);
    ASSERT_ACTIVITY_SESSION_PRESENT(s_captured_activity_sessions, s_num_captured_activity_sessions,
                                    &exp_session_walk);
    ASSERT_ACTIVITY_SESSION_PRESENT(s_captured_activity_sessions, s_num_captured_activity_sessions,
                                    &exp_session_run);
  }

  kernel_free(s_kalg_state);
  s_kalg_state = NULL;
}


// ---------------------------------------------------------------------------------------
void test_kraepelin_algorithm__sleep_stats(void) {
  // Init algorithm state
  s_kalg_state = kernel_zalloc(kalg_state_size());

  // It's easier to understand the algorithm log messages if we start at 0 time
  rtc_set_time(0);
  memset(s_kalg_state, 0, kalg_state_size());
  kalg_init(s_kalg_state, prv_stats_cb);
  s_num_captured_sleep_sessions = 0;

  // Get the samples for this test
  int num_samples;
  AlgMinuteFileSampleV5 *samples = activity_sample_sleep_v1_1(&num_samples);

  // Run samples through the activity detector
  time_t now = rtc_get_time();
  time_t test_start_utc = now;
  for (int i = 0; i < num_samples; i++) {
    uint16_t vmc = samples[i].vmc;
    // Convert from the old compressed VMC to the new uncompressed one
    vmc = vmc * vmc * 1850 / 1250;
    kalg_activities_update(s_kalg_state, now, samples[i].steps, vmc,
                           samples[i].orientation, samples[i].plugged_in,
                           0 /*rest_cals*/, 0 /*active_cals*/, 0 /*distance*/, false /* shutting_down */,
                           prv_sleep_session_callback, NULL);

    // This particular sample has sleep from minute 32 to 353
    const int k_sleep_start_m = 32;
    const int k_sleep_end_m = 353;
    const time_t k_sleep_start_utc = test_start_utc + k_sleep_start_m * SECONDS_PER_MINUTE;

    KAlgOngoingSleepStats stats;
    kalg_get_sleep_stats(s_kalg_state, &stats);

    // If we ask for the stats before sleep starts, should be no sleep info
    if (i < k_sleep_start_m) {
      cl_assert_equal_i(stats.sleep_start_utc, 0);
      cl_assert_equal_i(stats.sleep_len_m, 0);
      cl_assert_equal_i(stats.uncertain_start_utc, 0);
    }

    // If we ask once we know for sure sleep has started (at least 1 hour into it)
    if (i >= (k_sleep_start_m + 70) && (i <= k_sleep_end_m)) {
      cl_assert_equal_i(stats.sleep_start_utc, k_sleep_start_utc);
      cl_assert_equal_i(stats.sleep_len_m, i - k_sleep_start_m - KALG_MAX_UNCERTAIN_SLEEP_M);
      cl_assert_equal_i((now - stats.uncertain_start_utc) / SECONDS_PER_MINUTE,
                        KALG_MAX_UNCERTAIN_SLEEP_M);
    }

    // After we're certain sleep ended
    if (i > k_sleep_end_m + KALG_MAX_UNCERTAIN_SLEEP_M) {
      cl_assert_equal_i(stats.sleep_start_utc, k_sleep_start_utc);
      cl_assert_equal_i(stats.sleep_len_m, k_sleep_end_m - k_sleep_start_m);
      cl_assert_equal_i(stats.uncertain_start_utc, 0);
    }

    now += SECONDS_PER_MINUTE;
    rtc_set_time(now);
  }

  kernel_free(s_kalg_state);
  s_kalg_state = NULL;
}


