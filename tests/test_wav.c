#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/sample_conversion.h"
#include "config/engine_config_types.h"
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
