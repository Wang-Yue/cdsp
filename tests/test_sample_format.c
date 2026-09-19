#include <string.h>

#include "config/engine_config_types.h"
#include "test_support.h"

#if defined(ENABLE_COREAUDIO)
#include <stdbool.h>

#include "backend/backend_error.h"
#include "backend/core_audio_capabilities.h"
#include "backend/core_audio_device.h"

TEST(CanonicalRawValues) {
  ASSERT_STR_EQ("S16",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S16));
  ASSERT_STR_EQ("S24",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S24));
  ASSERT_STR_EQ("S32",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_S32));
  ASSERT_STR_EQ("F32",
                coreaudio_sample_format_to_string(COREAUDIO_SAMPLE_FORMAT_F32));
}

TEST(DecodesCanonicalNames) {
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S16,
            coreaudio_sample_format_from_string("S16"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S24,
            coreaudio_sample_format_from_string("S24"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_S32,
            coreaudio_sample_format_from_string("S32"));
  ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_F32,
            coreaudio_sample_format_from_string("F32"));
}

TEST(RejectsAliases) {
  const char *aliases[] = {"S16LE",  "S24LE",  "S32LE",     "FLOAT32LE",
                           "F32_LE", "S16_LE", "FLOAT64LE", "s16"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_INVALID,
              coreaudio_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  coreaudio_sample_format_t formats[] = {
      COREAUDIO_SAMPLE_FORMAT_S16,     COREAUDIO_SAMPLE_FORMAT_S24,
      COREAUDIO_SAMPLE_FORMAT_S32,     COREAUDIO_SAMPLE_FORMAT_F32,
      COREAUDIO_SAMPLE_FORMAT_INVALID,
  };
  int count = 0;
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    const char *str = coreaudio_sample_format_to_string(formats[i]);
    if (str != NULL && strcmp(str, "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(4, count);
}

TEST(CoreAudioFormatToBinaryFormat) {
  ASSERT_EQ(
      BINARY_SAMPLE_FORMAT_S16_LE,
      coreaudio_sample_format_to_binary_format(COREAUDIO_SAMPLE_FORMAT_S16));
  ASSERT_EQ(
      BINARY_SAMPLE_FORMAT_S24_4_LJ_LE,
      coreaudio_sample_format_to_binary_format(COREAUDIO_SAMPLE_FORMAT_S24));
  ASSERT_EQ(
      BINARY_SAMPLE_FORMAT_S32_LE,
      coreaudio_sample_format_to_binary_format(COREAUDIO_SAMPLE_FORMAT_S32));
  ASSERT_EQ(
      BINARY_SAMPLE_FORMAT_F32_LE,
      coreaudio_sample_format_to_binary_format(COREAUDIO_SAMPLE_FORMAT_F32));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_INVALID,
            coreaudio_sample_format_to_binary_format(
                COREAUDIO_SAMPLE_FORMAT_INVALID));
}

TEST(CoreAudioASBDForFormatAndBinaryMapping) {
  AudioStreamBasicDescription s16_asbd =
      core_audio_device_asbd_for_format(48000.0, 2, "S16");
  ASSERT_EQ(kAudioFormatLinearPCM, s16_asbd.mFormatID);
  ASSERT_EQ(16, (int)s16_asbd.mBitsPerChannel);
  ASSERT_EQ(4, (int)s16_asbd.mBytesPerFrame);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S16_LE,
            core_audio_device_asbd_to_binary_format(&s16_asbd));

  AudioStreamBasicDescription s24_asbd =
      core_audio_device_asbd_for_format(48000.0, 2, "S24");
  ASSERT_EQ(24, (int)s24_asbd.mBitsPerChannel);
  ASSERT_EQ(8, (int)s24_asbd.mBytesPerFrame);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE,
            core_audio_device_asbd_to_binary_format(&s24_asbd));

  AudioStreamBasicDescription s32_asbd =
      core_audio_device_asbd_for_format(96000.0, 2, "S32");
  ASSERT_EQ(32, (int)s32_asbd.mBitsPerChannel);
  ASSERT_EQ(8, (int)s32_asbd.mBytesPerFrame);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S32_LE,
            core_audio_device_asbd_to_binary_format(&s32_asbd));

  AudioStreamBasicDescription f32_asbd =
      core_audio_device_asbd_for_format(44100.0, 2, "F32");
  ASSERT_EQ(32, (int)f32_asbd.mBitsPerChannel);
  ASSERT_EQ(8, (int)f32_asbd.mBytesPerFrame);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_F32_LE,
            core_audio_device_asbd_to_binary_format(&f32_asbd));

  // 24-bit in 3-byte packed container
  AudioStreamBasicDescription s24_3_asbd = {
      .mSampleRate = 48000.0,
      .mFormatID = kAudioFormatLinearPCM,
      .mFormatFlags = kAudioFormatFlagIsSignedInteger,
      .mBytesPerPacket = 6,
      .mFramesPerPacket = 1,
      .mBytesPerFrame = 6,
      .mChannelsPerFrame = 2,
      .mBitsPerChannel = 24,
  };
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_3_LE,
            core_audio_device_asbd_to_binary_format(&s24_3_asbd));

  // 24-bit in 4-byte container (Left-justified / high bits)
  AudioStreamBasicDescription s24_4_lj_asbd = {
      .mSampleRate = 48000.0,
      .mFormatID = kAudioFormatLinearPCM,
      .mFormatFlags =
          kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsAlignedHigh,
      .mBytesPerPacket = 8,
      .mFramesPerPacket = 1,
      .mBytesPerFrame = 8,
      .mChannelsPerFrame = 2,
      .mBitsPerChannel = 24,
  };
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE,
            core_audio_device_asbd_to_binary_format(&s24_4_lj_asbd));

  // 24-bit in 4-byte container (Right-justified / low bits)
  AudioStreamBasicDescription s24_4_rj_asbd = {
      .mSampleRate = 48000.0,
      .mFormatID = kAudioFormatLinearPCM,
      .mFormatFlags = kAudioFormatFlagIsSignedInteger,
      .mBytesPerPacket = 8,
      .mFramesPerPacket = 1,
      .mBytesPerFrame = 8,
      .mChannelsPerFrame = 2,
      .mBitsPerChannel = 24,
  };
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_RJ_LE,
            core_audio_device_asbd_to_binary_format(&s24_4_rj_asbd));
}

TEST(CoreAudioCapabilitiesUnified) {
  device_error_t err;
  audio_device_descriptor_t *desc =
      core_audio_capabilities_describe("default", false, &err);
  if (desc) {
    ASSERT_EQ(1, (int)desc->capability_sets_count);
    ASSERT_STR_EQ("Unified", desc->capability_sets[0].mode);
    free_audio_device_descriptor(desc);
  }
}

TEST(CoreAudioDeviceSampleRateValidation) {
  ASSERT_FALSE(core_audio_device_is_sample_rate_supported(0, 48000.0));
  ASSERT_FALSE(core_audio_device_is_sample_rate_supported(0, -48000.0));
  ASSERT_FALSE(core_audio_device_is_sample_rate_supported(0, 0.0));

  AudioDeviceID dev_id = core_audio_device_default_id(CORE_AUDIO_SCOPE_OUTPUT);
  if (dev_id != kAudioObjectUnknown) {
    // Outrageous sample rate should be rejected by device available ranges
    ASSERT_FALSE(core_audio_device_is_sample_rate_supported(dev_id, 9999999.0));
  }
}

TEST(CoreAudioHogModeAndFormatSettle) {
  // device_id == 0 should safely no-op / return false without crashing
  core_audio_device_release_hog_mode(0);
  ASSERT_FALSE(core_audio_device_acquire_hog_mode(0));

  // Requesting an impossible physical format on device 0 should fail cleanly
  ASSERT_FALSE(core_audio_device_set_matching_physical_format(
      0, CORE_AUDIO_SCOPE_OUTPUT, 48000.0, "S32", 2));
}

#elif defined(ENABLE_ALSA)
#include <alsa/asoundlib.h>

#include "backend/alsa_device.h"

TEST(ALSABinaryFormatConversions) {
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S16_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_S16_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_3_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_S24_3_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_RJ_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_S24_4_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S32_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_S32_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_F32_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_F32_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_F64_LE,
            alsa_sample_format_to_binary_format(ALSA_SAMPLE_FORMAT_F64_LE));

  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_RJ_LE,
            alsa_pcm_format_to_binary_format(SND_PCM_FORMAT_S24_LE));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_3_LE,
            alsa_pcm_format_to_binary_format(SND_PCM_FORMAT_S24_3LE));
}

TEST(CanonicalRawValues) {
  ASSERT_STR_EQ("S16_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S16_LE));
  ASSERT_STR_EQ("S24_3_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S24_3_LE));
  ASSERT_STR_EQ("S24_4_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S24_4_LE));
  ASSERT_STR_EQ("S32_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_S32_LE));
  ASSERT_STR_EQ("F32_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_F32_LE));
  ASSERT_STR_EQ("F64_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_F64_LE));
  ASSERT_STR_EQ("DSD_U8",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_DSD_U8));
  ASSERT_STR_EQ("DSD_U16_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_DSD_U16_LE));
  ASSERT_STR_EQ("DSD_U16_BE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_DSD_U16_BE));
  ASSERT_STR_EQ("DSD_U32_LE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_DSD_U32_LE));
  ASSERT_STR_EQ("DSD_U32_BE",
                alsa_sample_format_to_string(ALSA_SAMPLE_FORMAT_DSD_U32_BE));
}

TEST(DecodesCanonicalNames) {
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S16_LE,
            alsa_sample_format_from_string("S16_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S24_3_LE,
            alsa_sample_format_from_string("S24_3_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S24_4_LE,
            alsa_sample_format_from_string("S24_4_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_S32_LE,
            alsa_sample_format_from_string("S32_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_F32_LE,
            alsa_sample_format_from_string("F32_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_F64_LE,
            alsa_sample_format_from_string("F64_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_DSD_U8,
            alsa_sample_format_from_string("DSD_U8"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_DSD_U16_LE,
            alsa_sample_format_from_string("DSD_U16_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_DSD_U16_BE,
            alsa_sample_format_from_string("DSD_U16_BE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_DSD_U32_LE,
            alsa_sample_format_from_string("DSD_U32_LE"));
  ASSERT_EQ(ALSA_SAMPLE_FORMAT_DSD_U32_BE,
            alsa_sample_format_from_string("DSD_U32_BE"));
}

TEST(RejectsAliases) {
  const char *aliases[] = {"S16", "S24",   "S32",   "FLOAT32",
                           "F32", "S16LE", "s16_le"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(ALSA_SAMPLE_FORMAT_INVALID,
              alsa_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  alsa_sample_format_t formats[] = {
      ALSA_SAMPLE_FORMAT_S16_LE,     ALSA_SAMPLE_FORMAT_S24_3_LE,
      ALSA_SAMPLE_FORMAT_S24_4_LE,   ALSA_SAMPLE_FORMAT_S32_LE,
      ALSA_SAMPLE_FORMAT_F32_LE,     ALSA_SAMPLE_FORMAT_F64_LE,
      ALSA_SAMPLE_FORMAT_DSD_U8,     ALSA_SAMPLE_FORMAT_DSD_U16_LE,
      ALSA_SAMPLE_FORMAT_DSD_U16_BE, ALSA_SAMPLE_FORMAT_DSD_U32_LE,
      ALSA_SAMPLE_FORMAT_DSD_U32_BE, ALSA_SAMPLE_FORMAT_INVALID,
  };
  int count = 0;
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    const char *str = alsa_sample_format_to_string(formats[i]);
    if (str != NULL && strcmp(str, "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(11, count);
}

#elif defined(ENABLE_ASIO)

TEST(CanonicalRawValues) {
  ASSERT_STR_EQ("S16_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_S16_LE));
  ASSERT_STR_EQ("S24_3_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_S24_3_LE));
  ASSERT_STR_EQ("S24_4_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_S24_4_LE));
  ASSERT_STR_EQ("S32_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_S32_LE));
  ASSERT_STR_EQ("F32_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_F32_LE));
  ASSERT_STR_EQ("F64_LE",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_F64_LE));
  ASSERT_STR_EQ("DSD_INT8",
                asio_sample_format_to_string(ASIO_SAMPLE_FORMAT_DSD_INT8));
}

TEST(DecodesCanonicalNames) {
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_S16_LE,
            asio_sample_format_from_string("S16_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_S24_3_LE,
            asio_sample_format_from_string("S24_3_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_S24_4_LE,
            asio_sample_format_from_string("S24_4_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_S32_LE,
            asio_sample_format_from_string("S32_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_F32_LE,
            asio_sample_format_from_string("F32_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_F64_LE,
            asio_sample_format_from_string("F64_LE"));
  ASSERT_EQ(ASIO_SAMPLE_FORMAT_DSD_INT8,
            asio_sample_format_from_string("DSD_INT8"));
}

TEST(RejectsAliases) {
  const char *aliases[] = {"S16", "S24",   "S32",   "FLOAT32",
                           "F32", "S16LE", "s16_le"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(ASIO_SAMPLE_FORMAT_INVALID,
              asio_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  asio_sample_format_t formats[] = {
      ASIO_SAMPLE_FORMAT_S16_LE,   ASIO_SAMPLE_FORMAT_S24_3_LE,
      ASIO_SAMPLE_FORMAT_S24_4_LE, ASIO_SAMPLE_FORMAT_S32_LE,
      ASIO_SAMPLE_FORMAT_F32_LE,   ASIO_SAMPLE_FORMAT_F64_LE,
      ASIO_SAMPLE_FORMAT_DSD_INT8, ASIO_SAMPLE_FORMAT_INVALID,
  };
  int count = 0;
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
    const char *str = asio_sample_format_to_string(formats[i]);
    if (str != NULL && strcmp(str, "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(7, count);
}

#include "backend/asio_capabilities.h"

TEST(ASIOCapabilitiesNonExistentDevice) {
  device_error_t err;
  audio_device_descriptor_t *desc =
      asio_capabilities_describe("NonExistentAsioDevice12345", false, &err);
  ASSERT_TRUE(desc == NULL);
}

#endif

TEST(AudioBackendTypeCanonical) {
  ASSERT_EQ(AUDIO_BACKEND_TYPE_FILE, audio_backend_type_from_string("File"));
  ASSERT_EQ(AUDIO_BACKEND_TYPE_FILE, audio_backend_type_from_string("RawFile"));
  ASSERT_EQ(AUDIO_BACKEND_TYPE_FILE, audio_backend_type_from_string("WavFile"));
  ASSERT_EQ(AUDIO_BACKEND_TYPE_STDIN_OUT,
            audio_backend_type_from_string("Stdin"));
  ASSERT_EQ(AUDIO_BACKEND_TYPE_STDIN_OUT,
            audio_backend_type_from_string("Stdout"));
  ASSERT_EQ(AUDIO_BACKEND_TYPE_GENERATOR,
            audio_backend_type_from_string("SignalGenerator"));
}

TEST(AudioBackendTypeRejectsAliases) {
  const char *invalid_backend_names[] = {
      "file",      "rawfile",         "wavfile",    "stdin",     "stdout",
      "Generator", "signalgenerator", "Core Audio", "coreaudio", "alsa",
      "ALSA",      "pipewire",        "wasapi",     "asio"};
  for (size_t i = 0;
       i < sizeof(invalid_backend_names) / sizeof(invalid_backend_names[0]);
       i++) {
    ASSERT_EQ(AUDIO_BACKEND_TYPE_INVALID,
              audio_backend_type_from_string(invalid_backend_names[i]));
  }
}

TEST_MAIN()
