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
  if (!isfinite(val)) return 0.0;
  if (val > 1.0) return 1.0;
  if (val < -1.0) return -1.0;
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
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 16-bit signed integer.
 */
static inline int16_t pcm_sample_encode_s16(double val) {
  val = pcm_clamp_sample(val);
  int32_t ival = (int32_t)lrint(val * 32768.0);
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
static inline void pcm_sample_encode_s16_bytes(double val, uint8_t* dst) {
  int16_t s16 = pcm_sample_encode_s16(val);
  memcpy(dst, &s16, sizeof(int16_t));
}

/**
 * @brief Decode a 2-byte little-endian 16-bit signed integer buffer to a
 * normalized double sample.
 *
 * @param src Source 2-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s16_bytes(const uint8_t* src) {
  int16_t val = (int16_t)(src[0] | (src[1] << 8));
  return pcm_sample_decode_s16(val);
}

// MARK: - 32-Bit Signed Integer Format (S32)

/**
 * @brief Encode a normalized double sample to a 32-bit signed integer
 * [-2147483648, 2147483647].
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 32-bit signed integer.
 */
static inline int32_t pcm_sample_encode_s32(double val) {
  val = pcm_clamp_sample(val);
  int64_t val64 = (int64_t)llrint(val * 2147483648.0);
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
static inline void pcm_sample_encode_s32_bytes(double val, uint8_t* dst) {
  int32_t s32 = pcm_sample_encode_s32(val);
  memcpy(dst, &s32, sizeof(int32_t));
}

/**
 * @brief Decode a 4-byte little-endian 32-bit signed integer buffer to a
 * normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s32_bytes(const uint8_t* src) {
  int32_t val;
  memcpy(&val, src, sizeof(int32_t));
  return pcm_sample_decode_s32(val);
}

// MARK: - 24-Bit Signed Integer Format (S24)

/**
 * @brief Encode a normalized double sample to a 24-bit signed integer
 * [-8388608, 8388607].
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 24-bit signed integer (right-justified in 32-bit output).
 */
static inline int32_t pcm_sample_encode_s24(double val) {
  val = pcm_clamp_sample(val);
  int32_t val24 = (int32_t)lrint(val * 8388608.0);
  if (val24 > 8388607)
    val24 = 8388607;
  else if (val24 < -8388608)
    val24 = -8388608;
  return val24;
}

/**
 * @brief Decode a 24-bit signed integer [-8388608, 8388607] to a normalized
 * double sample.
 *
 * @param val24 Encoded 24-bit signed integer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24(int32_t val24) {
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
  return val24 << 8;
}

/**
 * @brief Encode a normalized double sample into a 3-byte packed little-endian
 * buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 3-byte output buffer.
 */
static inline void pcm_sample_encode_s24_3bytes(double val, uint8_t* dst) {
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
static inline double pcm_sample_decode_s24_3bytes(const uint8_t* src) {
  int32_t val24 = (int32_t)(src[0] | (src[1] << 8) | (src[2] << 16));
  if (val24 & 0x800000) val24 |= (int32_t)0xFF000000;
  return (double)val24 / 8388608.0;
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit right-justified
 * little-endian buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_s24_4_rj_bytes(double val, uint8_t* dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  memcpy(dst, &s24, sizeof(int32_t));
}

/**
 * @brief Decode a 4-byte 24-bit right-justified little-endian integer buffer to
 * a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24_4_rj_bytes(const uint8_t* src) {
  return pcm_sample_decode_s24_3bytes(src);
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit left-justified
 * little-endian buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_s24_4_lj_bytes(double val, uint8_t* dst) {
  int32_t s24 = pcm_sample_encode_s24_msb(val);
  memcpy(dst, &s24, sizeof(int32_t));
}

/**
 * @brief Decode a 4-byte 24-bit left-justified little-endian integer buffer to
 * a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_s24_4_lj_bytes(const uint8_t* src) {
  int32_t val = (int32_t)((src[1] << 8) | (src[2] << 16) | (src[3] << 24));
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
  val = pcm_clamp_sample(val);
  return (float)val;
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
 * @brief Encode a normalized double sample into a 4-byte float buffer.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @param dst Target 4-byte output buffer.
 */
static inline void pcm_sample_encode_f32_bytes(double val, uint8_t* dst) {
  float fval = pcm_sample_encode_f32(val);
  memcpy(dst, &fval, sizeof(float));
}

/**
 * @brief Decode a 4-byte float buffer to a normalized double sample.
 *
 * @param src Source 4-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f32_bytes(const uint8_t* src) {
  float fval;
  memcpy(&fval, src, sizeof(float));
  return pcm_sample_decode_f32(fval);
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
 * @brief Encode a normalized double sample to a 32-bit uint32_t raw bit pattern
 * representing an IEEE 754 float.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Encoded 32-bit uint32_t bit pattern.
 */
static inline uint32_t pcm_sample_encode_f32_u32(double val) {
  float fval = pcm_sample_encode_f32(val);
  return pcm_sample_u32_from_f32(fval);
}

/**
 * @brief Decode a raw 32-bit uint32_t bit pattern representing an IEEE 754
 * float to a normalized double sample. Non-finite values (NaN, Inf) are
 * replaced with 0.0.
 *
 * @param bits Raw 32-bit uint32_t bit pattern.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f32_u32(uint32_t bits) {
  return pcm_sample_decode_f32(pcm_sample_f32_from_u32(bits));
}

// MARK: - 64-Bit Floating-Point Format (F64)

/**
 * @brief Encode a normalized double sample to a 64-bit float.
 *
 * @param val Input double sample in [-1.0, 1.0].
 * @return Clamped 64-bit float value.
 */
static inline double pcm_sample_encode_f64(double val) {
  return pcm_clamp_sample(val);
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
static inline void pcm_sample_encode_f64_bytes(double val, uint8_t* dst) {
  double dval = pcm_sample_encode_f64(val);
  memcpy(dst, &dval, sizeof(double));
}

/**
 * @brief Decode an 8-byte double buffer to a normalized double sample.
 * Non-finite values (NaN, Inf) are replaced with 0.0.
 *
 * @param src Source 8-byte buffer.
 * @return Normalized double sample in [-1.0, 1.0].
 */
static inline double pcm_sample_decode_f64_bytes(const uint8_t* src) {
  double dval;
  memcpy(&dval, src, sizeof(double));
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
 * @brief Encode 32 oversampled DSD bits from a normalized sample into 4 bytes
 * (MSB bit order).
 *
 * @param val The double-precision sample containing packed 32 DSD bits.
 * @param dst Target 4-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u32_bytes(float val, uint8_t* dst) {
  uint32_t u32;
  memcpy(&u32, &val, sizeof(uint32_t));
  dst[0] = (uint8_t)(u32 >> 24);
  dst[1] = (uint8_t)((u32 >> 16) & 0xFF);
  dst[2] = (uint8_t)((u32 >> 8) & 0xFF);
  dst[3] = (uint8_t)(u32 & 0xFF);
}

/**
 * @brief Encode 32 oversampled DSD bits from a normalized sample into 4 bytes
 * with bit order reversed per byte (LSB bit order).
 *
 * @param val The single-precision sample containing packed 32 DSD bits.
 * @param dst Target 4-byte buffer.
 */
static inline void pcm_sample_encode_dsd_u32_reversed_bytes(float val,
                                                            uint8_t* dst) {
  uint32_t u32;
  memcpy(&u32, &val, sizeof(uint32_t));
  dst[0] = pcm_reverse_bits_u8((uint8_t)(u32 >> 24));
  dst[1] = pcm_reverse_bits_u8((uint8_t)((u32 >> 16) & 0xFF));
  dst[2] = pcm_reverse_bits_u8((uint8_t)((u32 >> 8) & 0xFF));
  dst[3] = pcm_reverse_bits_u8((uint8_t)(u32 & 0xFF));
}

#endif  // CLIB_AUDIO_SAMPLE_CONVERSION_H
