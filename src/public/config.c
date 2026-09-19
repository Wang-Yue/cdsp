#include "cdsp/config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdsp/cdsp_pub_types.h"
#include "config/cJSON.h"
#include "config/cdsp_yaml.h"
#include "config/config_error.h"
#include "config/configuration.h"
#include "config/engine_config_types.h"
#include "engine/dsp_engine.h"
#include "pipeline/config_loader.h"
#include "utils/cdsp_path.h"

typedef struct {
  int samplerate;
  int channels;
  char format[32];
  bool has_format;
  int extra_samples;
} cdsp_cli_overrides_t;

static cdsp_cli_overrides_t g_cli_overrides = {
    .samplerate = -1,
    .channels = -1,
    .format = {0},
    .has_format = false,
    .extra_samples = -1,
};

void cdsp_set_cli_overrides(int samplerate, int channels, const char *format,
                            int extra_samples) {
  g_cli_overrides.samplerate = samplerate;
  g_cli_overrides.channels = channels;
  if (format && format[0] != '\0') {
    strncpy(g_cli_overrides.format, format, sizeof(g_cli_overrides.format) - 1);
    g_cli_overrides.format[sizeof(g_cli_overrides.format) - 1] = '\0';
    g_cli_overrides.has_format = true;
  } else {
    g_cli_overrides.format[0] = '\0';
    g_cli_overrides.has_format = false;
  }
  g_cli_overrides.extra_samples = extra_samples;
}

// Static utility to read file into string
static char *read_file_to_str(const char *path) {
  FILE *fp = cdsp_fopen(path, "rb");
  if (!fp)
    return NULL;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (len < 0) {
    fclose(fp);
    return NULL;
  }
  char *buf = (char *)calloc((size_t)len + 1, sizeof(char));
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  size_t read_bytes = fread(buf, 1, (size_t)len, fp);
  buf[read_bytes] = '\0';
  fclose(fp);
  return buf;
}

// Static JSON pointer locate helper (copied from ws_rpc_dispatcher.c)
static cJSON *locate_pointer(cJSON *root, const char *pointer,
                             cJSON **out_parent, const char **out_key,
                             int *out_index, char *out_new_key,
                             size_t new_key_max_len) {
  if (out_parent)
    *out_parent = NULL;
  if (out_key)
    *out_key = NULL;
  if (out_index)
    *out_index = -1;
  if (out_new_key && new_key_max_len > 0) {
    out_new_key[0] = '\0';
  }
  if (!root || !pointer)
    return NULL;
  const char *ptr = pointer;
  if (*ptr == '/')
    ptr++;
  cJSON *curr = root;
  cJSON *parent = NULL;
  const char *last_key = NULL;
  int last_idx = -1;

  while (*ptr && curr) {
    char segment[128];
    size_t seg_len = 0;
    while (*ptr && *ptr != '/') {
      if (seg_len >= sizeof(segment) - 1)
        return NULL;
      if (*ptr == '~') {
        ptr++;
        if (*ptr == '1') {
          segment[seg_len++] = '/';
          ptr++;
        } else if (*ptr == '0') {
          segment[seg_len++] = '~';
          ptr++;
        } else {
          segment[seg_len++] = '~';
        }
      } else {
        segment[seg_len++] = *ptr++;
      }
    }
    segment[seg_len] = '\0';
    if (*ptr == '/')
      ptr++;

    parent = curr;
    last_key = NULL;
    last_idx = -1;

    if (cJSON_IsObject(curr)) {
      cJSON *child = curr->child;
      curr = NULL;
      bool found = false;
      while (child) {
        if (child->string && strcmp(child->string, segment) == 0) {
          curr = child;
          last_key = child->string;
          found = true;
          break;
        }
        child = child->next;
      }
      if (!found) {
        if (*ptr == '\0') {
          if (out_new_key && new_key_max_len > 0) {
            strncpy(out_new_key, segment, new_key_max_len - 1);
            out_new_key[new_key_max_len - 1] = '\0';
          }
          if (out_parent)
            *out_parent = parent;
          if (out_key)
            *out_key = NULL;
          if (out_index)
            *out_index = -1;
          return NULL;
        }
        return NULL;
      }
    } else if (cJSON_IsArray(curr)) {
      char *endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (idx < 0 || endptr == segment || *endptr != '\0')
        return NULL;
      curr = cJSON_GetArrayItem(curr, idx);
      if (!curr) {
        return NULL;
      }
      last_idx = idx;
      last_key = NULL;
    } else {
      return NULL;
    }
  }

  if (*ptr != '\0') {
    return NULL;
  }

  if (out_parent)
    *out_parent = parent;
  if (out_key)
    *out_key = last_key;
  if (out_index)
    *out_index = last_idx;
  return curr;
}

char *cdsp_get_config_file_path(const dsp_engine_t *engine) {
  return engine && engine->get_config_file_path
             ? engine->get_config_file_path(engine->ctx)
             : NULL;
}

void cdsp_set_config_file_path(dsp_engine_t *engine, const char *path) {
  if (engine && engine->set_config_file_path) {
    engine->set_config_file_path(engine->ctx, path);
  }
}

static char *json_str_to_yaml_str(const char *json_str) {
  if (!json_str)
    return NULL;
  cJSON *root = cJSON_Parse(json_str);
  if (!root)
    return NULL;
  char *yaml = cdsp_json_to_yaml(root);
  cJSON_Delete(root);
  return yaml;
}

static char *yaml_str_to_json_str(const char *yaml_str, char **out_err_msg) {
  if (!yaml_str)
    return NULL;
  cJSON *root = cdsp_yaml_to_json(yaml_str, out_err_msg);
  if (!root)
    return NULL;
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

bool cdsp_get_active_config_json(const dsp_engine_t *engine, char **out_json) {
  if (!engine || !out_json)
    return false;
  return engine->get_active_config_json &&
         engine->get_active_config_json(engine->ctx, out_json);
}

bool cdsp_get_active_config_yaml(const dsp_engine_t *engine, char **out_yaml) {
  if (!engine || !out_yaml)
    return false;
  char *json_str = NULL;
  if (!cdsp_get_active_config_json(engine, &json_str))
    return false;
  *out_yaml = json_str_to_yaml_str(json_str);
  free(json_str);
  return *out_yaml != NULL;
}

bool cdsp_get_previous_config_json(const dsp_engine_t *engine,
                                   char **out_json) {
  if (!engine || !out_json)
    return false;
  return engine->get_previous_config_json &&
         engine->get_previous_config_json(engine->ctx, out_json);
}

bool cdsp_get_previous_config_yaml(const dsp_engine_t *engine,
                                   char **out_yaml) {
  if (!engine || !out_yaml)
    return false;
  char *json_str = NULL;
  if (!cdsp_get_previous_config_json(engine, &json_str))
    return false;
  *out_yaml = json_str_to_yaml_str(json_str);
  free(json_str);
  return *out_yaml != NULL;
}

static cdsp_backend_error_type_t
map_backend_error_type(audio_backend_error_type_t type) {
  switch (type) {
  case AUDIO_BACKEND_ERR_CONFIG_PARSE:
    return CDSP_BACKEND_ERR_CONFIG_PARSE;
  case AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND:
    return CDSP_BACKEND_ERR_DEVICE_NOT_FOUND;
  case AUDIO_BACKEND_ERR_DEVICE_BUSY:
    return CDSP_BACKEND_ERR_DEVICE_BUSY;
  case AUDIO_BACKEND_ERR_CONFIG_READ:
    return CDSP_BACKEND_ERR_CONFIG_READ;
  default:
    return CDSP_BACKEND_ERR_UNKNOWN;
  }
}

static bool apply_cjson_overrides(cJSON *root, int samplerate_override,
                                  int channels_override,
                                  const char *format_override,
                                  int extra_samples_override, char *err_msg,
                                  size_t err_msg_len) {
  if (!root)
    return true;

  cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (!devices)
    return true;

  cJSON *capture = cJSON_GetObjectItemCaseSensitive(devices, "capture");
  const char *cap_type = "";
  if (capture) {
    cJSON *t = cJSON_GetObjectItemCaseSensitive(capture, "type");
    if (t && t->valuestring) {
      cap_type = t->valuestring;
    }
  }

  if (samplerate_override > 0 && strcmp(cap_type, "WavFile") != 0) {
    cJSON *resampler = cJSON_GetObjectItemCaseSensitive(devices, "resampler");
    cJSON *old_sr = cJSON_GetObjectItemCaseSensitive(devices, "samplerate");
    double cfg_rate = old_sr ? old_sr->valuedouble : 0.0;

    if (!resampler || cJSON_IsNull(resampler)) {
      cJSON *old_cs = cJSON_GetObjectItemCaseSensitive(devices, "chunksize");
      if (cfg_rate > 0.0 && old_cs && old_cs->valuedouble > 0.0) {
        double rate = (double)samplerate_override;
        double cfg_chunksize = old_cs->valuedouble;
        long scaled_chunksize;
        if (rate > cfg_rate) {
          scaled_chunksize = (long)(cfg_chunksize * round(rate / cfg_rate));
        } else {
          scaled_chunksize = (long)(cfg_chunksize / round(cfg_rate / rate));
        }
        if (scaled_chunksize <= 0) {
          scaled_chunksize = 1;
        }
        cJSON_ReplaceItemInObject(devices, "chunksize",
                                  cJSON_CreateNumber((double)scaled_chunksize));
      }
      cJSON *item = cJSON_CreateNumber(samplerate_override);
      if (cJSON_HasObjectItem(devices, "samplerate")) {
        cJSON_ReplaceItemInObject(devices, "samplerate", item);
      } else {
        cJSON_AddItemToObject(devices, "samplerate", item);
      }

      // Rescale extra_samples for RawFile or Stdin capture
      if (capture && (strcmp(cap_type, "RawFile") == 0 ||
                      strcmp(cap_type, "Stdin") == 0)) {
        cJSON *old_extra =
            cJSON_GetObjectItemCaseSensitive(capture, "extra_samples");
        if (old_extra && cfg_rate > 0.0) {
          long new_extra =
              (long)((old_extra->valuedouble * (double)samplerate_override) /
                     cfg_rate);
          cJSON_ReplaceItemInObject(capture, "extra_samples",
                                    cJSON_CreateNumber((double)new_extra));
        }
      }
    } else {
      cJSON *item = cJSON_CreateNumber(samplerate_override);
      if (cJSON_HasObjectItem(devices, "capture_samplerate")) {
        cJSON_ReplaceItemInObject(devices, "capture_samplerate", item);
      } else {
        cJSON_AddItemToObject(devices, "capture_samplerate", item);
      }

      cJSON *era =
          cJSON_GetObjectItemCaseSensitive(devices, "enable_rate_adjust");
      bool has_rate_adjust = era && cJSON_IsTrue(era);
      if (cfg_rate > 0.0 && samplerate_override == (int)cfg_rate &&
          !has_rate_adjust) {
        cJSON_DeleteItemFromObject(devices, "resampler");
      }
    }
  }

  if (capture) {
    if (channels_override > 0) {
      if (strcmp(cap_type, "WavFile") != 0) {
        cJSON *item = cJSON_CreateNumber(channels_override);
        if (cJSON_HasObjectItem(capture, "channels")) {
          cJSON_ReplaceItemInObject(capture, "channels", item);
        } else {
          cJSON_AddItemToObject(capture, "channels", item);
        }
      }
    }
    if (extra_samples_override >= 0) {
      if (strcmp(cap_type, "RawFile") == 0 || strcmp(cap_type, "Stdin") == 0) {
        cJSON *item = cJSON_CreateNumber(extra_samples_override);
        if (cJSON_HasObjectItem(capture, "extra_samples")) {
          cJSON_ReplaceItemInObject(capture, "extra_samples", item);
        } else {
          cJSON_AddItemToObject(capture, "extra_samples", item);
        }
      }
    }
    if (format_override && format_override[0] != '\0') {
      const char *mapped_fmt = NULL;
      bool skip_format = false;
      if (strcmp(cap_type, "WavFile") == 0 ||
          strcmp(cap_type, "SignalGenerator") == 0) {
        skip_format = true;
      } else if (strcmp(cap_type, "PipeWire") == 0) {
        skip_format = true;
      } else if (strcmp(cap_type, "CoreAudio") == 0) {
        if (strcmp(format_override, "S16_LE") == 0)
          mapped_fmt = "S16";
        else if (strcmp(format_override, "S24_3_LE") == 0 ||
                 strcmp(format_override, "S24_4_LJ_LE") == 0 ||
                 strcmp(format_override, "S24_4_RJ_LE") == 0)
          mapped_fmt = "S24";
        else if (strcmp(format_override, "S32_LE") == 0)
          mapped_fmt = "S32";
        else if (strcmp(format_override, "F32_LE") == 0)
          mapped_fmt = "F32";
        else {
          if (err_msg && err_msg_len > 0)
            snprintf(
                err_msg, err_msg_len,
                "CoreAudio does not have a sample format corresponding to %s",
                format_override);
          return false;
        }
      } else if (strcmp(cap_type, "Wasapi") == 0) {
        if (strcmp(format_override, "S16_LE") == 0)
          mapped_fmt = "S16";
        else if (strcmp(format_override, "S24_3_LE") == 0 ||
                 strcmp(format_override, "S24_4_LJ_LE") == 0 ||
                 strcmp(format_override, "S24_4_RJ_LE") == 0)
          mapped_fmt = "S24";
        else if (strcmp(format_override, "S32_LE") == 0)
          mapped_fmt = "S32";
        else if (strcmp(format_override, "F32_LE") == 0)
          mapped_fmt = "F32";
        else {
          if (err_msg && err_msg_len > 0)
            snprintf(err_msg, err_msg_len,
                     "Wasapi does not have a sample format corresponding to %s",
                     format_override);
          return false;
        }
      } else if (strcmp(cap_type, "Alsa") == 0) {
        if (strcmp(format_override, "S16_LE") == 0)
          mapped_fmt = "S16_LE";
        else if (strcmp(format_override, "S24_3_LE") == 0)
          mapped_fmt = "S24_3_LE";
        else if (strcmp(format_override, "S24_4_LJ_LE") == 0 ||
                 strcmp(format_override, "S24_4_RJ_LE") == 0)
          mapped_fmt = "S24_4_LE";
        else if (strcmp(format_override, "S32_LE") == 0)
          mapped_fmt = "S32_LE";
        else if (strcmp(format_override, "F32_LE") == 0)
          mapped_fmt = "F32_LE";
        else if (strcmp(format_override, "F64_LE") == 0)
          mapped_fmt = "F64_LE";
        else
          mapped_fmt = format_override;
      } else if (strcmp(cap_type, "Asio") == 0) {
        if (strcmp(format_override, "S16_LE") == 0)
          mapped_fmt = "S16_LE";
        else if (strcmp(format_override, "S24_3_LE") == 0)
          mapped_fmt = "S24_3_LE";
        else if (strcmp(format_override, "S24_4_LJ_LE") == 0 ||
                 strcmp(format_override, "S24_4_RJ_LE") == 0)
          mapped_fmt = "S24_4_LE";
        else if (strcmp(format_override, "S32_LE") == 0)
          mapped_fmt = "S32_LE";
        else if (strcmp(format_override, "F32_LE") == 0)
          mapped_fmt = "F32_LE";
        else if (strcmp(format_override, "F64_LE") == 0)
          mapped_fmt = "F64_LE";
        else
          mapped_fmt = format_override;
      } else {
        mapped_fmt = format_override;
      }

      if (!skip_format && mapped_fmt) {
        cJSON *item = cJSON_CreateString(mapped_fmt);
        if (cJSON_HasObjectItem(capture, "format")) {
          cJSON_ReplaceItemInObject(capture, "format", item);
        } else {
          cJSON_AddItemToObject(capture, "format", item);
        }
      }
    }
  }
  return true;
}

bool cdsp_set_config_json(dsp_engine_t *engine, const char *json_str,
                          cdsp_backend_error_t *out_err) {
  if (!engine || !engine->set_config_json)
    return false;

  const char *json_to_set = json_str;
  char *overridden_json = NULL;

  if (g_cli_overrides.samplerate > 0 || g_cli_overrides.channels > 0 ||
      g_cli_overrides.has_format || g_cli_overrides.extra_samples >= 0) {
    cJSON *root = cJSON_Parse(json_str);
    if (root) {
      char err_msg[256] = {0};
      if (apply_cjson_overrides(
              root, g_cli_overrides.samplerate, g_cli_overrides.channels,
              g_cli_overrides.has_format ? g_cli_overrides.format : NULL,
              g_cli_overrides.extra_samples, err_msg, sizeof(err_msg))) {
        overridden_json = cJSON_PrintUnformatted(root);
        if (overridden_json) {
          json_to_set = overridden_json;
        }
      } else {
        cJSON_Delete(root);
        if (out_err) {
          out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
          snprintf(out_err->message, sizeof(out_err->message), "%s", err_msg);
        }
        return false;
      }
      cJSON_Delete(root);
    }
  }

  audio_backend_error_t berr = {0};
  bool ok = engine->set_config_json(engine->ctx, json_to_set, &berr);
  if (overridden_json) {
    free(overridden_json);
  }
  if (!ok && out_err) {
    out_err->type = map_backend_error_type(berr.type);
    strncpy(out_err->message, berr.message, sizeof(out_err->message) - 1);
    out_err->message[sizeof(out_err->message) - 1] = '\0';
  }
  return ok;
}

bool cdsp_set_config_yaml(dsp_engine_t *engine, const char *yaml_str,
                          cdsp_backend_error_t *out_err) {
  if (!engine || !yaml_str)
    return false;
  char *err_msg = NULL;
  char *json_str = yaml_str_to_json_str(yaml_str, &err_msg);
  if (!json_str) {
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
      snprintf(out_err->message, sizeof(out_err->message),
               "YAML parse error: %s",
               err_msg ? err_msg : "Invalid YAML syntax");
    }
    if (err_msg)
      free(err_msg);
    return false;
  }
  if (err_msg)
    free(err_msg);
  bool ok = cdsp_set_config_json(engine, json_str, out_err);
  free(json_str);
  return ok;
}

// Helper to load a YAML/JSON configuration file and apply CLI overrides
static char *read_config_file_as_json_with_overrides(
    const char *path, int samplerate_override, int channels_override,
    const char *format_override, int extra_samples_override, bool *out_is_json,
    char *err_msg, size_t err_msg_len) {
  if (out_is_json)
    *out_is_json = false;
  if (err_msg && err_msg_len > 0)
    err_msg[0] = '\0';
  char *raw_content = read_file_to_str(path);
  if (!raw_content) {
    if (err_msg)
      snprintf(err_msg, err_msg_len, "Could not read file %s", path);
    return NULL;
  }

  const char *p = raw_content;
  while (isspace((unsigned char)*p))
    p++;
  bool is_json = (*p == '{');
  if (out_is_json)
    *out_is_json = is_json;

  cJSON *root = NULL;
  if (is_json) {
    root = cJSON_Parse(raw_content);
    if (!root && err_msg) {
      snprintf(err_msg, err_msg_len, "Invalid JSON syntax");
    }
  } else {
    char *yaml_err = NULL;
    root = cdsp_yaml_to_json(raw_content, &yaml_err);
    if (!root && err_msg) {
      snprintf(err_msg, err_msg_len, "YAML parse error: %s",
               yaml_err ? yaml_err : "Invalid YAML syntax");
    }
    if (yaml_err)
      free(yaml_err);
  }
  free(raw_content);
  if (!root) {
    if (err_msg && err_msg[0] == '\0') {
      snprintf(err_msg, err_msg_len, "Could not parse config file format");
    }
    return NULL;
  }

  // Fall back to persistent CLI overrides if caller didn't specify explicit
  // values
  if (samplerate_override <= 0 && g_cli_overrides.samplerate > 0) {
    samplerate_override = g_cli_overrides.samplerate;
  }
  if (channels_override <= 0 && g_cli_overrides.channels > 0) {
    channels_override = g_cli_overrides.channels;
  }
  if (!format_override && g_cli_overrides.has_format) {
    format_override = g_cli_overrides.format;
  }
  if (extra_samples_override < 0 && g_cli_overrides.extra_samples >= 0) {
    extra_samples_override = g_cli_overrides.extra_samples;
  }

  if (!apply_cjson_overrides(root, samplerate_override, channels_override,
                             format_override, extra_samples_override, err_msg,
                             err_msg_len)) {
    cJSON_Delete(root);
    return NULL;
  }

  char *updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!updated_json) {
    if (err_msg)
      snprintf(err_msg, err_msg_len, "Failed to format updated JSON");
    return NULL;
  }
  return updated_json;
}

bool cdsp_engine_set_config_file(dsp_engine_t *engine, const char *path,
                                 int samplerate_override, int channels_override,
                                 const char *format_override,
                                 int extra_samples_override,
                                 cdsp_backend_error_t *out_err) {
  if (!engine || !path)
    return false;
  char err_msg[256] = {0};
  char *updated_json = read_config_file_as_json_with_overrides(
      path, samplerate_override, channels_override, format_override,
      extra_samples_override, NULL, err_msg, sizeof(err_msg));
  if (!updated_json) {
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_READ;
      snprintf(out_err->message, sizeof(out_err->message), "%s",
               err_msg[0] ? err_msg : "Could not read config file");
    }
    return false;
  }

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  if (ok) {
    cdsp_set_config_file_path(engine, path);
  }
  return ok;
}

char *cdsp_get_config_title(const dsp_engine_t *engine) {
  char *json = cdsp_get_config_value(engine, "/title");
  if (!json)
    return NULL;
  cJSON *node = cJSON_Parse(json);
  free(json);
  if (!node)
    return NULL;
  char *res = (cJSON_IsString(node) && node->valuestring)
                  ? strdup(node->valuestring)
                  : NULL;
  cJSON_Delete(node);
  return res;
}

char *cdsp_get_config_description(const dsp_engine_t *engine) {
  char *json = cdsp_get_config_value(engine, "/description");
  if (!json)
    return NULL;
  cJSON *node = cJSON_Parse(json);
  free(json);
  if (!node)
    return NULL;
  char *res = (cJSON_IsString(node) && node->valuestring)
                  ? strdup(node->valuestring)
                  : NULL;
  cJSON_Delete(node);
  return res;
}

char *cdsp_get_config_value(const dsp_engine_t *engine, const char *json_ptr) {
  char *json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return NULL;
  }
  cJSON *root = cJSON_Parse(json);
  free(json);
  if (!root)
    return NULL;

  cJSON *node = locate_pointer(root, json_ptr, NULL, NULL, NULL, NULL, 0);
  if (!node) {
    cJSON_Delete(root);
    return NULL;
  }

  char *val = cJSON_PrintUnformatted(node);

  cJSON_Delete(root);
  return val;
}

bool cdsp_set_config_value(dsp_engine_t *engine, const char *json_ptr,
                           const char *val_json,
                           cdsp_backend_error_t *out_err) {
  char *json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return false;
  }
  cJSON *root = cJSON_Parse(json);
  free(json);
  if (!root)
    return false;

  cJSON *parent = NULL;
  const char *key = NULL;
  int idx = -1;
  char new_key[128] = "";
  cJSON *target = locate_pointer(root, json_ptr, &parent, &key, &idx, new_key,
                                 sizeof(new_key));
  (void)target;
  if (!parent) {
    cJSON_Delete(root);
    return false;
  }

  cJSON *new_node = cJSON_Parse(val_json);
  if (!new_node) {
    cJSON_Delete(root);
    return false;
  }

  bool ok_mod = false;
  if (key && cJSON_IsObject(parent)) {
    ok_mod = cJSON_ReplaceItemInObject(parent, key, new_node);
  } else if (new_key[0] != '\0' && cJSON_IsObject(parent)) {
    cJSON_AddItemToObject(parent, new_key, new_node);
    ok_mod = true;
  } else if (idx >= 0 && cJSON_IsArray(parent)) {
    ok_mod = cJSON_ReplaceItemInArray(parent, idx, new_node);
  }

  if (!ok_mod) {
    cJSON_Delete(new_node);
    cJSON_Delete(root);
    return false;
  }

  char *updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!updated_json)
    return false;

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  return ok;
}

static void json_merge_patch(cJSON *target, cJSON *patch) {
  if (!target || !patch)
    return;
  cJSON *child = patch->child;
  while (child) {
    if (child->string) {
      cJSON *target_item =
          cJSON_GetObjectItemCaseSensitive(target, child->string);
      if (cJSON_IsNull(child)) {
        if (target_item) {
          cJSON_DeleteItemFromObject(target, child->string);
        }
      } else if (cJSON_IsObject(child)) {
        if (target_item && cJSON_IsObject(target_item)) {
          json_merge_patch(target_item, child);
        } else {
          cJSON *copy = cJSON_Duplicate(child, true);
          if (copy) {
            if (target_item) {
              cJSON_ReplaceItemInObject(target, child->string, copy);
            } else {
              cJSON_AddItemToObject(target, child->string, copy);
            }
          }
        }
      } else {
        cJSON *copy = cJSON_Duplicate(child, true);
        if (copy) {
          if (target_item) {
            cJSON_ReplaceItemInObject(target, child->string, copy);
          } else {
            cJSON_AddItemToObject(target, child->string, copy);
          }
        }
      }
    }
    child = child->next;
  }
}

bool cdsp_patch_config(dsp_engine_t *engine, const char *patch_json,
                       cdsp_backend_error_t *out_err) {
  char *json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return false;
  }
  cJSON *root = cJSON_Parse(json);
  free(json);
  if (!root)
    return false;

  cJSON *patch = cJSON_Parse(patch_json);
  if (!patch || !cJSON_IsObject(patch)) {
    if (patch)
      cJSON_Delete(patch);
    cJSON_Delete(root);
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
      snprintf(out_err->message, sizeof(out_err->message),
               "Patch must be a JSON object");
    }
    return false;
  }

  json_merge_patch(root, patch);
  cJSON_Delete(patch);

  char *updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!updated_json)
    return false;

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  return ok;
}

bool cdsp_reload_config(dsp_engine_t *engine, cdsp_backend_error_t *out_err) {
  char *path = cdsp_get_config_file_path(engine);
  if (!path || path[0] == '\0') {
    if (path)
      free(path);
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
      snprintf(out_err->message, sizeof(out_err->message),
               "Config path not given, cannot reload");
    }
    return false;
  }
  bool ok = cdsp_engine_set_config_file(engine, path, 0, 0, NULL, -1, out_err);
  free(path);
  return ok;
}

static void config_fill_defaults(cJSON *root) {
  if (!root || !cJSON_IsObject(root))
    return;

  if (!cJSON_HasObjectItem(root, "title")) {
    cJSON_AddNullToObject(root, "title");
  }
  if (!cJSON_HasObjectItem(root, "description")) {
    cJSON_AddNullToObject(root, "description");
  }

  cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (devices && cJSON_IsObject(devices)) {
    static const char *const dev_null_fields[] = {"queuelimit",
                                                  "silence_threshold",
                                                  "silence_timeout_s",
                                                  "enable_rate_adjust",
                                                  "target_level",
                                                  "adjust_interval_s",
                                                  "resampler",
                                                  "capture_samplerate",
                                                  "stop_on_rate_change",
                                                  "rate_measure_interval_s",
                                                  "volume_ramp_time_ms",
                                                  "volume_limit",
                                                  "multithreaded",
                                                  "worker_threads",
                                                  NULL};
    for (int i = 0; dev_null_fields[i] != NULL; i++) {
      if (!cJSON_HasObjectItem(devices, dev_null_fields[i])) {
        cJSON_AddNullToObject(devices, dev_null_fields[i]);
      }
    }

    cJSON *capture = cJSON_GetObjectItemCaseSensitive(devices, "capture");
    if (capture && cJSON_IsObject(capture)) {
      if (!cJSON_HasObjectItem(capture, "extra_samples")) {
        cJSON_AddNullToObject(capture, "extra_samples");
      }
      cJSON *type_item = cJSON_GetObjectItemCaseSensitive(capture, "type");
      if (type_item && cJSON_IsString(type_item) &&
          strcmp(type_item->valuestring, "RawFile") == 0) {
        if (!cJSON_HasObjectItem(capture, "skip_bytes")) {
          cJSON_AddNullToObject(capture, "skip_bytes");
        }
        if (!cJSON_HasObjectItem(capture, "read_bytes")) {
          cJSON_AddNullToObject(capture, "read_bytes");
        }
      }
      if (!cJSON_HasObjectItem(capture, "labels")) {
        cJSON_AddNullToObject(capture, "labels");
      }
    }

    cJSON *playback = cJSON_GetObjectItemCaseSensitive(devices, "playback");
    if (playback && cJSON_IsObject(playback)) {
      cJSON *type_item = cJSON_GetObjectItemCaseSensitive(playback, "type");
      if (type_item && cJSON_IsString(type_item) &&
          strcmp(type_item->valuestring, "File") == 0) {
        if (!cJSON_HasObjectItem(playback, "wav_header")) {
          cJSON_AddNullToObject(playback, "wav_header");
        }
        if (!cJSON_HasObjectItem(playback, "use_rf64")) {
          cJSON_AddNullToObject(playback, "use_rf64");
        }
      }
    }
  }

  cJSON *mixers = cJSON_GetObjectItemCaseSensitive(root, "mixers");
  if (!mixers) {
    cJSON_AddNullToObject(root, "mixers");
  } else if (cJSON_IsObject(mixers)) {
    cJSON *mixer = mixers->child;
    while (mixer) {
      if (cJSON_IsObject(mixer)) {
        if (!cJSON_HasObjectItem(mixer, "description")) {
          cJSON_AddNullToObject(mixer, "description");
        }
        if (!cJSON_HasObjectItem(mixer, "labels")) {
          cJSON_AddNullToObject(mixer, "labels");
        }
        cJSON *mapping = cJSON_GetObjectItemCaseSensitive(mixer, "mapping");
        if (mapping && cJSON_IsArray(mapping)) {
          cJSON *map_entry = mapping->child;
          while (map_entry) {
            if (cJSON_IsObject(map_entry)) {
              if (!cJSON_HasObjectItem(map_entry, "mute")) {
                cJSON_AddNullToObject(map_entry, "mute");
              }
              cJSON *sources =
                  cJSON_GetObjectItemCaseSensitive(map_entry, "sources");
              if (sources && cJSON_IsArray(sources)) {
                cJSON *src = sources->child;
                while (src) {
                  if (cJSON_IsObject(src)) {
                    if (!cJSON_HasObjectItem(src, "gain")) {
                      cJSON_AddNullToObject(src, "gain");
                    }
                    if (!cJSON_HasObjectItem(src, "inverted")) {
                      cJSON_AddNullToObject(src, "inverted");
                    }
                    if (!cJSON_HasObjectItem(src, "mute")) {
                      cJSON_AddNullToObject(src, "mute");
                    }
                    if (!cJSON_HasObjectItem(src, "scale")) {
                      cJSON_AddNullToObject(src, "scale");
                    }
                  }
                  src = src->next;
                }
              }
            }
            map_entry = map_entry->next;
          }
        }
      }
      mixer = mixer->next;
    }
  }

  cJSON *filters = cJSON_GetObjectItemCaseSensitive(root, "filters");
  if (!filters) {
    cJSON_AddNullToObject(root, "filters");
  } else if (cJSON_IsObject(filters)) {
    cJSON *filter = filters->child;
    while (filter) {
      if (cJSON_IsObject(filter)) {
        if (!cJSON_HasObjectItem(filter, "description")) {
          cJSON_AddNullToObject(filter, "description");
        }
        cJSON *type = cJSON_GetObjectItemCaseSensitive(filter, "type");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(filter, "parameters");
        if (type && cJSON_IsString(type) && params && cJSON_IsObject(params)) {
          if (strcmp(type->valuestring, "Volume") == 0) {
            if (!cJSON_HasObjectItem(params, "ramp_time_ms")) {
              cJSON_AddNullToObject(params, "ramp_time_ms");
            }
            if (!cJSON_HasObjectItem(params, "limit")) {
              cJSON_AddNullToObject(params, "limit");
            }
          } else if (strcmp(type->valuestring, "Loudness") == 0) {
            if (!cJSON_HasObjectItem(params, "ramp_time_ms")) {
              cJSON_AddNullToObject(params, "ramp_time_ms");
            }
            if (!cJSON_HasObjectItem(params, "high_boost")) {
              cJSON_AddNullToObject(params, "high_boost");
            }
            if (!cJSON_HasObjectItem(params, "low_boost")) {
              cJSON_AddNullToObject(params, "low_boost");
            }
            if (!cJSON_HasObjectItem(params, "attenuation")) {
              cJSON_AddNullToObject(params, "attenuation");
            }
          } else if (strcmp(type->valuestring, "Conv") == 0) {
            cJSON *conv_type = cJSON_GetObjectItemCaseSensitive(params, "type");
            if (conv_type && cJSON_IsString(conv_type)) {
              if (strcmp(conv_type->valuestring, "Raw") == 0) {
                if (!cJSON_HasObjectItem(params, "skip_bytes_lines")) {
                  cJSON_AddNullToObject(params, "skip_bytes_lines");
                }
                if (!cJSON_HasObjectItem(params, "read_bytes_lines")) {
                  cJSON_AddNullToObject(params, "read_bytes_lines");
                }
              } else if (strcmp(conv_type->valuestring, "Wav") == 0) {
                if (!cJSON_HasObjectItem(params, "channel")) {
                  cJSON_AddNullToObject(params, "channel");
                }
              }
            }
          } else if (strcmp(type->valuestring, "Delay") == 0) {
            if (!cJSON_HasObjectItem(params, "unit")) {
              cJSON_AddNullToObject(params, "unit");
            }
            if (!cJSON_HasObjectItem(params, "subsample")) {
              cJSON_AddNullToObject(params, "subsample");
            }
          } else if (strcmp(type->valuestring, "Dither") == 0) {
            if (!cJSON_HasObjectItem(params, "bits")) {
              cJSON_AddNullToObject(params, "bits");
            }
          }
        }
      }
      filter = filter->next;
    }
  }

  cJSON *processors = cJSON_GetObjectItemCaseSensitive(root, "processors");
  if (!processors) {
    cJSON_AddNullToObject(root, "processors");
  } else if (cJSON_IsObject(processors)) {
    cJSON *proc = processors->child;
    while (proc) {
      if (cJSON_IsObject(proc)) {
        if (!cJSON_HasObjectItem(proc, "description")) {
          cJSON_AddNullToObject(proc, "description");
        }
        cJSON *type = cJSON_GetObjectItemCaseSensitive(proc, "type");
        cJSON *params = cJSON_GetObjectItemCaseSensitive(proc, "parameters");
        if (type && cJSON_IsString(type) && params && cJSON_IsObject(params)) {
          if (strcmp(type->valuestring, "Compressor") == 0) {
            if (!cJSON_HasObjectItem(params, "monitor_channels")) {
              cJSON_AddNullToObject(params, "monitor_channels");
            }
            if (!cJSON_HasObjectItem(params, "process_channels")) {
              cJSON_AddNullToObject(params, "process_channels");
            }
          }
        }
      }
      proc = proc->next;
    }
  }

  if (!cJSON_HasObjectItem(root, "pipeline")) {
    cJSON_AddNullToObject(root, "pipeline");
  }
}

bool cdsp_read_config_json(const char *json_str, char **out_result,
                           cdsp_config_error_type_t *out_err_type) {
  if (!json_str || !out_result || !out_err_type)
    return false;
  cJSON *root = cJSON_Parse(json_str);
  if (!root) {
    *out_result = strdup("Failed to parse JSON (syntax error or invalid JSON)");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }
  dsp_config_t *parsed = NULL;
  config_error_t cerr = {0};
  if (dsp_config_parse_json_no_validate(json_str, &parsed, &cerr) != 0 ||
      !parsed) {
    cJSON_Delete(root);
    *out_result =
        strdup(cerr.message[0] ? cerr.message : "Failed to parse JSON");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }
  dsp_config_free(parsed);

  config_fill_defaults(root);
  *out_result = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  *out_err_type = CDSP_CONFIG_ERR_NONE;
  return true;
}

bool cdsp_validate_config_json(const char *json_str, char **out_result,
                               cdsp_config_error_type_t *out_err_type) {
  if (!json_str || !out_result || !out_err_type)
    return false;

  const char *json_to_use = json_str;
  char *overridden_json = NULL;

  if (g_cli_overrides.samplerate > 0 || g_cli_overrides.channels > 0 ||
      g_cli_overrides.has_format || g_cli_overrides.extra_samples >= 0) {
    cJSON *temp_root = cJSON_Parse(json_str);
    if (temp_root) {
      char err_msg[256] = {0};
      if (apply_cjson_overrides(
              temp_root, g_cli_overrides.samplerate, g_cli_overrides.channels,
              g_cli_overrides.has_format ? g_cli_overrides.format : NULL,
              g_cli_overrides.extra_samples, err_msg, sizeof(err_msg))) {
        overridden_json = cJSON_PrintUnformatted(temp_root);
        if (overridden_json)
          json_to_use = overridden_json;
      }
      cJSON_Delete(temp_root);
    }
  }

  cJSON *root = cJSON_Parse(json_to_use);
  if (!root) {
    if (overridden_json)
      free(overridden_json);
    *out_result = strdup("Failed to parse JSON (syntax error or invalid JSON)");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }
  dsp_config_t *parsed = NULL;
  config_error_t cerr = {0};
  if (dsp_config_parse_json_no_validate(json_to_use, &parsed, &cerr) != 0 ||
      !parsed) {
    if (overridden_json)
      free(overridden_json);
    cJSON_Delete(root);
    *out_result =
        strdup(cerr.message[0] ? cerr.message : "Failed to parse JSON");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }

  if (dsp_config_validate(parsed, &cerr) != 0) {
    if (overridden_json)
      free(overridden_json);
    dsp_config_free(parsed);
    cJSON_Delete(root);
    *out_result =
        strdup(cerr.message[0] ? cerr.message : "Config validation failed");
    *out_err_type = CDSP_CONFIG_ERR_VALIDATION;
    return false;
  }
  dsp_config_free(parsed);

  config_fill_defaults(root);
  *out_result = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (overridden_json)
    free(overridden_json);
  *out_err_type = CDSP_CONFIG_ERR_NONE;
  return true;
}

bool cdsp_read_config_yaml(const char *yaml_str, char **out_result,
                           cdsp_config_error_type_t *out_err_type) {
  if (!yaml_str || !out_result || !out_err_type)
    return false;
  char *err_msg = NULL;
  char *json_str = yaml_str_to_json_str(yaml_str, &err_msg);
  if (!json_str) {
    *out_result = strdup(err_msg ? err_msg : "Invalid YAML syntax");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    if (err_msg)
      free(err_msg);
    return false;
  }
  if (err_msg)
    free(err_msg);

  char *json_res = NULL;
  bool ok = cdsp_read_config_json(json_str, &json_res, out_err_type);
  free(json_str);
  if (ok && json_res && *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char *yaml_res = json_str_to_yaml_str(json_res);
    if (yaml_res) {
      free(json_res);
      *out_result = yaml_res;
      return true;
    }
  }
  *out_result = json_res;
  return ok;
}

bool cdsp_validate_config_yaml(const char *yaml_str, char **out_result,
                               cdsp_config_error_type_t *out_err_type) {
  if (!yaml_str || !out_result || !out_err_type)
    return false;
  char *err_msg = NULL;
  char *json_str = yaml_str_to_json_str(yaml_str, &err_msg);
  if (!json_str) {
    *out_result = strdup(err_msg ? err_msg : "Invalid YAML syntax");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    if (err_msg)
      free(err_msg);
    return false;
  }
  if (err_msg)
    free(err_msg);

  char *json_res = NULL;
  bool ok = cdsp_validate_config_json(json_str, &json_res, out_err_type);
  free(json_str);
  if (ok && json_res && *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char *yaml_res = json_str_to_yaml_str(json_res);
    if (yaml_res) {
      free(json_res);
      *out_result = yaml_res;
      return true;
    }
  }
  *out_result = json_res;
  return ok;
}

bool cdsp_read_config_file(const char *path, char **out_result,
                           cdsp_config_error_type_t *out_err_type) {
  if (!path)
    return false;
  char err_msg[256] = {0};
  bool is_json = false;
  char *updated_json = read_config_file_as_json_with_overrides(
      path, 0, 0, NULL, -1, &is_json, err_msg, sizeof(err_msg));
  if (!updated_json) {
    if (out_result)
      *out_result = strdup(err_msg[0] ? err_msg : "Could not read file");
    if (out_err_type)
      *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }

  bool ok = cdsp_read_config_json(updated_json, out_result, out_err_type);
  free(updated_json);

  if (ok && !is_json && out_result && *out_result &&
      *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char *yaml_res = json_str_to_yaml_str(*out_result);
    if (yaml_res) {
      free(*out_result);
      *out_result = yaml_res;
    }
  }

  return ok;
}

bool cdsp_validate_config_file(const char *path, char **out_result,
                               cdsp_config_error_type_t *out_err_type) {
  return cdsp_validate_config_file_with_overrides(path, 0, 0, NULL, -1,
                                                  out_result, out_err_type);
}

bool cdsp_validate_config_file_with_overrides(
    const char *path, int samplerate_override, int channels_override,
    const char *format_override, int extra_samples_override, char **out_result,
    cdsp_config_error_type_t *out_err_type) {
  if (!path)
    return false;
  char err_msg[256] = {0};
  bool is_json = false;
  char *updated_json = read_config_file_as_json_with_overrides(
      path, samplerate_override, channels_override, format_override,
      extra_samples_override, &is_json, err_msg, sizeof(err_msg));
  if (!updated_json) {
    if (out_result)
      *out_result = strdup(err_msg[0] ? err_msg : "Could not read file");
    if (out_err_type)
      *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }

  bool ok = cdsp_validate_config_json(updated_json, out_result, out_err_type);
  free(updated_json);

  if (ok && !is_json && out_result && *out_result &&
      *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char *yaml_res = json_str_to_yaml_str(*out_result);
    if (yaml_res) {
      free(*out_result);
      *out_result = yaml_res;
    }
  }

  return ok;
}
