#include "Public/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/cdsp_yaml.h"
#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"
#include "Pipeline/config_loader.h"
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

bool cdsp_get_active_config_json(const dsp_engine_t* engine, char** out_json) {
  if (!engine || !out_json) return false;
  return engine->get_active_config_json &&
         engine->get_active_config_json(engine->ctx, out_json);
}

bool cdsp_get_active_config_yaml(const dsp_engine_t* engine, char** out_yaml) {
  if (!engine || !out_yaml) return false;
  char* json_str = NULL;
  if (!cdsp_get_active_config_json(engine, &json_str)) return false;
  cJSON* root = cJSON_Parse(json_str);
  free(json_str);
  if (!root) return false;
  *out_yaml = cdsp_json_to_yaml(root);
  cJSON_Delete(root);
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
  cJSON* root = cJSON_Parse(json_str);
  free(json_str);
  if (!root) return false;
  *out_yaml = cdsp_json_to_yaml(root);
  cJSON_Delete(root);
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
  cJSON* json_root = cdsp_yaml_to_json(yaml_str, &err_msg);
  if (!json_root) {
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
  char* json_str = cJSON_PrintUnformatted(json_root);
  cJSON_Delete(json_root);
  if (!json_str) return false;
  bool ok = cdsp_set_config_json(engine, json_str, out_err);
  free(json_str);
  return ok;
}

bool cdsp_engine_set_config_file(dsp_engine_t* engine, const char* path,
                                 int samplerate_override, int channels_override,
                                 const char* format_override,
                                 int extra_samples_override,
                                 cdsp_backend_error_t* out_err) {
  if (!engine || !path) return false;
  char* raw_content = read_file_to_str(path);
  if (!raw_content) {
    if (out_err)
      snprintf(out_err->message, sizeof(out_err->message),
               "Could not read file %s", path);
    return false;
  }

  const char* p = raw_content;
  while (isspace((unsigned char)*p)) p++;
  cJSON* root = NULL;
  if (*p == '{') {
    root = cJSON_Parse(raw_content);
  } else {
    char* err_msg = NULL;
    root = cdsp_yaml_to_json(raw_content, &err_msg);
    if (err_msg) free(err_msg);
  }
  free(raw_content);
  if (!root) {
    if (out_err)
      snprintf(out_err->message, sizeof(out_err->message),
               "Could not parse config file format");
    return false;
  }

  cJSON* devices = cJSON_GetObjectItem(root, "devices");
  if (devices) {
    if (samplerate_override > 0) {
      cJSON* item = cJSON_CreateNumber(samplerate_override);
      if (cJSON_HasObjectItem(devices, "samplerate")) {
        cJSON_ReplaceItemInObject(devices, "samplerate", item);
      } else {
        cJSON_AddItemToObject(devices, "samplerate", item);
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
    if (out_err)
      snprintf(out_err->message, sizeof(out_err->message),
               "Failed to format updated JSON");
    return false;
  }

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
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
  cJSON* target = locate_pointer(root, json_ptr, &parent, &key, &idx, new_key, sizeof(new_key));
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
      cJSON* target_item = cJSON_GetObjectItemCaseSensitive(target, child->string);
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
  char* file_str = read_file_to_str(path);
  if (!file_str) {
    if (out_err)
      snprintf(out_err->message, sizeof(out_err->message),
               "Could not read config file %s", path);
    free(path);
    return false;
  }
  free(path);

  const char* p = file_str;
  while (isspace((unsigned char)*p)) p++;
  bool ok = false;
  if (*p == '{') {
    ok = cdsp_set_config_json(engine, file_str, out_err);
  } else {
    ok = cdsp_set_config_yaml(engine, file_str, out_err);
  }
  free(file_str);
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
  cJSON* json_root = cdsp_yaml_to_json(yaml_str, &err_msg);
  if (!json_root) {
    *out_result = strdup(err_msg ? err_msg : "Invalid YAML syntax");
    *out_err_type = CDSP_CONFIG_ERR_PARSE;
    if (err_msg) free(err_msg);
    return false;
  }
  if (err_msg) free(err_msg);
  char* json_str = cJSON_PrintUnformatted(json_root);
  cJSON_Delete(json_root);
  if (!json_str) {
    *out_result = strdup("Memory allocation error during YAML validation");
    *out_err_type = CDSP_CONFIG_ERR_VALIDATION;
    return false;
  }
  char* json_res = NULL;
  bool ok = cdsp_validate_config_json(json_str, &json_res, out_err_type);
  free(json_str);
  if (ok && json_res && *out_err_type == CDSP_CONFIG_ERR_NONE) {
    cJSON* val_root = cJSON_Parse(json_res);
    if (val_root) {
      free(json_res);
      *out_result = cdsp_json_to_yaml(val_root);
      cJSON_Delete(val_root);
      return true;
    }
  }
  *out_result = json_res;
  return ok;
}

bool cdsp_validate_config_file(const char* path, char** out_result,
                               cdsp_config_error_type_t* out_err_type) {
  if (!path) return false;
  char* file_str = read_file_to_str(path);
  if (!file_str) {
    if (out_result) *out_result = strdup("Could not read file");
    if (out_err_type) *out_err_type = CDSP_CONFIG_ERR_PARSE;
    return false;
  }
  const char* p = file_str;
  while (isspace((unsigned char)*p)) p++;
  bool ok = false;
  if (*p == '{') {
    ok = cdsp_validate_config_json(file_str, out_result, out_err_type);
  } else {
    ok = cdsp_validate_config_yaml(file_str, out_result, out_err_type);
  }
  free(file_str);
  return ok;
}
