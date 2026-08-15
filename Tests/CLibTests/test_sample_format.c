#include <string.h>

#include "Config/engine_config_types.h"
#include "test_support.h"

#if defined(ENABLE_COREAUDIO)

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
  const char* aliases[] = {"S16LE",  "S24LE",  "S32LE",     "FLOAT32LE",
                           "F32_LE", "S16_LE", "FLOAT64LE", "s16"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(COREAUDIO_SAMPLE_FORMAT_INVALID,
              coreaudio_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  int count = 0;
  for (int i = 0; i < 10; i++) {
    if (coreaudio_sample_format_to_string((coreaudio_sample_format_t)i) !=
            NULL &&
        strcmp(coreaudio_sample_format_to_string((coreaudio_sample_format_t)i),
               "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(4, count);
}

#elif defined(ENABLE_ALSA)

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
  const char* aliases[] = {"S16", "S24",   "S32",   "FLOAT32",
                           "F32", "S16LE", "s16_le"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(ALSA_SAMPLE_FORMAT_INVALID,
              alsa_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  int count = 0;
  for (int i = 0; i < 15; i++) {
    if (alsa_sample_format_to_string((alsa_sample_format_t)i) != NULL &&
        strcmp(alsa_sample_format_to_string((alsa_sample_format_t)i),
               "Invalid") != 0) {
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
  const char* aliases[] = {"S16", "S24",   "S32",   "FLOAT32",
                           "F32", "S16LE", "s16_le"};
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
    ASSERT_EQ(ASIO_SAMPLE_FORMAT_INVALID,
              asio_sample_format_from_string(aliases[i]));
  }
}

TEST(AllCases) {
  int count = 0;
  for (int i = 0; i < 10; i++) {
    if (asio_sample_format_to_string((asio_sample_format_t)i) != NULL &&
        strcmp(asio_sample_format_to_string((asio_sample_format_t)i),
               "Invalid") != 0) {
      count++;
    }
  }
  ASSERT_EQ(7, count);
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
  const char* invalid_backend_names[] = {
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
