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

    uint8_t buf4_rj[4];
    pcm_sample_encode_s24_4_rj_bytes(samples[i], buf4_rj);
    double decoded_4rj = pcm_sample_decode_s24_4_rj_bytes(buf4_rj);
    ASSERT_NEAR(samples[i], decoded_4rj, 1e-6);
    // Verify padding byte (byte 3) is strictly 0 (no sign extension on negative values)
    ASSERT_EQ(0, buf4_rj[3]);

    uint8_t buf4_lj[4];
    pcm_sample_encode_s24_4_lj_bytes(samples[i], buf4_lj);
    double decoded_4lj = pcm_sample_decode_s24_4_lj_bytes(buf4_lj);
    ASSERT_NEAR(samples[i], decoded_4lj, 1e-6);
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

  double dec_nan_f64 = pcm_sample_decode_f64(nan_val);
  ASSERT_NEAR(0.0, dec_nan_f64, 1e-15);

  double dec_inf_f64 = pcm_sample_decode_f64(inf_val);
  ASSERT_NEAR(0.0, dec_inf_f64, 1e-15);
}

TEST(SampleConversion_Utilities) {
  ASSERT_EQ((uint8_t)0b10100011, pcm_reverse_bits_u8(0b11000101));
  ASSERT_NEAR(1.0, pcm_clamp_sample(1.5), 1e-15);
  ASSERT_NEAR(-1.0, pcm_clamp_sample(-2.0), 1e-15);
  ASSERT_NEAR(0.0, pcm_clamp_sample(NAN), 1e-15);
}

TEST(SampleConversion_DSD_U8_RoundTrip) {
  double samples[] = {-0.5, 0.0, 0.5};
  for (size_t i = 0; i < 3; i++) {
    uint8_t encoded = pcm_sample_encode_dsd_u8(samples[i]);
    double decoded = pcm_sample_decode_dsd_u8(encoded);
    ASSERT_NEAR(samples[i], decoded, 1e-4);
  }
}

TEST(SampleConversion_DSD_U32_RoundTrip) {
  uint32_t patterns[] = {0x3F800000, 0xBF800000, 0x00000000}; // 1.0f, -1.0f, 0.0f
  double expected_doubles[] = {1.0, -1.0, 0.0};
  
  for (size_t i = 0; i < 3; i++) {
    double decoded = pcm_sample_decode_dsd_u32(patterns[i]);
    ASSERT_NEAR(expected_doubles[i], decoded, 1e-15);
    
    float val_f = (float)decoded;
    uint8_t buf_msb[4];
    pcm_sample_encode_dsd_u32_bytes(val_f, buf_msb);
    
    // Convert back from bytes to u32 bits
    uint32_t val_msb_bits;
    memcpy(&val_msb_bits, buf_msb, sizeof(uint32_t));
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t val_msb_host = __builtin_bswap32(val_msb_bits);
#else
    uint32_t val_msb_host = val_msb_bits;
#endif
    ASSERT_EQ(patterns[i], val_msb_host);
    
    // LSB reversed bytes
    uint8_t buf_lsb[4];
    pcm_sample_encode_dsd_u32_reversed_bytes(val_f, buf_lsb);
    
    // Check that LSB bytes are indeed bit-reversed versions of MSB bytes
    ASSERT_EQ(pcm_reverse_bits_u8(buf_msb[0]), buf_lsb[0]);
    ASSERT_EQ(pcm_reverse_bits_u8(buf_msb[1]), buf_lsb[1]);
    ASSERT_EQ(pcm_reverse_bits_u8(buf_msb[2]), buf_lsb[2]);
    ASSERT_EQ(pcm_reverse_bits_u8(buf_msb[3]), buf_lsb[3]);
  }
}

TEST_MAIN()
