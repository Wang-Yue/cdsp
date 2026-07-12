/**
 * @file sample_conversion.h
 * @brief Bit-exact helper functions for converting between normalized
 * double/float audio samples [-1.0, 1.0] and standard audio sample formats
 * (S16, S24, S32, F32, F64).
 */

#ifndef CLIB_AUDIO_SAMPLE_CONVERSION_H
#define CLIB_AUDIO_SAMPLE_CONVERSION_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief Clamp a floating-point sample to the [-1.0, 1.0] range.
 */
static inline double pcm_clamp_sample(double val) {
  if (!isfinite(val)) return 0.0;
  if (val > 1.0) return 1.0;
  if (val < -1.0) return -1.0;
  return val;
}

/**
 * @brief Encode a normalized double sample to a 16-bit signed integer [-32768,
 * 32767].
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
 * @brief Encode a normalized double sample to a 24-bit signed integer
 * [-8388608, 8388607].
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
 * @brief Encode a normalized double sample into a 3-byte packed little-endian
 * buffer.
 */
static inline void pcm_sample_encode_s24_3bytes(double val, uint8_t* dst) {
  int32_t val24 = pcm_sample_encode_s24(val);
  dst[0] = (uint8_t)(val24 & 0xFF);
  dst[1] = (uint8_t)((val24 >> 8) & 0xFF);
  dst[2] = (uint8_t)((val24 >> 16) & 0xFF);
}

/**
 * @brief Encode a normalized double sample into a 32-bit container with valid
 * bits in MSB 24 bits.
 */
static inline int32_t pcm_sample_encode_s24_msb(double val) {
  int32_t val24 = pcm_sample_encode_s24(val);
  return val24 << 8;
}

/**
 * @brief Encode a normalized double sample to a 32-bit signed integer
 * [-2147483648, 2147483647].
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
 * @brief Encode a normalized double sample into a 2-byte 16-bit little-endian
 * buffer.
 */
static inline void pcm_sample_encode_s16_bytes(double val, uint8_t* dst) {
  int16_t s16 = pcm_sample_encode_s16(val);
  memcpy(dst, &s16, sizeof(int16_t));
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit right-justified
 * little-endian buffer.
 */
static inline void pcm_sample_encode_s24_4_rj_bytes(double val, uint8_t* dst) {
  int32_t s24 = pcm_sample_encode_s24(val);
  memcpy(dst, &s24, sizeof(int32_t));
}

/**
 * @brief Encode a normalized double sample into a 4-byte 24-bit left-justified
 * little-endian buffer.
 */
static inline void pcm_sample_encode_s24_4_lj_bytes(double val, uint8_t* dst) {
  int32_t s24 = pcm_sample_encode_s24_msb(val);
  memcpy(dst, &s24, sizeof(int32_t));
}

/**
 * @brief Encode a normalized double sample into a 4-byte 32-bit little-endian
 * buffer.
 */
static inline void pcm_sample_encode_s32_bytes(double val, uint8_t* dst) {
  int32_t s32 = pcm_sample_encode_s32(val);
  memcpy(dst, &s32, sizeof(int32_t));
}

/**
 * @brief Encode a normalized double sample to a 32-bit float.
 */
static inline float pcm_sample_encode_f32(double val) {
  val = pcm_clamp_sample(val);
  return (float)val;
}

/**
 * @brief Encode a normalized double sample into a 4-byte float buffer.
 */
static inline void pcm_sample_encode_f32_bytes(double val, uint8_t* dst) {
  float fval = pcm_sample_encode_f32(val);
  memcpy(dst, &fval, sizeof(float));
}

/**
 * @brief Encode a normalized double sample into an 8-byte double buffer.
 */
static inline void pcm_sample_encode_f64_bytes(double val, uint8_t* dst) {
  val = pcm_clamp_sample(val);
  memcpy(dst, &val, sizeof(double));
}

/**
 * @brief Decode a 16-bit signed integer to a normalized double sample.
 */
static inline double pcm_sample_decode_s16(int16_t val) {
  return (double)val / 32768.0;
}

/**
 * @brief Decode a 2-byte little-endian 16-bit signed integer buffer to a
 * normalized double sample.
 */
static inline double pcm_sample_decode_s16_bytes(const uint8_t* src) {
  int16_t val = (int16_t)(src[0] | (src[1] << 8));
  return pcm_sample_decode_s16(val);
}

/**
 * @brief Decode a 24-bit signed integer [-8388608, 8388607] to a normalized
 * double sample.
 */
static inline double pcm_sample_decode_s24(int32_t val24) {
  return (double)val24 / 8388608.0;
}

/**
 * @brief Decode a 3-byte packed little-endian 24-bit integer to a normalized
 * double sample.
 */
static inline double pcm_sample_decode_s24_3bytes(const uint8_t* src) {
  int32_t val24 = (int32_t)(src[0] | (src[1] << 8) | (src[2] << 16));
  if (val24 & 0x800000) val24 |= (int32_t)0xFF000000;
  return (double)val24 / 8388608.0;
}

/**
 * @brief Decode a 32-bit signed integer to a normalized double sample.
 */
static inline double pcm_sample_decode_s32(int32_t val) {
  return (double)val / 2147483648.0;
}

/**
 * @brief Decode a 4-byte 24-bit right-justified little-endian integer buffer to
 * a normalized double sample.
 */
static inline double pcm_sample_decode_s24_4_rj_bytes(const uint8_t* src) {
  return pcm_sample_decode_s24_3bytes(src);
}

/**
 * @brief Decode a 4-byte 24-bit left-justified little-endian integer buffer to
 * a normalized double sample.
 */
static inline double pcm_sample_decode_s24_4_lj_bytes(const uint8_t* src) {
  int32_t val = (int32_t)((src[1] << 8) | (src[2] << 16) | (src[3] << 24));
  return pcm_sample_decode_s32(val);
}

/**
 * @brief Decode a 4-byte little-endian 32-bit signed integer buffer to a
 * normalized double sample.
 */
static inline double pcm_sample_decode_s32_bytes(const uint8_t* src) {
  int32_t val;
  memcpy(&val, src, sizeof(int32_t));
  return pcm_sample_decode_s32(val);
}

/**
 * @brief Decode a 32-bit float to a normalized double sample. Replace
 * non-finite values with 0.0.
 */
static inline double pcm_sample_decode_f32(float val) {
  return isfinite(val) ? (double)val : 0.0;
}

/**
 * @brief Decode a 4-byte float buffer to a normalized double sample.
 */
static inline double pcm_sample_decode_f32_bytes(const uint8_t* src) {
  float fval;
  memcpy(&fval, src, sizeof(float));
  return pcm_sample_decode_f32(fval);
}

/**
 * @brief Decode an 8-byte double buffer to a normalized double sample. Replace
 * non-finite values with 0.0.
 */
static inline double pcm_sample_decode_f64_bytes(const uint8_t* src) {
  double dval;
  memcpy(&dval, src, sizeof(double));
  return isfinite(dval) ? dval : 0.0;
}

#endif  // CLIB_AUDIO_SAMPLE_CONVERSION_H
