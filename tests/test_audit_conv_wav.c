// Regression tests for audit report 03 (convolution / FFT) and the
// report 08 items in src/wav that were assigned to the conv fixer.
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_support.h"
#include "wav/raw_reader.h"
#include "wav/wav_reader.h"
#include "wav/wav_writer.h"

// MARK: - Helpers

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

/**
 * @brief Builds a fmt chunk (16 or 40 bytes payload) into @p out.
 * @return Number of bytes written (8 + payload).
 */
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

/**
 * @brief Writes a plain RIFF WAV file with the given fmt chunk and payload.
 */
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

// MARK: - Report 03 F1: 24-in-32 WAV must decode left-justified

TEST(AuditConvWav24In32StandardIsLeftJustified) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f1_std");
  // Left-justified 0.5: 0x40000000 -> bytes 00 00 00 40.
  // Left-justified -0.25: 0xE0000000 -> bytes 00 00 00 E0.
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

// MARK: - Report 08 §1.9: WAV writers must reject S24_4_RJ_LE

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

// MARK: - Report 08 §1.8: odd-sized ds64 chunk pad byte

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
  // ds64 with odd size 25 (24 mandatory bytes + 1 extra), followed by pad.
  memcpy(buf + pos, "ds64", 4);
  put_u32(buf + pos + 4, 25);
  pos += 8;
  put_u64(buf + pos, 0);      // riff size (unused)
  put_u64(buf + pos + 8, 4);  // data size
  put_u64(buf + pos + 16, 2); // sample count
  buf[pos + 24] = 0xAB;       // extra odd byte
  pos += 25;
  buf[pos++] = 0; // RIFF pad byte
  pos += build_fmt_chunk(buf + pos, 1, 1, 48000, 2, 16, false, 0, 0);
  memcpy(buf + pos, "data", 4);
  put_u32(buf + pos + 4, 0xFFFFFFFFU);
  pos += 8;
  // 0.5 and -0.5 as S16
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

// MARK: - Report 08 §1.7 + report 03 F8.2: 8-bit encodings

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
  // Reference values from upstream audioadapter-sample tests.
  const uint8_t payload[] = {0xD5, 0x55, 0xAA, 0x2A};
  ASSERT_TRUE(write_riff_wav(path, 6, 1, 1, 8, false, 0, 0, payload,
                             sizeof(payload), sizeof(payload)));
  size_t count = 0;
  char err[256] = {0};
  double *s = wav_read_channel_samples(path, 0, &count, err, sizeof(err));
  wav_info_t info;
  bool info_ok = wav_read_info_from_file(path, &info, err, sizeof(err));
  remove(path);
  ASSERT_FALSE(info_ok); // not a streamable capture format
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

// MARK: - Report 03 F8.1: streaming / over-declared data chunk

TEST(AuditConvWavStreamingAndOverDeclaredDataReadToEof) {
  char path[256];
  audit_conv_tmp_path(path, sizeof(path), "f8_stream");
  // 3 S16 stereo frames plus one dangling byte of a partial frame.
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

// MARK: - Report 03 F10.1: TEXT parsing must be locale independent

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
  // Upstream rejects the comma form too.
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
