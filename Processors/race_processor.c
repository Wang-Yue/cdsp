/**
 * @file race_processor.c
 * @brief Implementation of the RACE cross-talk cancellation processor.
 *
 * Implementation details:
 * - Delay Unit Conversion: Converts user-configured delay units into
 * milliseconds/samples.
 * - Latency Compensation: Subtracted 1 sample period (e.g. 1000.0/sample_rate
 * in ms) from target delay to compensate for 1-sample processing feedback
 * latency: compensated_delay = max(delay - sample_period, 0.0)
 * - Gain Setup: Configured with negative dB attenuation and inverted phase
 * (`inverted = true`).
 * - Real-time processing (`race_processor_process`):
 *   Sample-by-sample feedback loop evaluating:
 *   1. added_A = val_A + feedback_B; added_B = val_B + feedback_A
 *   2. Pass added_A/B through interaural delay lines (`delay_A`, `delay_B`).
 *   3. Pass delayed samples through attenuation & phase inversion gain filter.
 *   4. Store new feedback samples and update output waveforms in place.
 */

#include "race_processor.h"

#include "Filters/delay.h"
#include "Filters/filter.h"
#include "Filters/gain.h"
#include "Logging/app_logger.h"
#include "processor.h"

static const logger_t g_logger = {"race_processor"};

struct race_processor {
  char name[64];  ///< Unique name of the RACE processor instance.
  int channel_a;  ///< Index of primary channel A (e.g., Left).
  int channel_b;  ///< Index of primary channel B (e.g., Right).
  delay_filter_t*
      delay_a;  ///< Contralateral delay line filter for channel A path.
  delay_filter_t*
      delay_b;          ///< Contralateral delay line filter for channel B path.
  gain_filter_t* gain;  ///< Attenuation and phase-inversion gain filter.
  double feedback_a;    ///< Recursive feedback sample from channel A delay/gain
                        ///< path.
  double feedback_b;    ///< Recursive feedback sample from channel B delay/gain
                        ///< path.
  bool channel_warning_logged;  ///< Track if we already logged a channel
                                ///< mismatch warning.
};

typedef struct race_processor race_processor_t;

/**
 * @brief Gets the name of the RACE processor.
 *
 * @param processor Pointer to the RACE processor.
 * @return The unique name of the processor instance.
 */
static const char* race_processor_get_name(const void* impl) {
  const race_processor_t* processor = (const race_processor_t*)impl;
  return processor ? processor->name : "";
}

#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Validates RACE cross-talk cancellation processor parameters.
 *
 * @param config Pointer to the RACE parameters to validate.
 * @param err Pointer to a config error struct to populate on failure.
 * @return 0 on success, -1 on failure.
 */
static int race_config_validate(const processor_config_t* config,
                                config_error_t* err) {
  if (!config || config->type != PROCESSOR_TYPE_RACE) return -1;
  const race_config_t* p = &config->parameters.race;
  if (p->channels <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: channels must be > 0, got %d", p->channels);
    return -1;
  }
  if (p->attenuation <= 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: attenuation must be > 0, got %g", p->attenuation);
    return -1;
  }
  if (p->delay <= 0.0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: delay must be > 0, got %g", p->delay);
    return -1;
  }
  if (p->channel_a == p->channel_b) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: channels A and B must be different, got both %d",
                     p->channel_a);
    return -1;
  }
  if (p->channel_a < 0 || p->channel_a >= p->channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: channel A %d is invalid (max: %d)", p->channel_a,
                     p->channels - 1);
    return -1;
  }
  if (p->channel_b < 0 || p->channel_b >= p->channels) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE: channel B %d is invalid (max: %d)", p->channel_b,
                     p->channels - 1);
    return -1;
  }
  return 0;
}

/**
 * @brief Frees all resources associated with the RACE processor.
 *
 * @param processor Pointer to RACE processor to free.
 */
static void race_processor_free(void* impl) {
  race_processor_t* processor = (race_processor_t*)impl;
  if (!processor) return;
  if (processor->delay_a) g_delay_vtable.free(processor->delay_a);
  if (processor->delay_b) g_delay_vtable.free(processor->delay_b);
  if (processor->gain) g_gain_vtable.free(processor->gain);
  free(processor);
}

/**
 * @brief Creates a new RACE cross-talk cancellation processor.
 *
 * @param name Unique name for this RACE instance.
 * @param config RACE configuration parameters.
 * @param sample_rate Audio sample rate in Hz.
 * @param chunk_size Maximum number of frames per processing chunk.
 * @param err Pointer to a config error struct to populate on failure.
 * @return Pointer to newly allocated race_processor_t, or NULL on failure.
 */
static void* race_processor_create(const char* name,
                                   const processor_config_t* config,
                                   int sample_rate, size_t chunk_size,
                                   config_error_t* err) {
  (void)chunk_size;
  if (!config || config->type != PROCESSOR_TYPE_RACE) return NULL;
  const race_config_t* params = &config->parameters.race;
  if (race_config_validate(config, err) != 0) return NULL;
  if (sample_rate <= 0) {
    config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                     "RACE sample_rate must be positive");
    return NULL;
  }

  race_processor_t* processor =
      (race_processor_t*)calloc(1, sizeof(race_processor_t));
  if (!processor) {
    config_error_set(err, CONFIG_ERR_PARSE,
                     "Failed to allocate RACE processor wrapper");
    return NULL;
  }

  if (name) {
    strncpy(processor->name, name, sizeof(processor->name) - 1);
    processor->name[sizeof(processor->name) - 1] = '\0';
  } else {
    strcpy(processor->name, "race");
  }

  processor->channel_a = params->channel_a < params->channel_b
                             ? params->channel_a
                             : params->channel_b;
  processor->channel_b = params->channel_a > params->channel_b
                             ? params->channel_a
                             : params->channel_b;

  delay_unit_t unit =
      params->has_delay_unit ? params->delay_unit : DELAY_UNIT_MS;

  /* Calculate the duration of one sample in the requested delay units.
     This is used to compensate for the implicit 1-sample delay introduced
     by the recursive feedback structure in the process loop.
     For MM (millimeters), we assume speed of sound is 343 m/s. */
  double sample_period = 1.0;
  switch (unit) {
    case DELAY_UNIT_US:
      sample_period = 1000000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_MS:
      sample_period = 1000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_S:
      sample_period = 1.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_MM:
      sample_period = 343.0 * 1000.0 / (double)sample_rate;
      break;
    case DELAY_UNIT_SAMPLES:
      sample_period = 1.0;
      break;
    default:
      sample_period = 1000.0 / (double)sample_rate;
      break;
  }

  /* Compensate for the 1-sample pipeline delay in the feedback path.
     Since the feedback signal is applied in the next sample period, we must
     subtract one sample period from the target delay line length.
     Clamp to 0.0 if target delay is smaller than one sample. */
  double comp_delay = params->delay - sample_period;
  if (comp_delay < 0.0) comp_delay = 0.0;

  delay_config_t dparams = {0};
  dparams.delay = comp_delay;
  dparams.delay_unit = unit;
  dparams.subsample =
      params->has_subsample_delay ? params->subsample_delay : false;

  filter_config_t dcfg = {.type = FILTER_TYPE_DELAY,
                          .parameters.delay = dparams};
  processor->delay_a = (delay_filter_t*)g_delay_vtable.create(
      "race-DelayA", &dcfg, sample_rate, 0, NULL, err);
  processor->delay_b = (delay_filter_t*)g_delay_vtable.create(
      "race-DelayB", &dcfg, sample_rate, 0, NULL, err);

  gain_config_t gparams = {0};
  gparams.gain = -params->attenuation;
  gparams.has_gain = true;
  gparams.scale = GAIN_SCALE_DB;
  gparams.inverted = true;
  gparams.mute = false;

  filter_config_t gcfg = {.type = FILTER_TYPE_GAIN, .parameters.gain = gparams};
  processor->gain =
      (gain_filter_t*)g_gain_vtable.create("race-Gain", &gcfg, 0, 0, NULL, err);
  processor->feedback_a = 0.0;
  processor->feedback_b = 0.0;

  if (!processor->delay_a || !processor->delay_b || !processor->gain) {
    if (err && err->type == CONFIG_ERR_NONE) {
      config_error_set(
          err, CONFIG_ERR_INVALID_FILTER,
          "Failed to initialize sub-filters for RACE processor '%s'",
          processor->name);
    }
    race_processor_free(processor);
    return NULL;
  }

  return processor;
}

/**
 * @brief Applies RACE cross-talk cancellation to audio chunk in place.
 *
 * Evaluates sample-by-sample recursive feedback loop across channel A and
 * channel B.
 *
 * @param processor Pointer to RACE processor.
 * @param chunk Audio chunk to process in place.
 */
static void race_processor_process(void* impl, audio_chunk_t* chunk) {
  race_processor_t* processor = (race_processor_t*)impl;
  if (!processor || !chunk) return;
  size_t count = audio_chunk_get_valid_frames(chunk);
  if (count == 0 || !processor->delay_a || !processor->delay_b ||
      !processor->gain)
    return;

  double* base_a = audio_chunk_get_channel(chunk, processor->channel_a);
  double* base_b = audio_chunk_get_channel(chunk, processor->channel_b);
  if (!base_a || !base_b) {
    if (!processor->channel_warning_logged) {
      logger_error(
          &g_logger,
          "RACE channel indices (%d, %d) out of bounds for chunk channels (%d)",
          processor->channel_a, processor->channel_b,
          audio_chunk_get_channels(chunk));
      processor->channel_warning_logged = true;
    }
    return;
  }

  // Evaluate sample-by-sample recursive cross-talk cancellation loop
  for (size_t i = 0; i < count; i++) {
    double val_a = base_a[i];
    double val_b = base_b[i];

    // Step 1: Add contralateral cancellation feedback signal from previous
    // sample step
    double added_a = val_a + processor->feedback_b;
    double added_b = val_b + processor->feedback_a;

    // Step 2: Pass combined signals through delay filters representing
    // interaural time difference (ITD)
    processor->feedback_a =
        delay_filter_process_single(processor->delay_a, added_a);
    processor->feedback_b =
        delay_filter_process_single(processor->delay_b, added_b);

    // Step 3: Pass delayed signals through gain filter representing acoustic
    // attenuation and phase inversion
    processor->feedback_a =
        gain_filter_process_single(processor->gain, processor->feedback_a);
    processor->feedback_b =
        gain_filter_process_single(processor->gain, processor->feedback_b);

    if (!isfinite(processor->feedback_a)) processor->feedback_a = 0.0;
    if (!isfinite(processor->feedback_b)) processor->feedback_b = 0.0;

    // Step 4: Output cross-talk cancelled samples in place
    base_a[i] = added_a;
    base_b[i] = added_b;
  }
}

/**
 * @brief Transfers recursive feedback loop registers from src to dest.
 *
 * @param dest The destination RACE processor instance.
 * @param src The source RACE processor instance.
 */
static void race_processor_transfer_state(void* dest_ptr,
                                          const void* src_ptr) {
  race_processor_t* dest = (race_processor_t*)dest_ptr;
  const race_processor_t* src = (const race_processor_t*)src_ptr;
  if (!dest || !src || dest == src) return;
  dest->feedback_a = src->feedback_a;
  dest->feedback_b = src->feedback_b;
  if (dest->delay_a && src->delay_a) {
    g_delay_vtable.transfer_state(dest->delay_a, src->delay_a);
  }
  if (dest->delay_b && src->delay_b) {
    g_delay_vtable.transfer_state(dest->delay_b, src->delay_b);
  }
}

const processor_vtable_t g_race_vtable = {
    .validate = race_config_validate,
    .create = race_processor_create,
    .process = race_processor_process,
    .get_name = race_processor_get_name,
    .transfer_state = race_processor_transfer_state,
    .free = race_processor_free};
