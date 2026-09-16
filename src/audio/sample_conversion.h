/**
 * @file sample_conversion.h
 * @brief Bit-exact helper functions for converting between normalized
 * double/float audio samples [-1.0, 1.0] and standard audio sample formats
 * (S16, S24, S32, F32, F64, DSD).
 */

#ifndef CLIB_AUDIO_SAMPLE_CONVERSION_H
#define CLIB_AUDIO_SAMPLE_CONVERSION_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// MARK: - General Utilities

/**
 * @brief Clamp a floating-point sample to the [-1.0, 1.0] range.
 * Non-finite values (NaN, Inf) are clamped to 0.0.
 *
 * @param val Input sample value.
 * @return Clamped double value in [-1.0, 1.0].
 */
static inline double pcm_clamp_sample(double val) {
  if (!isfinite(val))
    return 0.0;
  if (val > 1.0)
    return 1.0;
  if (val < -1.0)
    return -1.0;
  return val;
}

/**
 * @brief Reverse the bit order of an 8-bit unsigned integer (MSB <-> LSB).
 *
 * @param b Input byte.
 * @return Byte with reversed bits.
 */
static inline uint8_t pcm_reverse_bits_u8(uint8_t b) {
  b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
  return b;
}

// MARK: - 16-Bit Signed Integer Format (S16)

/**
 * @brief Encode a normalized double sample to a 16-bit signed integer [-32768,
 * 32767].
 *
 * Rounding is truncation toward zero, matching upstream: `audioadapter`'s
 * `from_scaled_float` scales by 2^15 and converts with
 * `num_traits::ToPrimitive::to_i16`, which truncates and only reports
 * out-of-range (→ clamp) values. Upstream pins `0.1 -> 0x0CCC`.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 16-bit signed integer.
 */
static inline int16_t pcm_sample_encode_s16(double val) {
  val = pcm_clamp_sample(val);
  int32_t ival = (int32_t)(val * 32768.0);
  if (ival > 32767)
    ival = 32767;
  else if (ival < -32768)
    ival = -32768;
  return (int16_t)ival;
}

/**
 * @brief Decode a 16-bit signed integer to a normalized double sample.
 *
 * @param val Encoded 16-bit signed integer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s16(int16_t val) {
  return (double)val / 32768.0;
}

/**
 * @brief Encode a normalized double sample into a 2-byte 16-bit little-endian
 * buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 2-byte output buffer.
 */
static inline void pcm_sample_encode_s16_bytes(double val, uint8_t *dst) {
  int16_t s16 = pcm_sample_encode_s16(val);
  dst[0] = (uint8_t)((uint16_t)s16 & 0xFF);
  dst[1] = (uint8_t)(((uint16_t)s16 >> 8) & 0xFF);
}

/**
 * @brief Decode a 2-byte little-endian 16-bit signed integer buffer to a
 * normalized double sample.
 *
 * @param src Source 2-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s16_bytes(const uint8_t *src) {
  int16_t val = (int16_t)(src[0] | (src[1] << 8));
  return pcm_sample_decode_s16(val);
}

// MARK: - 32-Bit Signed Integer Format (S32)

/**
 * @brief Encode a normalized double sample to a 32-bit signed integer
 * [-2147483648, 2147483647].
 *
 * Rounding is truncation toward zero, matching `ToPrimitive::to_i32` in
 * upstream's `audioadapter` conversion.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 32-bit signed integer.
 */
static inline int32_t pcm_sample_encode_s32(double val) {
  val = pcm_clamp_sample(val);
  int64_t val64 = (int64_t)(val * 2147483648.0);
  if (val64 > 2147483647)
    val64 = 2147483647;
  else if (val64 < -2147483648LL)
    val64 = -2147483648LL;
  return (int32_t)val64;
}

/**
 * @brief Decode a 32-bit signed integer to a normalized double sample.
 *
 * @param val Encoded 32-bit signed integer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s32(int32_t val) {
  return (double)val / 2147483648.0;
}

/**
 * @brief Encode a normalized double sample into a 4-byte 32-bit little-endian
 * buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_s32_bytes(double val, uint8_t *dst) {
  int32_t s32 = pcm_sample_encode_s32(val);
  uint32_t u32 = (uint32_t)s32;
  dst[0] = (uint8_t)(u32 & 0xFF);
  dst[1] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[3] = (uint8_t)((u32 >> 24) & 0xFF);
}

/**
 * @brief Decode a 4-byte little-endian 32-bit signed integer buffer to a
 * normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s32_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) |
                 ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  return pcm_sample_decode_s32((int32_t)u32);
}

// MARK: - 24-Bit Signed Integer Format (S24)

/**
 * @brief Encode a normalized double sample to a 24-bit signed integer
 * [-8388608, 8388607].
 *
 * Reproduces upstream's two-step conversion exactly. `audioadapter` represents
 * every 24-bit format as a *left-justified* `i32`: it scales by 2^31 and
 * truncates toward zero (`ToPrimitive::to_i32`), then keeps only the top three
 * bytes, which is an arithmetic `>> 8` and therefore **floors**. The two steps
 * are not interchangeable with a single `floor(val * 2^23)` — truncating first
 * can move the value across a 24-bit boundary — so both are performed here.
 * Upstream pins `0.1 -> 0x0CCCCC` and `-0.1 -> 0xF33333`.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 24-bit signed integer (right-justified in 32-bit output).
 */
static inline int32_t pcm_sample_encode_s24(double val) {
  val = pcm_clamp_sample(val);
  int64_t val32 = (int64_t)(val * 2147483648.0);
  if (val32 > 2147483647)
    val32 = 2147483647;
  else if (val32 < -2147483648LL)
    val32 = -2147483648LL;
  return (int32_t)(val32 >> 8);
}

/**
 * @brief Decode a 24-bit signed integer [-8388608, 8388607] to a normalized
 * double sample.
 *
 * @param val24 Encoded 24-bit signed integer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24(int32_t val24) {
  if (val24 & 0x00800000) {
    val24 |= (int32_t)0xFF000000;
  }
  return (double)val24 / 8388608.0;
}

/**
 * @brief Encode a normalized double sample into a 32-bit container with valid
 * bits in MSB 24 bits.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 24-bit left-justified signed integer.
 */
static inline int32_t pcm_sample_encode_s24_msb(double val) {
  int32_t val24 = pcm_sample_encode_s24(val);
  return (int32_t)((uint32_t)val24 << 8);
}

/**
 * @brief Encode a normalized double sample into a 3-byte packed little-endian
 * buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 3-byte output buffer.
 */
static inline void pcm_sample_encode_s24_3bytes(double val, uint8_t *dst) {
  int32_t val24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)(val24 & 0xFF);
  dst[1] = (uint8_t)((val24 >> 8) & 0xFF);
  dst[2] = (uint8_t)((val24 >> 16) & 0xFF);
}

/**
 * @brief Decode a 3-byte packed little-endian 24-bit integer to a normalized
 * double sample.
 *
 * @param src Source 3-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24_3bytes(const uint8_t *src) {
  int32_t val24 = (int32_t)(src[0] | (src[1] << 8) | (src[2] << 16));
  if (val24 & 0x800000)
    val24 |= (int32_t)0xFF000000;
  return (double)val24 / 8388608.0;
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit right-justified
 * little-endian buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_s24_4_rj_bytes(double val, uint8_t *dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)(s24 & 0xFF);
  dst[1] = (uint8_t)((s24 >> 8) & 0xFF);
  dst[2] = (uint8_t)((s24 >> 16) & 0xFF);
  dst[3] = 0;
}

/**
 * @brief Decode a 4-byte 24-bit right-justified little-endian integer buffer to
 * a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24_4_rj_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) |
                 ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  int32_t val = (int32_t)(u32 << 8) >> 8;
  return pcm_sample_decode_s24(val);
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit left-justified
 * little-endian buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_s24_4_lj_bytes(double val, uint8_t *dst) {
  int32_t s24 = pcm_sample_encode_s24_msb(val);
  uint32_t u32 = (uint32_t)s24;
  dst[0] = (uint8_t)(u32 & 0xFF);
  dst[1] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[3] = (uint8_t)((u32 >> 24) & 0xFF);
}

/**
 * @brief Decode a 4-byte 24-bit left-justified little-endian integer buffer to
 * a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24_4_lj_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) |
                 ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  int32_t val = (int32_t)(u32 & (uint32_t)0xFFFFFF00);
  return pcm_sample_decode_s32(val);
}

// MARK: - 32-Bit Floating-Point Format (F32)

/**
 * @brief Encode a normalized double sample to a 32-bit float.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 32-bit float value.
 */
static inline float pcm_sample_encode_f32(double val) {
  return isfinite(val) ? (float)val : 0.0f;
}

/**
 * @brief Decode a 32-bit float to a normalized double sample.
 * Non-finite values (NaN, Inf) are replaced with 0.0.
 *
 * @param val Input 32-bit float value.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f32(float val) {
  return isfinite(val) ? (double)val : 0.0;
}

/**
 * @brief Reinterpret a 32-bit uint32_t raw bit pattern as a 32-bit float.
 *
 * @param bits Raw uint32_t bit pattern.
 * @return 32-bit float value.
 */
static inline float pcm_sample_f32_from_u32(uint32_t bits) {
  float fval;
  memcpy(&fval, &bits, sizeof(float));
  return fval;
}

/**
 * @brief Reinterpret a 32-bit float as a 32-bit uint32_t raw bit pattern.
 *
 * @param fval 32-bit float value.
 * @return Raw uint32_t bit pattern.
 */
static inline uint32_t pcm_sample_u32_from_f32(float fval) {
  uint32_t bits;
  memcpy(&bits, &fval, sizeof(uint32_t));
  return bits;
}

/**
 * @brief Encode a normalized double sample into a 4-byte float buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_f32_bytes(double val, uint8_t *dst) {
  float fval = pcm_sample_encode_f32(val);
  uint32_t u32 = pcm_sample_u32_from_f32(fval);
  dst[0] = (uint8_t)(u32 & 0xFF);
  dst[1] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[3] = (uint8_t)((u32 >> 24) & 0xFF);
}

/**
 * @brief Decode a 4-byte float buffer to a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f32_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)src[0]) | ((uint32_t)src[1] << 8) |
                 ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  float fval = pcm_sample_f32_from_u32(u32);
  return pcm_sample_decode_f32(fval);
}

/**
 * @brief Encode a double sample containing 32 raw DSD bits back to uint32_t.
 */
static inline uint32_t pcm_sample_encode_dsd_u32(double val) {
  if (val <= 0.0)
    return 0;
  if (val >= 4294967295.0)
    return 0xFFFFFFFFU;
  return (uint32_t)(uint64_t)val;
}

/**
 * @brief Decode a raw 32-bit DSD container bit pattern to an exact
 * integer-valued double sample.
 */
static inline double pcm_sample_decode_dsd_u32(uint32_t bits) {
  return (double)bits;
}

// MARK: - 64-Bit Floating-Point Format (F64)

/**
 * @brief Encode a normalized double sample to a 64-bit float.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Clamped 64-bit float value.
 */
static inline double pcm_sample_encode_f64(double val) {
  return isfinite(val) ? val : 0.0;
}

/**
 * @brief Decode a 64-bit double to a normalized double sample.
 * Non-finite values (NaN, Inf) are replaced with 0.0.
 *
 * @param val Input 64-bit double value.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f64(double val) {
  return isfinite(val) ? val : 0.0;
}

/**
 * @brief Encode a normalized double sample into an 8-byte double buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 8-byte output buffer.
 */
static inline void pcm_sample_encode_f64_bytes(double val, uint8_t *dst) {
  double dval = pcm_sample_encode_f64(val);
  uint64_t u64;
  memcpy(&u64, &dval, sizeof(uint64_t));
  for (int i = 0; i < 8; i++) {
    dst[i] = (uint8_t)((u64 >> (i * 8)) & 0xFF);
  }
}

/**
 * @brief Decode an 8-byte double buffer to a normalized double sample.
 * Non-finite values (NaN, Inf) are replaced with 0.0.
 *
 * @param src Source 8-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f64_bytes(const uint8_t *src) {
  uint64_t u64 = 0;
  for (int i = 0; i < 8; i++) {
    u64 |= ((uint64_t)src[i]) << (i * 8);
  }
  double dval;
  memcpy(&dval, &u64, sizeof(double));
  return pcm_sample_decode_f64(dval);
}

// MARK: - Direct Stream Digital Format (DSD)

/**
 * @brief Encode a normalized double sample to an 8-bit DSD byte (MSB upper
 * byte).
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 8-bit DSD byte.
 */
static inline uint8_t pcm_sample_encode_dsd_u8(double val) {
  int16_t s16 = pcm_sample_encode_s16(val);
  return (uint8_t)((uint16_t)s16 >> 8);
}

/**
 * @brief Decode an 8-bit DSD byte (MSB upper byte) to normalized double
 * [-1.0, 1.0].
 *
 * @param u8 Input 8-bit DSD byte.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_dsd_u8(uint8_t u8) {
  return pcm_sample_decode_s16((int16_t)((uint16_t)u8 << 8));
}

/**
 * @brief Encode a normalized double sample to a 16-bit DSD byte pair in
 * little-endian byte order.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 2-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u16_le_bytes(double val,
                                                      uint8_t *dst) {
  uint16_t u16 = (uint16_t)pcm_sample_encode_s16(val);
  dst[0] = (uint8_t)(u16 & 0xFF);
  dst[1] = (uint8_t)((u16 >> 8) & 0xFF);
}

/**
 * @brief Decode a 2-byte little-endian 16-bit DSD buffer to a normalized double
 * sample.
 *
 * @param src Source 2-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_dsd_u16_le_bytes(const uint8_t *src) {
  uint16_t u16 = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
  return pcm_sample_decode_s16((int16_t)u16);
}

/**
 * @brief Encode a normalized double sample to a 16-bit DSD byte pair in
 * big-endian byte order.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 2-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u16_be_bytes(double val,
                                                      uint8_t *dst) {
  uint16_t u16 = (uint16_t)pcm_sample_encode_s16(val);
  dst[0] = (uint8_t)((u16 >> 8) & 0xFF);
  dst[1] = (uint8_t)(u16 & 0xFF);
}

/**
 * @brief Decode a 2-byte big-endian 16-bit DSD buffer to a normalized double
 * sample.
 *
 * @param src Source 2-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_dsd_u16_be_bytes(const uint8_t *src) {
  uint16_t u16 = ((uint16_t)src[0] << 8) | (uint16_t)src[1];
  return pcm_sample_decode_s16((int16_t)u16);
}

/**
 * @brief Encode a normalized sample containing 32 raw DSD bits into a 4-byte
 * buffer in little-endian byte order.
 *
 * @param val The double-precision sample containing packed 32 DSD bits.
 * @param dst Target 4-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u32_le_bytes(double val,
                                                      uint8_t *dst) {
  uint32_t u32 = pcm_sample_encode_dsd_u32(val);
  dst[0] = (uint8_t)(u32 & 0xFF);
  dst[1] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[3] = (uint8_t)((u32 >> 24) & 0xFF);
}

/**
 * @brief Decode a 4-byte little-endian buffer containing 32 raw DSD bits into
 * a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample containing packed 32 DSD bits.
 */
static inline double pcm_sample_decode_dsd_u32_le_bytes(const uint8_t *src) {
  uint32_t u32 = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                 ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
  return pcm_sample_decode_dsd_u32(u32);
}

/**
 * @brief Encode a normalized sample containing 32 raw DSD bits into a 4-byte
 * buffer in big-endian byte order.
 *
 * @param val The double-precision sample containing packed 32 DSD bits.
 * @param dst Target 4-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u32_be_bytes(double val,
                                                      uint8_t *dst) {
  uint32_t u32 = pcm_sample_encode_dsd_u32(val);
  dst[0] = (uint8_t)((u32 >> 24) & 0xFF);
  dst[1] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[3] = (uint8_t)(u32 & 0xFF);
}

/**
 * @brief Decode a 4-byte big-endian buffer containing 32 raw DSD bits into a
 * normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample containing packed 32 DSD bits.
 */
static inline double pcm_sample_decode_dsd_u32_be_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                 ((uint32_t)src[2] << 8) | (uint32_t)src[3];
  return pcm_sample_decode_dsd_u32(u32);
}

/**
 * @brief Encode 32 oversampled DSD bits from a normalized sample into 4 bytes
 * with bit order reversed per byte (LSB bit order).
 *
 * @param val The double-precision sample containing packed 32 DSD bits.
 * @param dst Target 4-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u32_reversed_bytes(double val,
                                                            uint8_t *dst) {
  pcm_sample_encode_dsd_u32_be_bytes(val, dst);
  dst[0] = pcm_reverse_bits_u8(dst[0]);
  dst[1] = pcm_reverse_bits_u8(dst[1]);
  dst[2] = pcm_reverse_bits_u8(dst[2]);
  dst[3] = pcm_reverse_bits_u8(dst[3]);
}

/**
 * @brief Decode 4 bytes (LSB bit order, reversed bits per byte) into a double
 * sample containing 32 raw DSD bits.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample containing packed 32 DSD bits.
 */
static inline double
pcm_sample_decode_dsd_u32_reversed_bytes(const uint8_t *src) {
  uint32_t u32 = ((uint32_t)pcm_reverse_bits_u8(src[0]) << 24) |
                 ((uint32_t)pcm_reverse_bits_u8(src[1]) << 16) |
                 ((uint32_t)pcm_reverse_bits_u8(src[2]) << 8) |
                 (uint32_t)pcm_reverse_bits_u8(src[3]);
  return pcm_sample_decode_dsd_u32(u32);
}

// MARK: - Big-Endian PCM & Float Formats

static inline void pcm_sample_encode_s16_be_bytes(double val, uint8_t *dst) {
  int16_t s16 = pcm_sample_encode_s16(val);
  dst[0] = (uint8_t)(((uint16_t)s16 >> 8) & 0xFF);
  dst[1] = (uint8_t)((uint16_t)s16 & 0xFF);
}

static inline double pcm_sample_decode_s16_be_bytes(const uint8_t *src) {
  int16_t val = (int16_t)(((uint16_t)src[0] << 8) | (uint16_t)src[1]);
  return pcm_sample_decode_s16(val);
}

static inline void pcm_sample_encode_s24_3be_bytes(double val, uint8_t *dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)((s24 >> 16) & 0xFF);
  dst[1] = (uint8_t)((s24 >> 8) & 0xFF);
  dst[2] = (uint8_t)(s24 & 0xFF);
}

static inline double pcm_sample_decode_s24_3be_bytes(const uint8_t *src) {
  int32_t val = ((int32_t)(int8_t)src[0] << 16) | ((int32_t)src[1] << 8) |
                (int32_t)src[2];
  return pcm_sample_decode_s24(val);
}

static inline void pcm_sample_encode_s24_4_rj_be_bytes(double val,
                                                       uint8_t *dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)((s24 < 0) ? 0xFF : 0x00);
  dst[1] = (uint8_t)((s24 >> 16) & 0xFF);
  dst[2] = (uint8_t)((s24 >> 8) & 0xFF);
  dst[3] = (uint8_t)(s24 & 0xFF);
}

static inline double pcm_sample_decode_s24_4_rj_be_bytes(const uint8_t *src) {
  int32_t val = ((int32_t)(int8_t)src[1] << 16) | ((int32_t)src[2] << 8) |
                (int32_t)src[3];
  return pcm_sample_decode_s24(val);
}

static inline void pcm_sample_encode_s24_4_lj_be_bytes(double val,
                                                       uint8_t *dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)((s24 >> 16) & 0xFF);
  dst[1] = (uint8_t)((s24 >> 8) & 0xFF);
  dst[2] = (uint8_t)(s24 & 0xFF);
  dst[3] = 0;
}

static inline double pcm_sample_decode_s24_4_lj_be_bytes(const uint8_t *src) {
  int32_t s32 = ((int32_t)(int8_t)src[0] << 24) | ((int32_t)src[1] << 16) |
                ((int32_t)src[2] << 8) | (int32_t)src[3];
  return pcm_sample_decode_s32(s32);
}

static inline void pcm_sample_encode_s32_be_bytes(double val, uint8_t *dst) {
  int32_t s32 = pcm_sample_encode_s32(val);
  dst[0] = (uint8_t)((s32 >> 24) & 0xFF);
  dst[1] = (uint8_t)((s32 >> 16) & 0xFF);
  dst[2] = (uint8_t)((s32 >> 8) & 0xFF);
  dst[3] = (uint8_t)(s32 & 0xFF);
}

static inline double pcm_sample_decode_s32_be_bytes(const uint8_t *src) {
  int32_t val = ((int32_t)(int8_t)src[0] << 24) | ((int32_t)src[1] << 16) |
                ((int32_t)src[2] << 8) | (int32_t)src[3];
  return pcm_sample_decode_s32(val);
}

static inline void pcm_sample_encode_f32_be_bytes(double val, uint8_t *dst) {
  float f = (float)pcm_clamp_sample(val);
  uint32_t u;
  memcpy(&u, &f, sizeof(float));
  dst[0] = (uint8_t)((u >> 24) & 0xFF);
  dst[1] = (uint8_t)((u >> 16) & 0xFF);
  dst[2] = (uint8_t)((u >> 8) & 0xFF);
  dst[3] = (uint8_t)(u & 0xFF);
}

static inline double pcm_sample_decode_f32_be_bytes(const uint8_t *src) {
  uint32_t u = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
               ((uint32_t)src[2] << 8) | (uint32_t)src[3];
  float f;
  memcpy(&f, &u, sizeof(float));
  return pcm_clamp_sample((double)f);
}

static inline void pcm_sample_encode_f64_be_bytes(double val, uint8_t *dst) {
  double d = pcm_clamp_sample(val);
  uint64_t u;
  memcpy(&u, &d, sizeof(double));
  dst[0] = (uint8_t)((u >> 56) & 0xFF);
  dst[1] = (uint8_t)((u >> 48) & 0xFF);
  dst[2] = (uint8_t)((u >> 40) & 0xFF);
  dst[3] = (uint8_t)((u >> 32) & 0xFF);
  dst[4] = (uint8_t)((u >> 24) & 0xFF);
  dst[5] = (uint8_t)((u >> 16) & 0xFF);
  dst[6] = (uint8_t)((u >> 8) & 0xFF);
  dst[7] = (uint8_t)(u & 0xFF);
}

static inline double pcm_sample_decode_f64_be_bytes(const uint8_t *src) {
  uint64_t u = ((uint64_t)src[0] << 56) | ((uint64_t)src[1] << 48) |
               ((uint64_t)src[2] << 40) | ((uint64_t)src[3] << 32) |
               ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
               ((uint64_t)src[6] << 8) | (uint64_t)src[7];
  double d;
  memcpy(&d, &u, sizeof(double));
  return pcm_clamp_sample(d);
}

#endif // CLIB_AUDIO_SAMPLE_CONVERSION_H
