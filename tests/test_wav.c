#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/sample_conversion.h"
#include "audio/sample_format.h"
#include "test_support.h"
#include "wav/raw_reader.h"
#include "wav/raw_writer.h"
#include "wav/wav_reader.h"
#include "wav/wav_types.h"
#include "wav/wav_writer.h"

TEST(WavWriterAndReader_PCM16) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_pcm16_%d.wav", getpid());
  remove(filename);

  FILE *f = fopen(filename, "wb");
  ASSERT_TRUE(f != NULL);

  uint32_t sample_rate = 48000;
  size_t channels = 2;
  uint32_t num_frames = 100;
  uint32_t data_bytes = num_frames * (uint32_t)channels * 2;

  ASSERT_TRUE(wav_write_header(f, channels, BINARY_SAMPLE_FORMAT_S16_LE,
                               sample_rate, data_bytes, true));

  // Write some 16-bit samples: Left = 0.5 (16384), Right = -0.5 (-16384)
  for (uint32_t i = 0; i < num_frames; i++) {
    int16_t lr[2] = {16384, -16384};
    ASSERT_EQ(2, fwrite(lr, sizeof(int16_t), 2, f));
  }
  fclose(f);

  // Read header via wav_read_info_from_file
  wav_info_t info;
  char err_msg[256] = {0};
  ASSERT_TRUE(
      wav_read_info_from_file(filename, &info, err_msg, sizeof(err_msg)));
  ASSERT_EQ(48000, (int)info.sample_rate);
  ASSERT_EQ(2, (int)info.channels);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S16_LE, info.format);
  ASSERT_EQ(data_bytes, info.data_bytes);
  ASSERT_FALSE(info.is_rf64);

  // Read channel samples via wav_read_channel_samples
  size_t count0 = 0, count1 = 0;
  double *ch0 =
      wav_read_channel_samples(filename, 0, &count0, err_msg, sizeof(err_msg));
  double *ch1 =
      wav_read_channel_samples(filename, 1, &count1, err_msg, sizeof(err_msg));

  ASSERT_TRUE(ch0 != NULL);
  ASSERT_TRUE(ch1 != NULL);
  ASSERT_EQ(num_frames, (uint32_t)count0);
  ASSERT_EQ(num_frames, (uint32_t)count1);

  for (size_t i = 0; i < count0; i++) {
    ASSERT_TRUE(fabs(ch0[i] - (16384.0 / 32768.0)) < 1e-5);
    ASSERT_TRUE(fabs(ch1[i] - (-16384.0 / 32768.0)) < 1e-5);
  }

  free(ch0);
  free(ch1);
  remove(filename);
}

TEST(WavWriterAndReader_Float32) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_f32_%d.wav", getpid());
  remove(filename);

  FILE *f = fopen(filename, "wb");
  ASSERT_TRUE(f != NULL);

  uint32_t sample_rate = 96000;
  size_t channels = 1;
  uint32_t num_frames = 50;
  uint32_t data_bytes = num_frames * 4;

  ASSERT_TRUE(wav_write_header(f, channels, BINARY_SAMPLE_FORMAT_F32_LE,
                               sample_rate, data_bytes, true));

  for (uint32_t i = 0; i < num_frames; i++) {
    float s = (float)i / 50.0f;
    ASSERT_EQ(1, fwrite(&s, sizeof(float), 1, f));
  }
  fclose(f);

  wav_info_t info;
  char err_msg[256] = {0};
  ASSERT_TRUE(
      wav_read_info_from_file(filename, &info, err_msg, sizeof(err_msg)));
  ASSERT_EQ(96000, (int)info.sample_rate);
  ASSERT_EQ(1, (int)info.channels);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_F32_LE, info.format);
  ASSERT_EQ(data_bytes, info.data_bytes);

  size_t count = 0;
  double *ch =
      wav_read_channel_samples(filename, 0, &count, err_msg, sizeof(err_msg));
  ASSERT_TRUE(ch != NULL);
  ASSERT_EQ(num_frames, (uint32_t)count);

  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(ch[i] - ((double)i / 50.0)) < 1e-5);
  }

  free(ch);
  remove(filename);
}

TEST(WavWriterAndReader_RF64) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_rf64_%d.wav", getpid());
  remove(filename);

  FILE *f = fopen(filename, "wb");
  ASSERT_TRUE(f != NULL);

  uint32_t sample_rate = 44100;
  size_t channels = 4;
  uint64_t data_bytes = 1024;

  ASSERT_TRUE(wav_write_rf64_header(f, channels, BINARY_SAMPLE_FORMAT_S32_LE,
                                    sample_rate, data_bytes));
  // Write dummy 1024 bytes
  uint8_t dummy[1024] = {0};
  fwrite(dummy, 1, sizeof(dummy), f);
  fclose(f);

  wav_info_t info;
  char err_msg[256] = {0};
  ASSERT_TRUE(
      wav_read_info_from_file(filename, &info, err_msg, sizeof(err_msg)));
  ASSERT_TRUE(info.is_rf64);
  ASSERT_EQ(44100, (int)info.sample_rate);
  ASSERT_EQ(4, (int)info.channels);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S32_LE, info.format);
  ASSERT_EQ(data_bytes, info.data_bytes);

  remove(filename);
}

TEST(WavUpdateHeader_Seekable) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_update_%d.wav", getpid());
  remove(filename);

  FILE *f = fopen(filename, "wb+");
  ASSERT_TRUE(f != NULL);

  // Write streaming placeholder
  ASSERT_TRUE(wav_write_header(f, 2, BINARY_SAMPLE_FORMAT_S16_LE, 44100,
                               0xFFFFFFFF, true));

  // Write 160 bytes of audio (40 stereo frames)
  uint8_t audio[160] = {0};
  fseek(f, 0, SEEK_END);
  fwrite(audio, 1, sizeof(audio), f);

  // Update header with actual written bytes
  ASSERT_TRUE(wav_update_header(f, 2, BINARY_SAMPLE_FORMAT_S16_LE, 44100,
                                sizeof(audio), false));
  fclose(f);

  wav_info_t info;
  char err_msg[256] = {0};
  ASSERT_TRUE(
      wav_read_info_from_file(filename, &info, err_msg, sizeof(err_msg)));
  ASSERT_EQ(160, (int)info.data_bytes);
  ASSERT_EQ(44100, (int)info.sample_rate);
  ASSERT_EQ(2, (int)info.channels);

  remove(filename);
}

TEST(WavChannelOutOfRange_ReturnsError) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_ch_oob_%d.wav", getpid());
  remove(filename);

  FILE *f = fopen(filename, "wb");
  ASSERT_TRUE(f != NULL);
  ASSERT_TRUE(
      wav_write_header(f, 2, BINARY_SAMPLE_FORMAT_S16_LE, 44100, 40, true));
  uint8_t audio[40] = {0};
  fwrite(audio, 1, sizeof(audio), f);
  fclose(f);

  size_t count = 0;
  char err_msg[256] = {0};
  // Channel 2 is out of range for 2-channel audio (valid indices: 0, 1)
  double *samples =
      wav_read_channel_samples(filename, 2, &count, err_msg, sizeof(err_msg));
  ASSERT_TRUE(samples == NULL);
  ASSERT_TRUE(strstr(err_msg, "Cant read channel 2") != NULL);

  // Negative channel
  double *neg_samples =
      wav_read_channel_samples(filename, -1, &count, err_msg, sizeof(err_msg));
  ASSERT_TRUE(neg_samples == NULL);
  ASSERT_TRUE(strstr(err_msg, "Conv channel must be non-negative") != NULL);

  remove(filename);
}

TEST(WavWriteFile_MultiChannel) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_wav_multi_%d.wav", getpid());
  remove(filename);

  size_t frames = 64;
  double ch0[64];
  double ch1[64];
  for (size_t i = 0; i < frames; i++) {
    ch0[i] = (double)i / 64.0;
    ch1[i] = -(double)i / 64.0;
  }
  const double *channels[2] = {ch0, ch1};

  char err_msg[256] = {0};
  ASSERT_TRUE(wav_write_file(filename, channels, 2, frames, 48000,
                             BINARY_SAMPLE_FORMAT_F32_LE, false, err_msg,
                             sizeof(err_msg)));

  size_t read_count0 = 0;
  size_t read_count1 = 0;
  double *r0 = wav_read_channel_samples(filename, 0, &read_count0, err_msg,
                                        sizeof(err_msg));
  double *r1 = wav_read_channel_samples(filename, 1, &read_count1, err_msg,
                                        sizeof(err_msg));
  ASSERT_TRUE(r0 != NULL);
  ASSERT_TRUE(r1 != NULL);
  ASSERT_EQ(frames, read_count0);
  ASSERT_EQ(frames, read_count1);

  for (size_t i = 0; i < frames; i++) {
    ASSERT_TRUE(fabs(r0[i] - ch0[i]) < 1e-6);
    ASSERT_TRUE(fabs(r1[i] - ch1[i]) < 1e-6);
  }

  free(r0);
  free(r1);
  remove(filename);
}

TEST(WavCanWriteBytes_BoundaryCheck) {
  // Plain WAV seekable: cannot exceed 0xFFFFFFFF - 256
  uint64_t max_bytes = 0xFFFFFFFFULL - 256;
  ASSERT_TRUE(wav_can_write_bytes(0, 1024, true, true, false));
  ASSERT_TRUE(wav_can_write_bytes(max_bytes - 100, 100, true, true, false));
  ASSERT_FALSE(wav_can_write_bytes(max_bytes - 50, 100, true, true, false));
  ASSERT_FALSE(wav_can_write_bytes(max_bytes, 1, true, true, false));

  // RF64 can exceed 4 GB
  ASSERT_TRUE(wav_can_write_bytes(max_bytes, 1024 * 1024, true, true, true));

  // Non-wav can exceed 4 GB
  ASSERT_TRUE(wav_can_write_bytes(max_bytes, 1024 * 1024, false, true, false));

  // Streaming non-seekable plain WAV does not know size in advance, so writes
  // proceed
  ASSERT_TRUE(wav_can_write_bytes(max_bytes, 1024, true, false, false));
}

TEST(RawWriteAndRead_BinaryFormats) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_raw_bin_%d.raw", getpid());
  remove(filename);

  size_t count = 32;
  double samples[32];
  for (size_t i = 0; i < count; i++) {
    samples[i] = (double)i / 32.0;
  }

  char err_msg[256] = {0};
  // S16_LE
  ASSERT_TRUE(raw_write_samples(filename, samples, count,
                                BINARY_SAMPLE_FORMAT_S16_LE, err_msg,
                                sizeof(err_msg)));
  size_t out_count = 0;
  double *read_back = raw_read_samples(filename, "S16_LE", 0, 0, &out_count,
                                       err_msg, sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(count, out_count);
  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(read_back[i] - samples[i]) < 1e-4);
  }
  free(read_back);
  remove(filename);

  // F32_LE
  ASSERT_TRUE(raw_write_samples(filename, samples, count,
                                BINARY_SAMPLE_FORMAT_F32_LE, err_msg,
                                sizeof(err_msg)));
  out_count = 0;
  read_back = raw_read_samples(filename, "F32_LE", 0, 0, &out_count, err_msg,
                               sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(count, out_count);
  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(read_back[i] - samples[i]) < 1e-6);
  }
  free(read_back);
  remove(filename);

  // F64_LE
  ASSERT_TRUE(raw_write_samples(filename, samples, count,
                                BINARY_SAMPLE_FORMAT_F64_LE, err_msg,
                                sizeof(err_msg)));
  out_count = 0;
  read_back = raw_read_samples(filename, "F64_LE", 0, 0, &out_count, err_msg,
                               sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(count, out_count);
  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(read_back[i] - samples[i]) < 1e-12);
  }
  free(read_back);
  remove(filename);

  // S32_LE
  ASSERT_TRUE(raw_write_samples(filename, samples, count,
                                BINARY_SAMPLE_FORMAT_S32_LE, err_msg,
                                sizeof(err_msg)));
  out_count = 0;
  read_back = raw_read_samples(filename, "S32_LE", 0, 0, &out_count, err_msg,
                               sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(count, out_count);
  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(read_back[i] - samples[i]) < 1e-8);
  }
  free(read_back);
  remove(filename);
}

TEST(RawWriteAndRead_TextFormat) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_raw_text_%d.txt", getpid());
  remove(filename);

  size_t count = 5;
  double values[5] = {0.125, -0.5, 0.75, -1.0, 0.0};
  char err_msg[256] = {0};

  ASSERT_TRUE(raw_write_text_samples(filename, values, count, err_msg,
                                     sizeof(err_msg)));

  size_t out_count = 0;
  double *read_back = raw_read_samples(filename, "TEXT", 0, 0, &out_count,
                                       err_msg, sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(count, out_count);
  for (size_t i = 0; i < count; i++) {
    ASSERT_TRUE(fabs(read_back[i] - values[i]) < 1e-9);
  }
  free(read_back);

  // Test skip_lines and read_lines
  out_count = 0;
  read_back = raw_read_samples(filename, "TEXT", 2, 2, &out_count, err_msg,
                               sizeof(err_msg));
  ASSERT_TRUE(read_back != NULL);
  ASSERT_EQ(2, out_count);
  ASSERT_TRUE(fabs(read_back[0] - values[2]) < 1e-9);
  ASSERT_TRUE(fabs(read_back[1] - values[3]) < 1e-9);
  free(read_back);

  remove(filename);
}

TEST(RawRead_TextValidationErrors) {
  char filename[256];
  snprintf(filename, sizeof(filename), "/tmp/test_raw_err_%d.txt", getpid());
  remove(filename);

  char err_msg[512] = {0};
  size_t out_count = 0;

  // 1. Empty line rejection
  FILE *f = fopen(filename, "w");
  fprintf(f, "0.5\n\n0.25\n");
  fclose(f);
  double *res = raw_read_samples(filename, "TEXT", 0, 0, &out_count, err_msg,
                                 sizeof(err_msg));
  ASSERT_TRUE(res == NULL);
  ASSERT_TRUE(strstr(err_msg, "empty line") != NULL);
  remove(filename);

  // 2. Hex float rejection
  f = fopen(filename, "w");
  fprintf(f, "0.5\n0x1.0p0\n");
  fclose(f);
  res = raw_read_samples(filename, "TEXT", 0, 0, &out_count, err_msg,
                         sizeof(err_msg));
  ASSERT_TRUE(res == NULL);
  ASSERT_TRUE(strstr(err_msg, "hex float not supported") != NULL);
  remove(filename);
}

// MARK: - WAV / RIFF Format Regression Tests

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++)
    p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}

static void audit_conv_tmp_path(char *buf, size_t len, const char *tag) {
  snprintf(buf, len, "/tmp/cdsp_audit_conv_%d_%s.bin", (int)getpid(), tag);
}

static bool audit_conv_write_file(const char *path, const uint8_t *data,
                                  size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok;
}

static size_t build_fmt_chunk(uint8_t *out, uint16_t tag, uint16_t channels,
                              uint32_t rate, uint16_t block_align,
                              uint16_t bits, bool extensible,
                              uint16_t valid_bits, uint16_t sub_format) {
  static const uint8_t guid_suffix[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                          0x00, 0x80, 0x00, 0x00, 0xAA,
                                          0x00, 0x38, 0x9B, 0x71};
  uint32_t size = extensible ? 40 : 16;
  memcpy(out, "fmt ", 4);
  put_u32(out + 4, size);
  uint8_t *p = out + 8;
  memset(p, 0, size);
  put_u16(p, extensible ? 0xFFFE : tag);
  put_u16(p + 2, channels);
  put_u32(p + 4, rate);
  put_u32(p + 8, rate * block_align);
  put_u16(p + 12, block_align);
  put_u16(p + 14, bits);
  if (extensible) {
    put_u16(p + 16, 22);
    put_u16(p + 18, valid_bits);
    put_u16(p + 24, sub_format);
    memcpy(p + 26, guid_suffix, 14);
  }
  return 8 + size;
}

static bool write_riff_wav(const char *path, uint16_t tag, uint16_t channels,
                           uint16_t block_align, uint16_t bits, bool extensible,
                           uint16_t valid_bits, uint16_t sub_format,
                           const uint8_t *payload, uint32_t payload_len,
                           uint32_t declared_len) {
  uint8_t buf[4096];
  size_t pos = 12;
  memcpy(buf, "RIFF", 4);
  memcpy(buf + 8, "WAVE", 4);
  pos += build_fmt_chunk(buf + pos, tag, channels, 48000, block_align, bits,
                         extensible, valid_bits, sub_format);
  memcpy(buf + pos, "data", 4);
  put_u32(buf + pos + 4, declared_len);
  pos += 8;
  if (pos + payload_len > sizeof(buf))
    return false;
  memcpy(buf + pos, payload, payload_len);
  pos += payload_len;
  put_u32(buf + 4, (uint32_t)(pos - 8));
  return audit_conv_write_file(path, buf, pos);
}

TEST(AuditConvWav24In32StandardIsLeftJustified) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f1_std");
  const uint8_t payload[] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xE0};
  ASSERT_TRUE(write_riff_wav(path, 1, 1, 4, 24, false, 0, 0, payload,
                             sizeof(payload), sizeof(payload)));
  size_t count = 0;
  char err[256] = {0};
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(2, (long long)count);
  ASSERT_NEAR(0.5, s[0], 1e-9);
  ASSERT_NEAR(-0.25, s[1], 1e-9);
  free(s);
}

TEST(AuditConvWav24In32ExtensibleIsLeftJustified) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f1_ext");
  const uint8_t payload[] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xE0};
  ASSERT_TRUE(write_riff_wav(path, 1, 1, 4, 32, true, 24, 1, payload,
                             sizeof(payload), sizeof(payload)));
  wav_info_t info;
  char err[256] = {0};
  ASSERT_TRUE(wav_read_info_from_file(path, &info, err, sizeof(err)));
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, info.format);
  size_t count = 0;
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(2, (long long)count);
  ASSERT_NEAR(0.5, s[0], 1e-9);
  ASSERT_NEAR(-0.25, s[1], 1e-9);
  free(s);
}

TEST(AuditConvWavWriterRejectsS24RightJustified) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "r08_19");
  FILE *f = fopen(path, "wb");
  ASSERT_TRUE(f != NULL);
  bool plain =
      wav_write_header(f, 2, BINARY_SAMPLE_FORMAT_S24_4_RJ_LE, 48000, 0, true);
  bool rf64 =
      wav_write_rf64_header(f, 2, BINARY_SAMPLE_FORMAT_S24_4_RJ_LE, 48000, 0);
  bool lj =
      wav_write_header(f, 2, BINARY_SAMPLE_FORMAT_S24_4_LJ_LE, 48000, 0, true);
  fclose(f);
  remove(path);
  ASSERT_FALSE(plain);
  ASSERT_FALSE(rf64);
  ASSERT_TRUE(lj);
}

TEST(AuditConvWavOddDs64ChunkSkipsPadByte) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "r08_18");
  uint8_t buf[256];
  memset(buf, 0, sizeof(buf));
  size_t pos = 0;
  memcpy(buf, "RF64", 4);
  put_u32(buf + 4, 0xFFFFFFFFU);
  memcpy(buf + 8, "WAVE", 4);
  pos = 12;
  memcpy(buf + pos, "ds64", 4);
  put_u32(buf + pos + 4, 25);
  pos += 8;
  put_u64(buf + pos, 0);
  put_u64(buf + pos + 8, 4);
  put_u64(buf + pos + 16, 2);
  buf[pos + 24] = 0xAB;
  pos += 25;
  buf[pos++] = 0;
  pos += build_fmt_chunk(buf + pos, 1, 1, 48000, 2, 16, false, 0, 0);
  memcpy(buf + pos, "data", 4);
  put_u32(buf + pos + 4, 0xFFFFFFFFU);
  pos += 8;
  put_u16(buf + pos, 0x4000);
  put_u16(buf + pos + 2, 0xC000);
  pos += 4;
  ASSERT_TRUE(audit_conv_write_file(path, buf, pos));

  wav_info_t info;
  char err[256] = {0};
  bool ok = wav_read_info_from_file(path, &info, err, sizeof(err));
  size_t count = 0;
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_TRUE(ok);
  ASSERT_EQ(BINARY_SAMPLE_FORMAT_S16_LE, info.format);
  ASSERT_EQ(4, (long long)info.data_bytes);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(2, (long long)count);
  ASSERT_NEAR(0.5, s[0], 1e-9);
  ASSERT_NEAR(-0.5, s[1], 1e-9);
  free(s);
}

TEST(AuditConvWavU8InfoRejectedButCoeffReadable) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "r08_17");
  const uint8_t payload[] = {0xC0, 0x40, 0x80};
  ASSERT_TRUE(write_riff_wav(path, 1, 1, 1, 8, false, 0, 0, payload,
                             sizeof(payload), sizeof(payload)));
  wav_info_t info;
  char err[256] = {0};
  bool info_ok = wav_read_info_from_file(path, &info, err, sizeof(err));
  bool has_msg = err[0] != '\0';
  size_t count = 0;
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_FALSE(info_ok);
  ASSERT_TRUE(has_msg);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(3, (long long)count);
  ASSERT_NEAR(0.5, s[0], 1e-12);
  ASSERT_NEAR(-0.5, s[1], 1e-12);
  ASSERT_NEAR(0.0, s[2], 1e-12);
  free(s);
}

TEST(AuditConvWavALawCoefficientsDecode) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f8_alaw");
  const uint8_t payload[] = {0xD5, 0x55, 0xAA, 0x2A};
  ASSERT_TRUE(write_riff_wav(path, 6, 1, 1, 8, false, 0, 0, payload,
                             sizeof(payload), sizeof(payload)));
  size_t count = 0;
  char err[256] = {0};
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  wav_info_t info;
  bool info_ok = wav_read_info_from_file(path, &info, err, sizeof(err));
  remove(path);
  ASSERT_FALSE(info_ok);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(4, (long long)count);
  ASSERT_NEAR(8.0 / 32768.0, s[0], 1e-15);
  ASSERT_NEAR(-8.0 / 32768.0, s[1], 1e-15);
  ASSERT_NEAR(32256.0 / 32768.0, s[2], 1e-15);
  ASSERT_NEAR(-32256.0 / 32768.0, s[3], 1e-15);
  free(s);
}

TEST(AuditConvWavMuLawExtensibleCoefficientsDecode) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f8_mulaw");
  const uint8_t payload[] = {0xFF, 0x7F, 0x80, 0x00};
  ASSERT_TRUE(write_riff_wav(path, 0, 1, 1, 8, true, 8, 7, payload,
                             sizeof(payload), sizeof(payload)));
  size_t count = 0;
  char err[256] = {0};
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(4, (long long)count);
  ASSERT_NEAR(0.0, s[0], 1e-15);
  ASSERT_NEAR(0.0, s[1], 1e-15);
  ASSERT_NEAR(32124.0 / 32768.0, s[2], 1e-15);
  ASSERT_NEAR(-32124.0 / 32768.0, s[3], 1e-15);
  free(s);
}

TEST(AuditConvWavG711Other8BitWidthsRejected) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f8_alaw16");
  const uint8_t payload[] = {0, 0, 0, 0};
  ASSERT_TRUE(write_riff_wav(path, 6, 1, 2, 16, false, 0, 0, payload,
                             sizeof(payload), sizeof(payload)));
  size_t count = 0;
  char err[256] = {0};
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  remove(path);
  ASSERT_TRUE(s == NULL);
  ASSERT_TRUE(err[0] != '\0');
}

TEST(AuditConvWavStreamingAndOverDeclaredDataReadToEof) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f8_stream");
  const uint8_t payload[] = {0x00, 0x40, 0x00, 0xC0, 0x00, 0x20, 0x00,
                             0xE0, 0x00, 0x10, 0x00, 0xF0, 0x55};
  const uint32_t declared[] = {0xFFFFFFFFU, 0x7FFFFFF0U};
  for (size_t k = 0; k < 2; k++) {
    ASSERT_TRUE(write_riff_wav(path, 1, 2, 4, 16, false, 0, 0, payload,
                               sizeof(payload), declared[k]));
    size_t count = 0;
    char err[256] = {0};
    double *s = wav_read_channel_samples(path, 1, &count, err, sizeof(err));
    remove(path);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(3, (long long)count);
    ASSERT_NEAR(-0.5, s[0], 1e-12);
    ASSERT_NEAR(-0.25, s[1], 1e-12);
    ASSERT_NEAR(-0.125, s[2], 1e-12);
    free(s);
  }
}

TEST(AuditConvTextCoeffsParseUnderCommaLocale) {
  const char *locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "de_DE"};
  const char *set = NULL;
  for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]) && !set; i++)
    set = setlocale(LC_NUMERIC, locales[i]);
  if (!set) {
    printf("  [SKIP] no comma-decimal locale available\n");
    return;
  }
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f10_text");
  const char *body = "0.5\n-1.25e-1\n";
  bool wrote = audit_conv_write_file(path, (const uint8_t *)body, strlen(body));
  size_t count = 0;
  char err[256] = {0};
  double *s = raw_read_text_samples(path, 0, 0, &count, err, sizeof(err));
  const char *comma = "0,5\n";
  bool wrote2 =
      audit_conv_write_file(path, (const uint8_t *)comma, strlen(comma));
  size_t count2 = 0;
  double *s2 = raw_read_text_samples(path, 0, 0, &count2, err, sizeof(err));
  setlocale(LC_NUMERIC, "C");
  remove(path);
  ASSERT_TRUE(wrote);
  ASSERT_TRUE(wrote2);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ(2, (long long)count);
  ASSERT_NEAR(0.5, s[0], 1e-15);
  ASSERT_NEAR(-0.125, s[1], 1e-15);
  ASSERT_TRUE(s2 == NULL);
  free(s);
}

TEST_MAIN()
