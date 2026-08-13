#include "Public/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/cdsp_yaml.h"
#include "Config/config_error.h"
#include "Config/configuration.h"
#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"
#include "Pipeline/config_loader.h"
#include "Public/cdsp_pub_types.h"
#include "Utils/cdsp_path.h"

// Static utility to read file into string
static char* read_file_to_str(const char* path) {
  FILE* fp = cdsp_fopen(path, "rb");
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  long len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (len < 0) {
    fclose(fp);
    return NULL;
  }
  char* buf = (char*)calloc((size_t)len + 1, sizeof(char));
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
static cJSON* locate_pointer(cJSON* root, const char* pointer,
                             cJSON** out_parent, const char** out_key,
                             int* out_index, char* out_new_key,
                             size_t new_key_max_len) {
  if (out_new_key && new_key_max_len > 0) {
    out_new_key[0] = '\0';
  }
  if (!root || !pointer) return NULL;
  const char* ptr = pointer;
  if (*ptr == '/') ptr++;
  cJSON* curr = root;
  cJSON* parent = NULL;
  const char* last_key = NULL;
  int last_idx = -1;

  while (*ptr && curr) {
    char segment[128];
    size_t seg_len = 0;
    while (*ptr && *ptr != '/') {
      if (seg_len >= sizeof(segment) - 1) return NULL;
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
    if (*ptr == '/') ptr++;

    parent = curr;
    if (cJSON_IsObject(curr)) {
      cJSON* child = curr->child;
      curr = NULL;
      last_key = NULL;
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
        }
        break;
      }
      last_idx = -1;
    } else if (cJSON_IsArray(curr)) {
      char* endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (idx < 0 || endptr == segment || *endptr != '\0') return NULL;
      curr = cJSON_GetArrayItem(curr, idx);
      last_idx = idx;
      last_key = NULL;
    } else {
      return NULL;
    }
  }

  if (out_parent) *out_parent = parent;
  if (out_key) *out_key = last_key;
  if (out_index) *out_index = last_idx;
  return curr;
}

char* cdsp_get_config_file_path(const dsp_engine_t* engine) {
  return engine && engine->get_config_file_path
             ? engine->get_config_file_path(engine->ctx)
             : NULL;
}

void cdsp_set_config_file_path(dsp_engine_t* engine, const char* path) {
  if (engine && engine->set_config_file_path) {
    engine->set_config_file_path(engine->ctx, path);
  }
}

static char* json_str_to_yaml_str(const char* json_str) {
  if (!json_str) return NULL;
  cJSON* root = cJSON_Parse(json_str);
  if (!root) return NULL;
  char* yaml = cdsp_json_to_yaml(root);
  cJSON_Delete(root);
  return yaml;
}

static char* yaml_str_to_json_str(const char* yaml_str, char** out_err_msg) {
  if (!yaml_str) return NULL;
  cJSON* root = cdsp_yaml_to_json(yaml_str, out_err_msg);
  if (!root) return NULL;
  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

bool cdsp_get_active_config_json(const dsp_engine_t* engine, char** out_json) {
  if (!engine || !out_json) return false;
  return engine->get_active_config_json &&
         engine->get_active_config_json(engine->ctx, out_json);
}

bool cdsp_get_active_config_yaml(const dsp_engine_t* engine, char** out_yaml) {
  if (!engine || !out_yaml) return false;
  char* json_str = NULL;
  if (!cdsp_get_active_config_json(engine, &json_str)) return false;
  *out_yaml = json_str_to_yaml_str(json_str);
  free(json_str);
  return *out_yaml != NULL;
}

bool cdsp_get_previous_config_json(const dsp_engine_t* engine,
                                   char** out_json) {
  if (!engine || !out_json) return false;
  return engine->get_previous_config_json &&
         engine->get_previous_config_json(engine->ctx, out_json);
}

bool cdsp_get_previous_config_yaml(const dsp_engine_t* engine,
                                   char** out_yaml) {
  if (!engine || !out_yaml) return false;
  char* json_str = NULL;
  if (!cdsp_get_previous_config_json(engine, &json_str)) return false;
  *out_yaml = json_str_to_yaml_str(json_str);
  free(json_str);
  return *out_yaml != NULL;
}

static cdsp_backend_error_type_t map_backend_error_type(
    audio_backend_error_type_t type) {
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

bool cdsp_set_config_json(dsp_engine_t* engine, const char* json_str,
                          cdsp_backend_error_t* out_err) {
  if (!engine || !engine->set_config_json) return false;
  audio_backend_error_t berr = {0};
  bool ok = engine->set_config_json(engine->ctx, json_str, &berr);
  if (!ok && out_err) {
    out_err->type = map_backend_error_type(berr.type);
    strncpy(out_err->message, berr.message, sizeof(out_err->message) - 1);
    out_err->message[sizeof(out_err->message) - 1] = '\0';
  }
  return ok;
}

bool cdsp_set_config_yaml(dsp_engine_t* engine, const char* yaml_str,
                          cdsp_backend_error_t* out_err) {
  if (!engine || !yaml_str) return false;
  char* err_msg = NULL;
  char* json_str = yaml_str_to_json_str(yaml_str, &err_msg);
  if (!json_str) {
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
      snprintf(out_err->message, sizeof(out_err->message),
               "YAML parse error: %s",
               err_msg ? err_msg : "Invalid YAML syntax");
    }
    if (err_msg) free(err_msg);
    return false;
  }
  if (err_msg) free(err_msg);
  bool ok = cdsp_set_config_json(engine, json_str, out_err);
  free(json_str);
  return ok;
}

// Helper to load a YAML/JSON configuration file and apply CLI overrides
static char* read_config_file_as_json_with_overrides(
    const char* path, int samplerate_override, int channels_override,
    const char* format_override, int extra_samples_override, bool* out_is_json,
    char* err_msg, size_t err_msg_len) {
  if (out_is_json) *out_is_json = false;
  if (err_msg && err_msg_len > 0) err_msg[0] = '\0';
  char* raw_content = read_file_to_str(path);
  if (!raw_content) {
    if (err_msg) snprintf(err_msg, err_msg_len, "Could not read file %s", path);
    return NULL;
  }

  const char* p = raw_content;
  while (isspace((unsigned char)*p)) p++;
  bool is_json = (*p == '{');
  if (out_is_json) *out_is_json = is_json;

  cJSON* root = NULL;
  if (is_json) {
    root = cJSON_Parse(raw_content);
    if (!root && err_msg) {
      snprintf(err_msg, err_msg_len, "Invalid JSON syntax");
    }
  } else {
    char* yaml_err = NULL;
    root = cdsp_yaml_to_json(raw_content, &yaml_err);
    if (!root && err_msg) {
      snprintf(err_msg, err_msg_len, "YAML parse error: %s",
               yaml_err ? yaml_err : "Invalid YAML syntax");
    }
    if (yaml_err) free(yaml_err);
  }
  free(raw_content);
  if (!root) {
    if (err_msg && err_msg[0] == '\0') {
      snprintf(err_msg, err_msg_len, "Could not parse config file format");
    }
    return NULL;
  }

  cJSON* devices = cJSON_GetObjectItem(root, "devices");
  if (devices) {
    if (samplerate_override > 0) {
      cJSON* resampler = cJSON_GetObjectItem(devices, "resampler");
      if (!resampler || cJSON_IsNull(resampler)) {
        cJSON* old_sr = cJSON_GetObjectItem(devices, "samplerate");
        cJSON* old_cs = cJSON_GetObjectItem(devices, "chunksize");
        if (old_sr && old_cs && old_sr->valuedouble > 0) {
          double scaled_cs =
              old_cs->valuedouble *
              ((double)samplerate_override / old_sr->valuedouble);
          cJSON_ReplaceItemInObject(devices, "chunksize",
                                    cJSON_CreateNumber((int)scaled_cs));
        }
        cJSON* item = cJSON_CreateNumber(samplerate_override);
        if (cJSON_HasObjectItem(devices, "samplerate")) {
          cJSON_ReplaceItemInObject(devices, "samplerate", item);
        } else {
          cJSON_AddItemToObject(devices, "samplerate", item);
        }
      } else {
        cJSON* item = cJSON_CreateNumber(samplerate_override);
        if (cJSON_HasObjectItem(devices, "capture_samplerate")) {
          cJSON_ReplaceItemInObject(devices, "capture_samplerate", item);
        } else {
          cJSON_AddItemToObject(devices, "capture_samplerate", item);
        }
      }
    }
    cJSON* capture = cJSON_GetObjectItem(devices, "capture");
    if (capture) {
      if (channels_override > 0) {
        cJSON* item = cJSON_CreateNumber(channels_override);
        if (cJSON_HasObjectItem(capture, "channels")) {
          cJSON_ReplaceItemInObject(capture, "channels", item);
        } else {
          cJSON_AddItemToObject(capture, "channels", item);
        }
      }
      if (extra_samples_override >= 0) {
        cJSON* item = cJSON_CreateNumber(extra_samples_override);
        if (cJSON_HasObjectItem(capture, "extra_samples")) {
          cJSON_ReplaceItemInObject(capture, "extra_samples", item);
        } else {
          cJSON_AddItemToObject(capture, "extra_samples", item);
        }
      }
      if (format_override) {
        cJSON* item = cJSON_CreateString(format_override);
        if (cJSON_HasObjectItem(capture, "format")) {
          cJSON_ReplaceItemInObject(capture, "format", item);
        } else {
          cJSON_AddItemToObject(capture, "format", item);
        }
      }
    }
  }

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!updated_json) {
    if (err_msg)
      snprintf(err_msg, err_msg_len, "Failed to format updated JSON");
    return NULL;
  }
  return updated_json;
}

bool cdsp_engine_set_config_file(dsp_engine_t* engine, const char* path,
                                 int samplerate_override, int channels_override,
                                 const char* format_override,
                                 int extra_samples_override,
                                 cdsp_backend_error_t* out_err) {
  if (!engine || !path) return false;
  char err_msg[256] = {0};
  char* updated_json = read_config_file_as_json_with_overrides(
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

char* cdsp_get_config_title(const dsp_engine_t* engine) {
  return cdsp_get_config_value(engine, "/title");
}

char* cdsp_get_config_description(const dsp_engine_t* engine) {
  return cdsp_get_config_value(engine, "/description");
}

char* cdsp_get_config_value(const dsp_engine_t* engine, const char* json_ptr) {
  char* json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return NULL;
  }
  cJSON* root = cJSON_Parse(json);
  free(json);
  if (!root) return NULL;

  cJSON* node = locate_pointer(root, json_ptr, NULL, NULL, NULL, NULL, 0);
  if (!node) {
    cJSON_Delete(root);
    return NULL;
  }

  char* val = NULL;
  if (cJSON_IsString(node) && node->valuestring) {
    val = strdup(node->valuestring);
  } else {
    val = cJSON_PrintUnformatted(node);
  }

  cJSON_Delete(root);
  return val;
}

bool cdsp_set_config_value(dsp_engine_t* engine, const char* json_ptr,
                           const char* val_json,
                           cdsp_backend_error_t* out_err) {
  char* json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return false;
  }
  cJSON* root = cJSON_Parse(json);
  free(json);
  if (!root) return false;

  cJSON* parent = NULL;
  const char* key = NULL;
  int idx = -1;
  char new_key[128] = "";
  cJSON* target = locate_pointer(root, json_ptr, &parent, &key, &idx, new_key,
                                 sizeof(new_key));
  (void)target;
  if (!parent) {
    cJSON_Delete(root);
    return false;
  }

  cJSON* new_node = cJSON_Parse(val_json);
  if (!new_node) {
    cJSON_Delete(root);
    return false;
  }

  if (key) {
    cJSON_ReplaceItemInObject(parent, key, new_node);
  } else if (new_key[0] != '\0') {
    cJSON_AddItemToObject(parent, new_key, new_node);
  } else if (idx >= 0) {
    cJSON_ReplaceItemInArray(parent, idx, new_node);
  } else {
    cJSON_Delete(new_node);
    cJSON_Delete(root);
    return false;
  }

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (!updated_json) return false;

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  return ok;
}

static void json_merge_patch(cJSON* target, cJSON* patch) {
  if (!target || !patch) return;
  cJSON* child = patch->child;
  while (child) {
    if (child->string) {
      cJSON* target_item =
          cJSON_GetObjectItemCaseSensitive(target, child->string);
      if (cJSON_IsNull(child)) {
        if (target_item) {
          cJSON_DeleteItemFromObject(target, child->string);
        }
      } else if (cJSON_IsObject(child)) {
        if (target_item && cJSON_IsObject(target_item)) {
          json_merge_patch(target_item, child);
        } else {
          cJSON* copy = cJSON_Duplicate(child, true);
          if (copy) {
            if (target_item) {
              cJSON_ReplaceItemInObject(target, child->string, copy);
            } else {
              cJSON_AddItemToObject(target, child->string, copy);
            }
          }
        }
      } else {
        cJSON* copy = cJSON_Duplicate(child, true);
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

bool cdsp_patch_config(dsp_engine_t* engine, const char* patch_json,
                       cdsp_backend_error_t* out_err) {
  char* json = NULL;
  if (!cdsp_get_active_config_json(engine, &json) || !json) {
    return false;
  }
  cJSON* root = cJSON_Parse(json);
  free(json);
  if (!root) return false;

  cJSON* patch = cJSON_Parse(patch_json);
  if (!patch) {
    cJSON_Delete(root);
    return false;
  }

  json_merge_patch(root, patch);
  cJSON_Delete(patch);

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!updated_json) return false;

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  return ok;
}

bool cdsp_reload_config(dsp_engine_t* engine, cdsp_backend_error_t* out_err) {
  char* path = cdsp_get_config_file_path(engine);
  if (!path) {
    if (out_err) {
      out_err->type = CDSP_BACKEND_ERR_CONFIG_PARSE;
      snprintf(out_err->message, sizeof(out_err->message),
               "No config file path set");
    }
    return false;
  }
  bool ok = cdsp_engine_set_config_file(engine, path, 0, 0, NULL, -1, out_err);
  free(path);
  return ok;
}

bool cdsp_validate_config_json(const char* json_str, char** out_result,
                               cdsp_config_error_type_t* out_err_type) {
  if (!json_str || !out_result || !out_err_type) return false;
  dsp_config_t* parsed = NULL;
  config_error_t cerr = {0};
  if (config_loader_parse(json_str, &parsed, &cerr) == 0 && parsed) {
    *out_result = strdup(json_str);
    *out_err_type = CDSP_CONFIG_ERR_NONE;
    dsp_config_free(parsed);
    return true;
  } else {
    *out_result = strdup(cerr.message);
    if (cerr.type == CONFIG_ERR_PARSE) {
      *out_err_type = CDSP_CONFIG_ERR_PARSE;
    } else {
      *out_err_type = CDSP_CONFIG_ERR_VALIDATION;
    }
    return false;
  }
}

bool cdsp_validate_config_yaml(const char* yaml_str, char** out_result,
                               cdsp_config_error_type_t* out_err_type) {
  if (!yaml_str || !out_result || !out_err_type) return false;
  char* err_msg = NULL;
  char* json_str = yaml_str_to_json_str(yaml_str, &err_msg);
  if (!json_str) {
    *out_result = strdup(err_msg ? err_msg : "Invalid YAML syntax");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    if (err_msg) free(err_msg);
    return false;
  }
  if (err_msg) free(err_msg);

  char* json_res = NULL;
  bool ok = cdsp_validate_config_json(json_str, &json_res, out_err_type);
  free(json_str);
  if (ok && json_res && *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char* yaml_res = json_str_to_yaml_str(json_res);
    if (yaml_res) {
      free(json_res);
      *out_result = yaml_res;
      return true;
    }
  }
  *out_result = json_res;
  return ok;
}

bool cdsp_validate_config_file(const char* path, char** out_result,
                               cdsp_config_error_type_t* out_err_type) {
  return cdsp_validate_config_file_with_overrides(path, 0, 0, NULL, -1,
                                                  out_result, out_err_type);
}

bool cdsp_validate_config_file_with_overrides(
    const char* path, int samplerate_override, int channels_override,
    const char* format_override, int extra_samples_override, char** out_result,
    cdsp_config_error_type_t* out_err_type) {
  if (!path) return false;
  char err_msg[256] = {0};
  bool is_json = false;
  char* updated_json = read_config_file_as_json_with_overrides(
      path, samplerate_override, channels_override, format_override,
      extra_samples_override, &is_json, err_msg, sizeof(err_msg));
  if (!updated_json) {
    if (out_result)
      *out_result = strdup(err_msg[0] ? err_msg : "Could not read file");
    if (out_err_type) *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }

  bool ok = cdsp_validate_config_json(updated_json, out_result, out_err_type);
  free(updated_json);

  if (ok && !is_json && out_result && *out_result &&
      *out_err_type == CDSP_CONFIG_ERR_NONE) {
    char* yaml_res = json_str_to_yaml_str(*out_result);
    if (yaml_res) {
      free(*out_result);
      *out_result = yaml_res;
    }
  }

  return ok;
}
