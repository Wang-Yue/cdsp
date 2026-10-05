#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp_wasm.h"
#include "audio/audio_chunk.h"
#include "audio/processing_parameters.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "pipeline/pipeline.h"
#include "test_support.h"

static const char *kSharedDspConfigJson =
    "{\n"
    "  \"title\": \"Native vs WASM Parity Test\",\n"
    "  \"devices\": {\n"
    "    \"samplerate\": 48000,\n"
    "    \"chunksize\": 128,\n"
    "    \"capture\": {\n"
    "      \"type\": \"WebAudio\",\n"
    "      \"channels\": 2\n"
    "    },\n"
    "    \"playback\": {\n"
    "      \"type\": \"WebAudio\",\n"
    "      \"channels\": 2\n"
    "    }\n"
    "  },\n"
    "  \"filters\": {\n"
    "    \"low_shelf\": {\n"
    "      \"type\": \"Biquad\",\n"
    "      \"parameters\": {\n"
    "        \"type\": \"Lowshelf\",\n"
    "        \"freq\": 120.0,\n"
    "        \"gain\": 5.5,\n"
    "        \"q\": 0.707\n"
    "      }\n"
    "    },\n"
    "    \"peaking\": {\n"
    "      \"type\": \"Biquad\",\n"
    "      \"parameters\": {\n"
    "        \"type\": \"Peaking\",\n"
    "        \"freq\": 1000.0,\n"
    "        \"gain\": -3.0,\n"
    "        \"q\": 1.414\n"
    "      }\n"
    "    },\n"
    "    \"high_shelf\": {\n"
    "      \"type\": \"Biquad\",\n"
    "      \"parameters\": {\n"
    "        \"type\": \"Highshelf\",\n"
    "        \"freq\": 8000.0,\n"
    "        \"gain\": 4.0,\n"
    "        \"q\": 0.707\n"
    "      }\n"
    "    }\n"
    "  },\n"
    "  \"pipeline\": [\n"
    "    { \"type\": \"Filter\", \"channels\": [0], \"names\": [\"low_shelf\", \"peaking\", \"high_shelf\"] },\n"
    "    { \"type\": \"Filter\", \"channels\": [1], \"names\": [\"low_shelf\", \"peaking\", \"high_shelf\"] }\n"
    "  ]\n"
    "}\n";

TEST(wasm_lifecycle_and_api) {
  cdsp_wasm_t *wasm = cdsp_wasm_create(kSharedDspConfigJson, 48000, 128);
  ASSERT_TRUE(wasm != NULL);
  ASSERT_EQ(cdsp_wasm_get_input_channels(wasm), 2);
  ASSERT_EQ(cdsp_wasm_get_output_channels(wasm), 2);
  ASSERT_EQ(cdsp_wasm_get_quantum_size(wasm), 128);

  float **in_ptrs = cdsp_wasm_get_input_buffer_ptrs(wasm);
  float **out_ptrs = cdsp_wasm_get_output_buffer_ptrs(wasm);
  ASSERT_TRUE(in_ptrs != NULL);
  ASSERT_TRUE(out_ptrs != NULL);
  ASSERT_TRUE(in_ptrs[0] != NULL);
  ASSERT_TRUE(in_ptrs[1] != NULL);
  ASSERT_TRUE(out_ptrs[0] != NULL);
  ASSERT_TRUE(out_ptrs[1] != NULL);

  // Test volume fader
  cdsp_wasm_set_fader_volume(wasm, 0, -6.0, true);
  double vol = cdsp_wasm_get_fader_volume(wasm, 0);
  ASSERT_NEAR(vol, -6.0, 1e-4);

  // Test mute
  cdsp_wasm_set_fader_mute(wasm, 0, true);
  ASSERT_TRUE(cdsp_wasm_get_fader_mute(wasm, 0));
  cdsp_wasm_set_fader_mute(wasm, 0, false);
  ASSERT_FALSE(cdsp_wasm_get_fader_mute(wasm, 0));

  // Test VU levels
  float in_peak[2] = {0}, in_rms[2] = {0};
  float out_peak[2] = {0}, out_rms[2] = {0};
  cdsp_wasm_get_vu_levels(wasm, in_peak, in_rms, out_peak, out_rms);

  // Test hot-swap config reload
  static const char *kNewJson =
      "{\n"
      "  \"title\": \"WebAudio Reload Test\",\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 48000,\n"
      "    \"chunksize\": 128,\n"
      "    \"capture\": { \"type\": \"WebAudio\", \"channels\": 2 },\n"
      "    \"playback\": { \"type\": \"WebAudio\", \"channels\": 2 }\n"
      "  }\n"
      "}\n";
  ASSERT_TRUE(cdsp_wasm_set_config_json(wasm, kNewJson));

  cdsp_wasm_destroy(wasm);
}

TEST(wasm_bit_correctness_against_native_pipeline) {
  // 1. Build Native CDSP Pipeline directly
  config_error_t cfg_err;
  config_error_init(&cfg_err);

  dsp_config_t *native_config = NULL;
  ASSERT_EQ(dsp_config_parse_json(kSharedDspConfigJson, &native_config, &cfg_err), 0);
  ASSERT_TRUE(native_config != NULL);

  processing_parameters_t *native_params = processing_parameters_create(2, 2);
  ASSERT_TRUE(native_params != NULL);

  pipeline_t *native_pipeline = pipeline_create(native_config, native_params, 128, &cfg_err);
  ASSERT_TRUE(native_pipeline != NULL);

  audio_chunk_t *native_in = audio_chunk_create(128, 2);
  audio_chunk_t *native_out = audio_chunk_create(128, 2);
  ASSERT_TRUE(native_in != NULL);
  ASSERT_TRUE(native_out != NULL);

  // 2. Build WebAssembly Engine using identical config
  cdsp_wasm_t *wasm = cdsp_wasm_create(kSharedDspConfigJson, 48000, 128);
  ASSERT_TRUE(wasm != NULL);

  float **wasm_in = cdsp_wasm_get_input_buffer_ptrs(wasm);
  float **wasm_out = cdsp_wasm_get_output_buffer_ptrs(wasm);
  ASSERT_TRUE(wasm_in != NULL);
  ASSERT_TRUE(wasm_out != NULL);

  // 3. Process multiple chunks across varied frequencies & dynamics (e.g. 5 blocks = 640 frames)
  for (int block = 0; block < 5; block++) {
    // Generate multi-tone test signal (100 Hz, 1 kHz, 5 kHz)
    for (size_t i = 0; i < 128; i++) {
      size_t t = (size_t)block * 128 + i;
      double s0 = sin(2.0 * M_PI * 100.0 * (double)t / 48000.0) * 0.3 +
                  sin(2.0 * M_PI * 1000.0 * (double)t / 48000.0) * 0.3 +
                  sin(2.0 * M_PI * 5000.0 * (double)t / 48000.0) * 0.3;
      double s1 = cos(2.0 * M_PI * 150.0 * (double)t / 48000.0) * 0.4 +
                  cos(2.0 * M_PI * 3000.0 * (double)t / 48000.0) * 0.4;

      // Feed into WASM float buffer (simulating AudioWorklet input)
      float f0 = (float)s0;
      float f1 = (float)s1;
      wasm_in[0][i] = f0;
      wasm_in[1][i] = f1;

      // Feed into Native chunk as float -> double exactly as wasm does
      audio_chunk_get_channel(native_in, 0)[i] = (double)f0;
      audio_chunk_get_channel(native_in, 1)[i] = (double)f1;
    }
    audio_chunk_set_valid_frames(native_in, 128);

    // Process both pipelines
    bool wasm_ok = cdsp_wasm_process(wasm, 128);
    ASSERT_TRUE(wasm_ok);

    pipeline_error_t native_err = pipeline_process(native_pipeline, native_in, native_out);
    ASSERT_EQ(native_err, PIPELINE_OK);

    // Compare each sample: Native double quantized to float must BIT-EXACTLY match WASM output float
    for (size_t ch = 0; ch < 2; ch++) {
      double *native_samples = audio_chunk_get_channel(native_out, ch);
      float *wasm_samples = wasm_out[ch];
      for (size_t i = 0; i < 128; i++) {
        float expected_sample = (float)native_samples[i];
        float actual_sample = wasm_samples[i];

        uint32_t expected_bits, actual_bits;
        memcpy(&expected_bits, &expected_sample, sizeof(uint32_t));
        memcpy(&actual_bits, &actual_sample, sizeof(uint32_t));

        if (expected_bits != actual_bits) {
          printf("  [FAIL] Bit mismatch at block %d, channel %zu, frame %zu: expected 0x%08X (float %f), got 0x%08X (float %f)\n",
                 block, ch, i, expected_bits, expected_sample, actual_bits, actual_sample);
        }
        ASSERT_EQ(expected_bits, actual_bits);
      }
    }
  }

  // Clean up
  cdsp_wasm_destroy(wasm);
  audio_chunk_free(native_in);
  audio_chunk_free(native_out);
  pipeline_free(native_pipeline);
  processing_parameters_free(native_params);
}

TEST(wasm_spectrum_computation) {
  cdsp_wasm_t *wasm = cdsp_wasm_create(kSharedDspConfigJson, 48000, 128);
  ASSERT_TRUE(wasm != NULL);

  float **wasm_in = cdsp_wasm_get_input_buffer_ptrs(wasm);
  ASSERT_TRUE(wasm_in != NULL);

  // Prime history buffer by feeding 1000 Hz sine wave for 100 blocks (12800 frames)
  for (int block = 0; block < 100; block++) {
    for (size_t i = 0; i < 128; i++) {
      size_t t = (size_t)block * 128 + i;
      float s = (float)(sin(2.0 * M_PI * 1000.0 * (double)t / 48000.0) * 0.8);
      wasm_in[0][i] = s;
      wasm_in[1][i] = s;
    }
    ASSERT_TRUE(cdsp_wasm_process(wasm, 128));
  }

  // Get spectrum from WASM
  const size_t n_bins = 48;
  float bins[48] = {0};
  bool ok = cdsp_wasm_get_spectrum(wasm, true, 0, 20.0, 20000.0, n_bins, bins);
  ASSERT_TRUE(ok);

  // Find peak bin
  float max_val = -200.0f;
  size_t peak_bin = 0;
  for (size_t i = 0; i < n_bins; i++) {
    if (bins[i] > max_val) {
      max_val = bins[i];
      peak_bin = i;
    }
  }

  // Peak must be around -2 dBFS to 0 dBFS (amplitude 0.8 is ~ -1.94 dBFS)
  ASSERT_NEAR((double)max_val, -1.94, 2.0);

  ASSERT_TRUE(peak_bin >= 25 && peak_bin <= 28);

  cdsp_wasm_destroy(wasm);
}

TEST(wasm_end_to_end_dummy_signal_gain_filter_levels_and_spectrum) {
  // Test configuration with a +6 dB Gain filter on Channel 0 and -6 dB Gain filter on Channel 1
  static const char *kGainDspConfigJson =
      "{\n"
      "  \"title\": \"End to End Gain Filter Test\",\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 48000,\n"
      "    \"chunksize\": 128,\n"
      "    \"capture\": {\n"
      "      \"type\": \"WebAudio\",\n"
      "      \"channels\": 2\n"
      "    },\n"
      "    \"playback\": {\n"
      "      \"type\": \"WebAudio\",\n"
      "      \"channels\": 2\n"
      "    }\n"
      "  },\n"
      "  \"filters\": {\n"
      "    \"gain_boost\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": {\n"
      "        \"gain\": 6.0,\n"
      "        \"inverted\": false\n"
      "      }\n"
      "    },\n"
      "    \"gain_cut\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": {\n"
      "        \"gain\": -6.0,\n"
      "        \"inverted\": false\n"
      "      }\n"
      "    }\n"
      "  },\n"
      "  \"pipeline\": [\n"
      "    { \"type\": \"Filter\", \"channels\": [0], \"names\": [\"gain_boost\"] },\n"
      "    { \"type\": \"Filter\", \"channels\": [1], \"names\": [\"gain_cut\"] }\n"
      "  ]\n"
      "}\n";

  cdsp_wasm_t *wasm = cdsp_wasm_create(kGainDspConfigJson, 48000, 128);
  ASSERT_TRUE(wasm != NULL);

  float **wasm_in = cdsp_wasm_get_input_buffer_ptrs(wasm);
  float **wasm_out = cdsp_wasm_get_output_buffer_ptrs(wasm);
  ASSERT_TRUE(wasm_in != NULL);
  ASSERT_TRUE(wasm_out != NULL);

  // Generate 1 kHz sine wave at 0.25 amplitude (-12.04 dBFS)
  // After +6 dB gain (ch 0): amplitude ~ 0.50 (-6.02 dBFS)
  // After -6 dB gain (ch 1): amplitude ~ 0.125 (-18.06 dBFS)
  const double in_amp = 0.25;
  for (int block = 0; block < 100; block++) {
    for (size_t i = 0; i < 128; i++) {
      size_t t = (size_t)block * 128 + i;
      float s = (float)(sin(2.0 * M_PI * 1000.0 * (double)t / 48000.0) * in_amp);
      wasm_in[0][i] = s;
      wasm_in[1][i] = s;
    }
    ASSERT_TRUE(cdsp_wasm_process(wasm, 128));
  }

  // 1. Verify Output Audio Samples Match the Gain Filter
  for (size_t i = 0; i < 128; i++) {
    float expected_ch0 = wasm_in[0][i] * powf(10.0f, 6.0f / 20.0f);
    float expected_ch1 = wasm_in[1][i] * powf(10.0f, -6.0f / 20.0f);
    ASSERT_NEAR(wasm_out[0][i], expected_ch0, 1e-4);
    ASSERT_NEAR(wasm_out[1][i], expected_ch1, 1e-4);
  }

  // 2. Verify VU Meter Levels (Peak & RMS) End-to-End
  float in_peak[2] = {0}, in_rms[2] = {0};
  float out_peak[2] = {0}, out_rms[2] = {0};
  cdsp_wasm_get_vu_levels(wasm, in_peak, in_rms, out_peak, out_rms);

  // Expected input peak = 20*log10(0.25) ~= -12.04 dBFS
  ASSERT_NEAR((double)in_peak[0], -12.04, 0.5);
  ASSERT_NEAR((double)in_peak[1], -12.04, 0.5);

  // Expected input RMS = -12.04 - 3.01 = -15.05 dBFS
  ASSERT_NEAR((double)in_rms[0], -15.05, 0.5);
  ASSERT_NEAR((double)in_rms[1], -15.05, 0.5);

  // Expected output ch0 peak (+6 dB) ~= -6.02 dBFS, ch1 peak (-6 dB) ~= -18.06 dBFS
  ASSERT_NEAR((double)out_peak[0], -6.02, 0.5);
  ASSERT_NEAR((double)out_peak[1], -18.06, 0.5);

  // Expected output ch0 RMS ~= -9.03 dBFS, ch1 RMS ~= -21.07 dBFS
  ASSERT_NEAR((double)out_rms[0], -9.03, 0.5);
  ASSERT_NEAR((double)out_rms[1], -21.07, 0.5);

  // 3. Verify Spectrum for Capture and Playback
  const size_t n_bins = 48;
  float in_spec[48] = {0};
  float out_spec_ch0[48] = {0};
  float out_spec_ch1[48] = {0};

  bool ok_in = cdsp_wasm_get_spectrum(wasm, true, 0, 20.0, 20000.0, n_bins, in_spec);
  bool ok_out0 = cdsp_wasm_get_spectrum(wasm, false, 0, 20.0, 20000.0, n_bins, out_spec_ch0);
  bool ok_out1 = cdsp_wasm_get_spectrum(wasm, false, 1, 20.0, 20000.0, n_bins, out_spec_ch1);

  ASSERT_TRUE(ok_in);
  ASSERT_TRUE(ok_out0);
  ASSERT_TRUE(ok_out1);

  // Find peak 1 kHz bins across capture & playback spectra
  float max_in = -200.0f, max_out0 = -200.0f, max_out1 = -200.0f;
  size_t peak_bin_in = 0, peak_bin_out0 = 0, peak_bin_out1 = 0;

  for (size_t i = 0; i < n_bins; i++) {
    if (in_spec[i] > max_in) {
      max_in = in_spec[i];
      peak_bin_in = i;
    }
    if (out_spec_ch0[i] > max_out0) {
      max_out0 = out_spec_ch0[i];
      peak_bin_out0 = i;
    }
    if (out_spec_ch1[i] > max_out1) {
      max_out1 = out_spec_ch1[i];
      peak_bin_out1 = i;
    }
  }

  // Peak bin must accurately be around 1 kHz (bin 26-27)
  ASSERT_TRUE(peak_bin_in >= 25 && peak_bin_in <= 28);
  ASSERT_TRUE(peak_bin_out0 >= 25 && peak_bin_out0 <= 28);
  ASSERT_TRUE(peak_bin_out1 >= 25 && peak_bin_out1 <= 28);

  // Spectrum peak magnitude differences must reflect +6 dB and -6 dB filters
  ASSERT_NEAR((double)(max_out0 - max_in), 6.0, 1.0);
  // 4. Test Dynamic Hot-Swap Reconfiguration (Simulating User tapping "Apply Configuration")
  // Change filter dynamically from +6 dB to -12 dB on Channel 0, and from -6 dB to +10 dB on Channel 1
  static const char *kDynamicGainJson =
      "{\n"
      "  \"title\": \"Hot Swap Reconfiguration Test\",\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 48000,\n"
      "    \"chunksize\": 128,\n"
      "    \"capture\": { \"type\": \"WebAudio\", \"channels\": 2 },\n"
      "    \"playback\": { \"type\": \"WebAudio\", \"channels\": 2 }\n"
      "  },\n"
      "  \"filters\": {\n"
      "    \"cut_12db\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": { \"gain\": -12.0, \"inverted\": false }\n"
      "    },\n"
      "    \"boost_10db\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": { \"gain\": 10.0, \"inverted\": false }\n"
      "    }\n"
      "  },\n"
      "  \"pipeline\": [\n"
      "    { \"type\": \"Filter\", \"channels\": [0], \"names\": [\"cut_12db\"] },\n"
      "    { \"type\": \"Filter\", \"channels\": [1], \"names\": [\"boost_10db\"] }\n"
      "  ]\n"
      "}\n";

  ASSERT_TRUE(cdsp_wasm_set_config_json(wasm, kDynamicGainJson));

  // Process 50 new blocks with identical input signal
  for (int block = 0; block < 50; block++) {
    for (size_t i = 0; i < 128; i++) {
      size_t t = (size_t)block * 128 + i;
      float s = (float)(sin(2.0 * M_PI * 1000.0 * (double)t / 48000.0) * in_amp);
      wasm_in[0][i] = s;
      wasm_in[1][i] = s;
    }
    ASSERT_TRUE(cdsp_wasm_process(wasm, 128));
  }

  // Verify that the output levels have immediately updated to match the newly applied config:
  // Channel 0: in -12.04 dBFS - 12 dB = -24.04 dBFS (sample amplitude 0.25 * 0.25119 ~= 0.0628)
  // Channel 1: in -12.04 dBFS + 10 dB = -2.04 dBFS (sample amplitude 0.25 * 3.16228 ~= 0.7906)
  for (size_t i = 0; i < 128; i++) {
    float expected_ch0 = wasm_in[0][i] * powf(10.0f, -12.0f / 20.0f);
    float expected_ch1 = wasm_in[1][i] * powf(10.0f, 10.0f / 20.0f);
    ASSERT_NEAR(wasm_out[0][i], expected_ch0, 1e-4);
    ASSERT_NEAR(wasm_out[1][i], expected_ch1, 1e-4);
  }

  float dyn_in_peak[2] = {0}, dyn_in_rms[2] = {0};
  float dyn_out_peak[2] = {0}, dyn_out_rms[2] = {0};
  cdsp_wasm_get_vu_levels(wasm, dyn_in_peak, dyn_in_rms, dyn_out_peak, dyn_out_rms);

  // Output Ch0 Peak must now be ~ -24.04 dBFS (changed from -6.02 dBFS)
  ASSERT_NEAR((double)dyn_out_peak[0], -24.04, 0.5);
  // Output Ch1 Peak must now be ~ -2.04 dBFS (changed from -18.06 dBFS)
  ASSERT_NEAR((double)dyn_out_peak[1], -2.04, 0.5);

  cdsp_wasm_destroy(wasm);
}

TEST(wasm_silent_spectrum_and_levels) {
  cdsp_wasm_t *wasm = cdsp_wasm_create(kSharedDspConfigJson, 48000, 128);
  ASSERT_TRUE(wasm != NULL);

  float **wasm_in = cdsp_wasm_get_input_buffer_ptrs(wasm);
  ASSERT_TRUE(wasm_in != NULL);

  // Process 50 blocks of pure silence (all 0.0f)
  for (int block = 0; block < 50; block++) {
    memset(wasm_in[0], 0, 128 * sizeof(float));
    memset(wasm_in[1], 0, 128 * sizeof(float));
    ASSERT_TRUE(cdsp_wasm_process(wasm, 128));
  }

  // Check VU levels
  float in_peak[2] = {0}, in_rms[2] = {0};
  float out_peak[2] = {0}, out_rms[2] = {0};
  cdsp_wasm_get_vu_levels(wasm, in_peak, in_rms, out_peak, out_rms);

  printf("  [SILENCE] in_peak: [%f, %f], in_rms: [%f, %f]\n", in_peak[0], in_peak[1], in_rms[0], in_rms[1]);
  printf("  [SILENCE] out_peak: [%f, %f], out_rms: [%f, %f]\n", out_peak[0], out_peak[1], out_rms[0], out_rms[1]);

  ASSERT_TRUE(isinf(in_peak[0]) && in_peak[0] < 0);
  ASSERT_TRUE(isinf(out_peak[0]) && out_peak[0] < 0);

  // Check spectrum
  const size_t n_bins = 48;
  float bins_in[48] = {0};
  float bins_out[48] = {0};
  bool ok_in = cdsp_wasm_get_spectrum(wasm, true, 0, 20.0, 20000.0, n_bins, bins_in);
  bool ok_out = cdsp_wasm_get_spectrum(wasm, false, 0, 20.0, 20000.0, n_bins, bins_out);

  ASSERT_TRUE(ok_in);
  ASSERT_TRUE(ok_out);

  for (size_t i = 0; i < n_bins; i++) {
    printf("  [SILENCE] bin %zu: in=%f, out=%f\n", i, bins_in[i], bins_out[i]);
    ASSERT_TRUE(isinf(bins_in[i]) && bins_in[i] < 0);
    ASSERT_TRUE(isinf(bins_out[i]) && bins_out[i] < 0);
  }

  cdsp_wasm_destroy(wasm);
}

TEST_MAIN()


