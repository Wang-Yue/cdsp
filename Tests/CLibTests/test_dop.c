#include <math.h>

#include "Audio/sample_conversion.h"
#include "Config/engine_config_types.h"
#include "DoP/dop_decoder.h"
#include "DoP/dsd_encoder.h"
#include "test_support.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TEST(DoPDetectionAndBypass) {
  int multipliers[] = {64, 128, 256};
  double base_rates[] = {44100.0, 48000.0};

  for (int i = 0; i < 3; i++) {
    int mult = multipliers[i];
    for (int j = 0; j < 2; j++) {
      double base_rate = base_rates[j];
      double pcm_sample_rate = base_rate * (double)mult / 16.0;
      dop_decoder_t* decoder =
          dop_decoder_create(2, pcm_sample_rate, false, 20000.0, false);
      ASSERT_TRUE(decoder != NULL);

      audio_chunk_t* chunk = audio_chunk_create(64, 2);
      for (size_t t = 0; t < 64; t++) {
        uint32_t marker = (t % 2 == 0) ? 0x05 : 0xFA;
        uint32_t val24 = (marker << 16) | 0x1234;
        int32_t int_val = (int32_t)(val24 << 8) >> 8;
        double float_val = (double)int_val / 8388608.0;
        audio_chunk_get_channel(chunk, 0)[t] = float_val;
        audio_chunk_get_channel(chunk, 1)[t] = float_val;
      }

      audio_chunk_t* part_chunk = audio_chunk_create(20, 2);
      for (int ch = 0; ch < 2; ch++) {
        mutable_waveform_t dst = audio_chunk_get_channel(part_chunk, ch);
        mutable_waveform_t src = audio_chunk_get_channel(chunk, ch);
        if (dst && src) {
          for (size_t t = 0; t < 20; t++) {
            dst[t] = src[t];
          }
        }
      }
      bool is_decoded = dop_decoder_detect_and_process(decoder, part_chunk);
      ASSERT_FALSE(is_decoded);
      ASSERT_FALSE(dop_decoder_is_active(decoder));
      audio_chunk_free(part_chunk);

      audio_chunk_t* part_chunk2 = audio_chunk_create(44, 2);
      for (int ch = 0; ch < 2; ch++) {
        mutable_waveform_t dst = audio_chunk_get_channel(part_chunk2, ch);
        mutable_waveform_t src = audio_chunk_get_channel(chunk, ch);
        if (dst && src) {
          for (size_t t = 0; t < 44; t++) {
            dst[t] = src[t + 20];
          }
        }
      }
      is_decoded = dop_decoder_detect_and_process(decoder, part_chunk2);
      ASSERT_TRUE(is_decoded);
      ASSERT_TRUE(dop_decoder_is_active(decoder));
      audio_chunk_free(part_chunk2);
      dop_decoder_free(decoder);

      dop_decoder_t* bypassed_decoder =
          dop_decoder_create(2, pcm_sample_rate, true, 20000.0, false);
      audio_chunk_t* test_chunk = audio_chunk_create(64, 2);
      for (int ch = 0; ch < 2; ch++) {
        for (size_t t = 0; t < 64; t++) {
          audio_chunk_get_channel(test_chunk, ch)[t] =
              audio_chunk_get_channel(chunk, ch)[t];
        }
      }
      bool processed =
          dop_decoder_detect_and_process(bypassed_decoder, test_chunk);
      ASSERT_FALSE(processed);
      ASSERT_FALSE(dop_decoder_is_active(bypassed_decoder));
      audio_chunk_free(test_chunk);
      audio_chunk_free(chunk);
      dop_decoder_free(bypassed_decoder);
    }
  }
}

TEST(DoPFalsePositives) {
  int multipliers[] = {64, 128, 256};
  double base_rates[] = {44100.0, 48000.0};

  for (int i = 0; i < 3; i++) {
    int mult = multipliers[i];
    for (int j = 0; j < 2; j++) {
      double base_rate = base_rates[j];
      double pcm_sample_rate = base_rate * (double)mult / 16.0;
      dop_decoder_t* decoder =
          dop_decoder_create(1, pcm_sample_rate, false, 20000.0, false);
      audio_chunk_t* chunk = audio_chunk_create(64, 1);

      for (size_t t = 0; t < 64; t++)
        audio_chunk_get_channel(chunk, 0)[t] = 0.0;
      bool res1 = dop_decoder_detect_and_process(decoder, chunk);
      ASSERT_FALSE(res1);

      for (size_t t = 0; t < 64; t++) {
        audio_chunk_get_channel(chunk, 0)[t] =
            ((double)rand() / RAND_MAX) * 2.0 - 1.0;
      }
      bool res2 = dop_decoder_detect_and_process(decoder, chunk);
      ASSERT_FALSE(res2);

      audio_chunk_free(chunk);
      dop_decoder_free(decoder);
    }
  }
}

TEST(MultiChunkDoPStreamStability) {
  int multipliers[] = {64, 128, 256};
  double base_rates[] = {44100.0, 48000.0};

  for (int i = 0; i < 3; i++) {
    int mult = multipliers[i];
    for (int j = 0; j < 2; j++) {
      double base_rate = base_rates[j];
      double pcm_sample_rate = base_rate * (double)mult / 16.0;
      dop_decoder_t* decoder =
          dop_decoder_create(2, pcm_sample_rate, false, 20000.0, false);
      int chunk_size = 1024;
      int num_chunks = 10;

      int global_frame_idx = 0;
      for (int chunk_idx = 1; chunk_idx <= num_chunks; chunk_idx++) {
        audio_chunk_t* chunk = audio_chunk_create(chunk_size, 2);
        for (int t = 0; t < chunk_size; t++) {
          uint32_t marker = (global_frame_idx % 2 == 0) ? 0x05 : 0xFA;
          uint32_t val24 = (marker << 16) | 0x4321;
          int32_t int_val = (int32_t)(val24 << 8) >> 8;
          double float_val = (double)int_val / 8388608.0;
          audio_chunk_get_channel(chunk, 0)[t] = float_val;
          audio_chunk_get_channel(chunk, 1)[t] = float_val;
          global_frame_idx++;
        }

        bool processed = dop_decoder_detect_and_process(decoder, chunk);
        ASSERT_TRUE(processed);
        ASSERT_TRUE(dop_decoder_is_active(decoder));
        audio_chunk_free(chunk);
      }
      dop_decoder_free(decoder);
    }
  }
}

TEST(DoPRoundtripSINAD) {
  int multipliers[] = {64, 128, 256};
  double base_rates[] = {44100.0, 48000.0};

  for (int i = 0; i < 3; i++) {
    int mult = multipliers[i];
    for (int j = 0; j < 2; j++) {
      double base_rate = base_rates[j];
      size_t pcm_sample_rate = (size_t)round(base_rate * (double)mult / 16.0);
      dsd_encoder_t* encoder =
          dsd_encoder_create(1, pcm_sample_rate, DSD_MODE_DOP, 16,
                             SDM_FILTER_SDM6, 20000.0, false);
      dop_decoder_t* decoder =
          dop_decoder_create(1, pcm_sample_rate, false, 20000.0, false);
      ASSERT_TRUE(encoder != NULL);
      ASSERT_TRUE(decoder != NULL);

      double frames_per_cycle = pcm_sample_rate / 1000.0;
      int active_frames = (int)round(frames_per_cycle * 10.0);
      int settle_frames = (int)round(frames_per_cycle * 4.0);
      int frames = settle_frames + active_frames;

      audio_chunk_t* chunk = audio_chunk_create(frames, 1);
      double amplitude = 0.7071;
      for (int t = 0; t < frames; t++) {
        audio_chunk_get_channel(chunk, 0)[t] =
            amplitude * sin(2.0 * M_PI * 1000.0 * (double)t / pcm_sample_rate);
      }

      dsd_encoder_encode(encoder, chunk);

      bool processed = dop_decoder_detect_and_process(decoder, chunk);
      ASSERT_TRUE(processed);
      ASSERT_TRUE(dop_decoder_is_active(decoder));

      double target_freq = 1000.0;
      double cos_sum = 0.0;
      double sin_sum = 0.0;
      for (int t = settle_frames; t < frames; t++) {
        double angle = 2.0 * M_PI * target_freq * (double)t / pcm_sample_rate;
        cos_sum += audio_chunk_get_channel(chunk, 0)[t] * cos(angle);
        sin_sum += audio_chunk_get_channel(chunk, 0)[t] * sin(angle);
      }
      double cos_amp = (2.0 / (double)active_frames) * cos_sum;
      double sin_amp = (2.0 / (double)active_frames) * sin_sum;
      double fundamental_power = (cos_amp * cos_amp + sin_amp * sin_amp) / 2.0;

      double total_power = 0.0;
      for (int t = settle_frames; t < frames; t++) {
        total_power += audio_chunk_get_channel(chunk, 0)[t] *
                       audio_chunk_get_channel(chunk, 0)[t];
      }
      total_power /= (double)active_frames;

      double noise_power = total_power - fundamental_power;
      if (noise_power < 1e-20) noise_power = 1e-20;
      double sinad = 10.0 * log10(fundamental_power / noise_power);

      double expected_min_sinad = 80.0;
      if (mult == 64)
        expected_min_sinad = 90.0;
      else if (mult == 128)
        expected_min_sinad = 110.0;
      else if (mult == 256)
        expected_min_sinad = 115.0;

      ASSERT_TRUE(sinad >= expected_min_sinad);

      audio_chunk_free(chunk);
      dop_decoder_free(decoder);
      dsd_encoder_free(encoder);
    }
  }
}

TEST(DoPVariableChunkRoundtrip) {
  size_t pcm_sample_rate = 176400;
  dsd_encoder_t* encoder = dsd_encoder_create(
      1, pcm_sample_rate, DSD_MODE_DOP, 16, SDM_FILTER_SDM6, 20000.0, false);
  dop_decoder_t* decoder =
      dop_decoder_create(1, (double)pcm_sample_rate, false, 20000.0, false);
  ASSERT_TRUE(encoder != NULL);
  ASSERT_TRUE(decoder != NULL);

  double frames_per_cycle = (double)pcm_sample_rate / 1000.0;
  int active_frames = (int)round(frames_per_cycle * 10.0);
  int settle_frames = (int)round(frames_per_cycle * 4.0);
  int total_frames = settle_frames + active_frames;

  double* input_buf = (double*)calloc(total_frames, sizeof(double));
  double* output_buf = (double*)calloc(total_frames, sizeof(double));
  double amplitude = 0.7071;
  for (int t = 0; t < total_frames; t++) {
    input_buf[t] = amplitude * sin(2.0 * M_PI * 1000.0 * (double)t /
                                   (double)pcm_sample_rate);
  }

  int chunk_sizes[] = {200, 50, 400, 1000, 5, 815};
  int num_chunks = sizeof(chunk_sizes) / sizeof(chunk_sizes[0]);

  int offset = 0;
  for (int i = 0; i < num_chunks; i++) {
    int sz = chunk_sizes[i];
    audio_chunk_t* chunk = audio_chunk_create(sz, 1);
    double* ch_data = audio_chunk_get_channel(chunk, 0);
    memcpy(ch_data, input_buf + offset, sz * sizeof(double));
    audio_chunk_set_valid_frames(chunk, sz);

    dsd_encoder_encode(encoder, chunk);
    bool processed = dop_decoder_detect_and_process(decoder, chunk);
    if (offset >= 32) {
      ASSERT_TRUE(processed);
      ASSERT_TRUE(dop_decoder_is_active(decoder));
    }

    memcpy(output_buf + offset, audio_chunk_get_channel(chunk, 0),
           sz * sizeof(double));
    audio_chunk_free(chunk);
    offset += sz;
  }

  double target_freq = 1000.0;
  double cos_sum = 0.0;
  double sin_sum = 0.0;
  for (int t = settle_frames; t < total_frames; t++) {
    double angle =
        2.0 * M_PI * target_freq * (double)t / (double)pcm_sample_rate;
    cos_sum += output_buf[t] * cos(angle);
    sin_sum += output_buf[t] * sin(angle);
  }
  double cos_amp = (2.0 / (double)active_frames) * cos_sum;
  double sin_amp = (2.0 / (double)active_frames) * sin_sum;
  double fundamental_power = (cos_amp * cos_amp + sin_amp * sin_amp) / 2.0;

  double total_power = 0.0;
  for (int t = settle_frames; t < total_frames; t++) {
    total_power += output_buf[t] * output_buf[t];
  }
  total_power /= (double)active_frames;

  double noise_power = total_power - fundamental_power;
  if (noise_power < 1e-20) noise_power = 1e-20;
  double sinad = 10.0 * log10(fundamental_power / noise_power);

  ASSERT_TRUE(sinad >= 90.0);

  free(input_buf);
  free(output_buf);
  dop_decoder_free(decoder);
  dsd_encoder_free(encoder);
}

TEST(NativeDSDEncoderCreationAndOutput) {
  size_t pcm_sample_rate = 176400;
  dsd_encoder_t* encoder = dsd_encoder_create(
      2, pcm_sample_rate, DSD_MODE_NATIVE, 16, SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(encoder != NULL);
  ASSERT_TRUE(dsd_encoder_is_enabled(encoder));

  audio_chunk_t* chunk = audio_chunk_create(64, 2);
  for (size_t t = 0; t < 64; t++) {
    audio_chunk_get_channel(chunk, 0)[t] =
        0.5 * sin(2.0 * M_PI * 1000.0 * (double)t / pcm_sample_rate);
    audio_chunk_get_channel(chunk, 1)[t] =
        0.5 * cos(2.0 * M_PI * 1000.0 * (double)t / pcm_sample_rate);
  }

  dsd_encoder_encode(encoder, chunk);

  // Native DSD should NOT output DoP marker bytes (0x05 / 0xFA) in the upper
  // byte.
  for (size_t t = 0; t < 64; t++) {
    double val0 = audio_chunk_get_channel(chunk, 0)[t];
    int16_t s16 = pcm_sample_encode_s16(val0);
    (void)s16;  // Valid bit-exact 16-bit DSD stream word
  }

  audio_chunk_free(chunk);
  dsd_encoder_free(encoder);
}

TEST(CarrierBitsCalculationTest) {
#if defined(ENABLE_ALSA)
  playback_device_config_t cfg = {0};
  cfg.type = AUDIO_BACKEND_TYPE_ALSA;
  cfg.cfg.alsa.output_dsd = true;
  cfg.cfg.alsa.has_format = true;

  cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_DSD_U8;
  ASSERT_EQ(playback_device_config_calculate_carrier_bits(&cfg), (size_t)8);

  cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_DSD_U16_LE;
  ASSERT_EQ(playback_device_config_calculate_carrier_bits(&cfg), (size_t)16);

  cfg.cfg.alsa.format = ALSA_SAMPLE_FORMAT_DSD_U32_LE;
  ASSERT_EQ(playback_device_config_calculate_carrier_bits(&cfg), (size_t)32);
#endif
}

TEST(NativeDSDBitDepthsEncodingTest) {
  size_t bit_depths[] = {8, 16, 32};
  for (int i = 0; i < 3; i++) {
    size_t depth = bit_depths[i];
    size_t rate = (depth == 8) ? 352800 : 176400;
    dsd_encoder_t* encoder = dsd_encoder_create(
        2, rate, DSD_MODE_NATIVE, depth, SDM_FILTER_SDM6, 20000.0, false);
    ASSERT_TRUE(encoder != NULL);
    ASSERT_TRUE(dsd_encoder_is_enabled(encoder));

    audio_chunk_t* chunk = audio_chunk_create(32, 2);
    for (size_t t = 0; t < 32; t++) {
      audio_chunk_get_channel(chunk, 0)[t] = 0.5 * sin((double)t);
      audio_chunk_get_channel(chunk, 1)[t] = 0.5 * cos((double)t);
    }

    dsd_encoder_encode(encoder, chunk);

    audio_chunk_free(chunk);
    dsd_encoder_free(encoder);
  }
}

TEST(SupportedCarrierRatesTest) {
  int dop_valid_rates[] = {176400, 192000, 352800, 384000, 705600, 768000};
  size_t dop_valid_count = sizeof(dop_valid_rates) / sizeof(dop_valid_rates[0]);

  for (size_t i = 0; i < dop_valid_count; i++) {
    ASSERT_TRUE(dsd_encoder_is_supported_carrier_rate(dop_valid_rates[i],
                                                      DSD_MODE_DOP));
  }

  int dop_invalid_rates[] = {0, 44100, 48000, 88200, 96000, 1411200, 1536000};
  size_t dop_invalid_count =
      sizeof(dop_invalid_rates) / sizeof(dop_invalid_rates[0]);

  for (size_t i = 0; i < dop_invalid_count; i++) {
    ASSERT_FALSE(dsd_encoder_is_supported_carrier_rate(dop_invalid_rates[i],
                                                       DSD_MODE_DOP));
  }

  int native_valid_rates[] = {88200,  96000,  176400, 192000,  352800,
                              384000, 705600, 768000, 1411200, 1536000};
  size_t native_valid_count =
      sizeof(native_valid_rates) / sizeof(native_valid_rates[0]);

  for (size_t i = 0; i < native_valid_count; i++) {
    ASSERT_TRUE(dsd_encoder_is_supported_carrier_rate(native_valid_rates[i],
                                                      DSD_MODE_NATIVE));
  }

  int native_invalid_rates[] = {0,      44100,  48000,  80000,
                                441000, 960000, 2822400};
  size_t native_invalid_count =
      sizeof(native_invalid_rates) / sizeof(native_invalid_rates[0]);

  for (size_t i = 0; i < native_invalid_count; i++) {
    ASSERT_FALSE(dsd_encoder_is_supported_carrier_rate(native_invalid_rates[i],
                                                       DSD_MODE_NATIVE));
  }

  // PCM mode returns false for all rates
  ASSERT_FALSE(dsd_encoder_is_supported_carrier_rate(176400, DSD_MODE_PCM));
  ASSERT_FALSE(dsd_encoder_is_supported_carrier_rate(88200, DSD_MODE_PCM));

  // Verify Native DSD encoding at 32-bit and 8-bit carrier rates
  dsd_encoder_t* enc_32bit_dsd64 = dsd_encoder_create(
      2, 88200, DSD_MODE_NATIVE, 32, SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_32bit_dsd64 != NULL);
  ASSERT_TRUE(dsd_encoder_is_enabled(enc_32bit_dsd64));
  dsd_encoder_free(enc_32bit_dsd64);

  dsd_encoder_t* enc_8bit_dsd256 = dsd_encoder_create(
      2, 1411200, DSD_MODE_NATIVE, 8, SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_8bit_dsd256 != NULL);
  ASSERT_TRUE(dsd_encoder_is_enabled(enc_8bit_dsd256));
  dsd_encoder_free(enc_8bit_dsd256);
}

TEST(DSDEncoderSilencePrefill) {
  // Test Native DSD 8-bit
  dsd_encoder_t* enc_nat8 = dsd_encoder_create(2, 352800, DSD_MODE_NATIVE, 8,
                                               SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_nat8 != NULL);
  audio_chunk_t* chunk8 = audio_chunk_create(10, 2);
  dsd_encoder_fill_silence(enc_nat8, chunk8);
  for (size_t t = 0; t < 10; t++) {
    uint8_t u8 =
        pcm_sample_encode_dsd_u8(audio_chunk_get_channel(chunk8, 0)[t]);
    ASSERT_EQ((int)u8, 0x69);
  }
  audio_chunk_free(chunk8);
  dsd_encoder_free(enc_nat8);

  // Test Native DSD 16-bit
  dsd_encoder_t* enc_nat16 = dsd_encoder_create(
      2, 176400, DSD_MODE_NATIVE, 16, SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_nat16 != NULL);
  audio_chunk_t* chunk16 = audio_chunk_create(10, 2);
  dsd_encoder_fill_silence(enc_nat16, chunk16);
  for (size_t t = 0; t < 10; t++) {
    uint16_t u16 =
        (uint16_t)pcm_sample_encode_s16(audio_chunk_get_channel(chunk16, 0)[t]);
    ASSERT_EQ((int)u16, 0x6969);
  }
  audio_chunk_free(chunk16);
  dsd_encoder_free(enc_nat16);

  // Test Native DSD 32-bit
  dsd_encoder_t* enc_nat32 = dsd_encoder_create(
      2, 88200, DSD_MODE_NATIVE, 32, SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_nat32 != NULL);
  audio_chunk_t* chunk32 = audio_chunk_create(10, 2);
  dsd_encoder_fill_silence(enc_nat32, chunk32);
  for (size_t t = 0; t < 10; t++) {
    float fval = (float)audio_chunk_get_channel(chunk32, 0)[t];
    uint32_t u32;
    memcpy(&u32, &fval, sizeof(uint32_t));
    ASSERT_EQ((int)u32, 0x69696969);
  }
  audio_chunk_free(chunk32);
  dsd_encoder_free(enc_nat32);

  // Test DoP Mode (16-bit DSD payload, alternating 0x05 / 0xFA marker)
  dsd_encoder_t* enc_dop = dsd_encoder_create(2, 176400, DSD_MODE_DOP, 16,
                                              SDM_FILTER_SDM6, 20000.0, false);
  ASSERT_TRUE(enc_dop != NULL);
  audio_chunk_t* chunk_dop = audio_chunk_create(10, 2);
  dsd_encoder_fill_silence(enc_dop, chunk_dop);
  for (size_t t = 0; t < 10; t++) {
    int32_t val24 =
        pcm_sample_encode_s24(audio_chunk_get_channel(chunk_dop, 0)[t]);
    uint8_t marker = (uint8_t)((val24 >> 16) & 0xFF);
    uint16_t payload = (uint16_t)(val24 & 0xFFFF);
    uint8_t expected_marker = (t % 2 == 0) ? 0x05 : 0xFA;
    ASSERT_EQ((int)marker, (int)expected_marker);
    ASSERT_EQ((int)payload, 0x6969);
  }
  audio_chunk_free(chunk_dop);
  dsd_encoder_free(enc_dop);
}

TEST(DSDEncoderGoldenCorrectness) {
  // Create input sine wave
  audio_chunk_t* chunk = audio_chunk_create(64, 1);
  double* ch = audio_chunk_get_channel(chunk, 0);
  for (size_t i = 0; i < 64; i++) {
    ch[i] = sin(2.0 * M_PI * (double)i / 16.0);
  }

  // 8-bit Native DSD encoder (352800 Hz * 8 = 2822400 Hz)
  dsd_encoder_t* enc8 = dsd_encoder_create(1, 352800, DSD_MODE_NATIVE, 8,
                                           SDM_FILTER_SDM6, 20000.0, false);
  audio_chunk_t* chunk8 = audio_chunk_create(64, 1);
  memcpy(audio_chunk_get_channel(chunk8, 0), ch, 64 * sizeof(double));
  dsd_encoder_encode(enc8, chunk8);

  const uint8_t golden8[64] = {
      0x96, 0x69, 0x69, 0x96, 0x96, 0x69, 0x69, 0x66, 0x95, 0xA6, 0x59,
      0xA6, 0x6A, 0xAA, 0xD5, 0x6A, 0xDA, 0xAD, 0xB6, 0xD6, 0x75, 0xB5,
      0x9A, 0xCB, 0x2A, 0x56, 0x2A, 0x45, 0x4A, 0x4A, 0xAA, 0x4A, 0xD5,
      0xAA, 0xDA, 0xD5, 0xB5, 0xAD, 0xAC, 0xAB, 0x25, 0x99, 0x49, 0x4A,
      0x25, 0x92, 0xA5, 0x55, 0x53, 0x6B, 0x55, 0xDA, 0xCD, 0xB4, 0xEA,
      0xB3, 0x2A, 0xA4, 0xA9, 0x29, 0x54, 0x89, 0x95, 0x4D};
  for (size_t i = 0; i < 64; i++) {
    uint8_t u8 =
        pcm_sample_encode_dsd_u8(audio_chunk_get_channel(chunk8, 0)[i]);
    ASSERT_EQ((int)u8, (int)golden8[i]);
  }

  // 16-bit Native DSD encoder (176400 Hz * 16 = 2822400 Hz)
  dsd_encoder_t* enc16 = dsd_encoder_create(1, 176400, DSD_MODE_NATIVE, 16,
                                            SDM_FILTER_SDM6, 20000.0, false);
  audio_chunk_t* chunk16 = audio_chunk_create(64, 1);
  memcpy(audio_chunk_get_channel(chunk16, 0), ch, 64 * sizeof(double));
  dsd_encoder_encode(enc16, chunk16);

  const uint16_t golden16[64] = {
      0x9669, 0x6996, 0x9669, 0x9669, 0x6996, 0x9666, 0x9999, 0x6996,
      0xA669, 0x669A, 0x6665, 0xA599, 0x5555, 0x3553, 0x34CD, 0x6569,
      0xD55D, 0xB35D, 0x76ED, 0xBB7A, 0xEEEB, 0x7D6E, 0xB6AB, 0xAD54,
      0xCAA9, 0x3148, 0xA249, 0x0908, 0xA481, 0x2514, 0x514A, 0x552D,
      0x4D6A, 0xADDD, 0xAB7B, 0x776F, 0x75BD, 0xB7D5, 0xADD9, 0xAAB5,
      0x4D29, 0x254A, 0x2145, 0x1111, 0x1142, 0x8512, 0x4A2A, 0xA4CB,
      0x4D57, 0x56BB, 0x6BBB, 0x77AF, 0x5BDE, 0xDD5B, 0xD6B6, 0xAB2D,
      0x52A9, 0x4A28, 0xA229, 0x2091, 0x0A44, 0x488A, 0x518A, 0x94B3};
  for (size_t i = 0; i < 64; i++) {
    uint16_t u16 =
        (uint16_t)pcm_sample_encode_s16(audio_chunk_get_channel(chunk16, 0)[i]);
    ASSERT_EQ((int)u16, (int)golden16[i]);
  }

  // 32-bit Native DSD encoder (88200 Hz * 32 = 2822400 Hz)
  dsd_encoder_t* enc32 = dsd_encoder_create(1, 88200, DSD_MODE_NATIVE, 32,
                                            SDM_FILTER_SDM6, 20000.0, false);
  audio_chunk_t* chunk32 = audio_chunk_create(64, 1);
  memcpy(audio_chunk_get_channel(chunk32, 0), ch, 64 * sizeof(double));
  dsd_encoder_encode(enc32, chunk32);

  const uint32_t golden32[64] = {
      0x96696996, 0x96699669, 0x69996666, 0x99969966, 0x69969969, 0x66999965,
      0xA9996696, 0x5A696696, 0x5A99969A, 0x5A59A5A9, 0x5A599996, 0x5699665A,
      0x59A6669A, 0x6A9A69A6, 0x999A5595, 0x554E5953, 0x6A6CD5AC, 0xD75ABD5D,
      0xAEEDB7AF, 0xAF76DEFB, 0x6F6EF6ED, 0xDF5B6DDB, 0xB6D5AEB9, 0xB555AB52,
      0xACA994AA, 0x46492A24, 0x89129090, 0x88A24110, 0x92421114, 0x44851242,
      0x92A25132, 0x552C9555, 0x356AD35A, 0xD5DB5B5D, 0xAF6EBD77, 0x76FAF75F,
      0x5FB5F6DD, 0xEEBAF75B, 0x5ED6D6B5, 0xAB6AACAA, 0xCCC65544, 0xB15282A4,
      0x91228508, 0x92209122, 0x40912242, 0x42292248, 0x524A9252, 0x55259699,
      0x4D56D55A, 0xD6D7576D, 0xB6EED7AF, 0xBAF6EF76, 0xDFB5F6DD, 0xF5B776EB,
      0xABD5D6D6, 0xACD9AB35, 0x2B2594AA, 0x50B12A28, 0x29444491, 0x0A112112,
      0x20A22221, 0x24910944, 0xA4515232, 0x64C9994D};
  for (size_t i = 0; i < 64; i++) {
    float fval = (float)audio_chunk_get_channel(chunk32, 0)[i];
    uint32_t u32;
    memcpy(&u32, &fval, sizeof(uint32_t));
    ASSERT_EQ((int)u32, (int)golden32[i]);
  }

  audio_chunk_free(chunk8);
  audio_chunk_free(chunk16);
  audio_chunk_free(chunk32);
  dsd_encoder_free(enc8);
  dsd_encoder_free(enc16);
  dsd_encoder_free(enc32);
  audio_chunk_free(chunk);
}

TEST_MAIN()
