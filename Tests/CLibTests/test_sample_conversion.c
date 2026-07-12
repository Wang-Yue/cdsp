#include "Audio/sample_conversion.h"
#include "test_support.h"

TEST(SampleConversion_S16_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    int16_t encoded = pcm_sample_encode_s16(samples[i]);
    double decoded = pcm_sample_decode_s16(encoded);
    ASSERT_NEAR(samples[i], decoded, 1e-4);
  }
}

TEST(SampleConversion_S24_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    int32_t encoded = pcm_sample_encode_s24(samples[i]);
    double decoded = pcm_sample_decode_s24(encoded);
    ASSERT_NEAR(samples[i], decoded, 1e-6);

    uint8_t buf3[3];
    pcm_sample_encode_s24_3bytes(samples[i], buf3);
    double decoded_3b = pcm_sample_decode_s24_3bytes(buf3);
    ASSERT_NEAR(samples[i], decoded_3b, 1e-6);
  }
}

TEST(SampleConversion_S32_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    int32_t encoded = pcm_sample_encode_s32(samples[i]);
    double decoded = pcm_sample_decode_s32(encoded);
    ASSERT_NEAR(samples[i], decoded, 1e-9);
  }
}

TEST(SampleConversion_Clipping_16) {
  double inputs[] = {-2.0, 0.0, 2.0};
  double expected[] = {-1.0, 0.0, 32767.0 / 32768.0};

  for (size_t i = 0; i < 3; i++) {
    int16_t enc = pcm_sample_encode_s16(inputs[i]);
    double dec = pcm_sample_decode_s16(enc);
    ASSERT_NEAR(expected[i], dec, 1e-6);
  }
}

TEST(SampleConversion_Clipping_24) {
  double inputs[] = {-2.0, 0.0, 2.0};
  double expected[] = {-1.0, 0.0, 8388607.0 / 8388608.0};

  for (size_t i = 0; i < 3; i++) {
    int32_t enc = pcm_sample_encode_s24(inputs[i]);
    double dec = pcm_sample_decode_s24(enc);
    ASSERT_NEAR(expected[i], dec, 1e-8);
  }
}

TEST(SampleConversion_Clipping_32) {
  double inputs[] = {-2.0, 0.0, 2.0};
  double expected[] = {-1.0, 0.0, 2147483647.0 / 2147483648.0};

  for (size_t i = 0; i < 3; i++) {
    int32_t enc = pcm_sample_encode_s32(inputs[i]);
    double dec = pcm_sample_decode_s32(enc);
    ASSERT_NEAR(expected[i], dec, 1e-12);
  }
}

TEST(SampleConversion_Float32_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    uint8_t buf[4];
    pcm_sample_encode_f32_bytes(samples[i], buf);
    double decoded = pcm_sample_decode_f32_bytes(buf);
    ASSERT_NEAR(samples[i], decoded, 1e-6);
  }
}

TEST(SampleConversion_Float64_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    uint8_t buf[8];
    pcm_sample_encode_f64_bytes(samples[i], buf);
    double decoded = pcm_sample_decode_f64_bytes(buf);
    ASSERT_NEAR(samples[i], decoded, 1e-15);
  }
}

TEST(SampleConversion_NaN_Infinity_Handling) {
  double nan_val = NAN;
  double inf_val = INFINITY;

  int16_t enc_nan16 = pcm_sample_encode_s16(nan_val);
  ASSERT_EQ(0, enc_nan16);

  int32_t enc_nan24 = pcm_sample_encode_s24(nan_val);
  ASSERT_EQ(0, enc_nan24);

  int32_t enc_nan32 = pcm_sample_encode_s32(nan_val);
  ASSERT_EQ(0, enc_nan32);

  double dec_nan_f32 = pcm_sample_decode_f32((float)nan_val);
  ASSERT_NEAR(0.0, dec_nan_f32, 1e-15);

  double dec_inf_f32 = pcm_sample_decode_f32((float)inf_val);
  ASSERT_NEAR(0.0, dec_inf_f32, 1e-15);
}

TEST_MAIN()
