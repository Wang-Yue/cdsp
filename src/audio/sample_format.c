#include "audio/sample_format.h"

#include <strings.h>

const char *dsd_mode_to_string(dsd_mode_t mode) {
  switch (mode) {
  case DSD_MODE_DOP:
    return "dop";
  case DSD_MODE_NATIVE:
    return "dsd";
  case DSD_MODE_PCM:
    return "pcm";
  }
  CDSP_UNREACHABLE();
  return "pcm";
}

dsd_mode_t dsd_mode_from_string(const char *str) {
  if (!str)
    return DSD_MODE_PCM;
  if (strcasecmp(str, "dop") == 0)
    return DSD_MODE_DOP;
  if (strcasecmp(str, "dsd") == 0 || strcasecmp(str, "native") == 0)
    return DSD_MODE_NATIVE;
  return DSD_MODE_PCM;
}

const char *file_sample_format_to_string(binary_sample_format_t fmt) {
  return binary_sample_format_to_string(fmt);
}

binary_sample_format_t file_sample_format_from_string(const char *str) {
  return binary_sample_format_from_string(str);
}

size_t sample_format_bytes_per_sample(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_DSD_U8:
    return 1;
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
    return 2;
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
    return 3;
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return 4;
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return 8;
  case BINARY_SAMPLE_FORMAT_INVALID:
    return 0;
  }
  CDSP_UNREACHABLE();
  return 0;
}

bool sample_format_is_dsd(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return true;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return false;
  }
  CDSP_UNREACHABLE();
  return false;
}

bool sample_format_is_float(binary_sample_format_t fmt) {
  switch (fmt) {
  case BINARY_SAMPLE_FORMAT_F32_LE:
  case BINARY_SAMPLE_FORMAT_F64_LE:
    return true;
  case BINARY_SAMPLE_FORMAT_INVALID:
  case BINARY_SAMPLE_FORMAT_S16_LE:
  case BINARY_SAMPLE_FORMAT_S24_3_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_RJ_LE:
  case BINARY_SAMPLE_FORMAT_S24_4_LJ_LE:
  case BINARY_SAMPLE_FORMAT_S32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U8:
  case BINARY_SAMPLE_FORMAT_DSD_U16_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U16_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_LE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_BE:
  case BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED:
    return false;
  }
  CDSP_UNREACHABLE();
  return false;
}
