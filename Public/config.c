#include "Public/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Config/cJSON.h"
#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"
#include "Pipeline/config_loader.h"

// Static utility to read file into string
static char* read_file_to_str(const char* path) {
  FILE* fp = fopen(path, "rb");
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
                             int* out_index) {
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
    while (*ptr && *ptr != '/' && seg_len < sizeof(segment) - 1) {
      segment[seg_len++] = *ptr++;
    }
    segment[seg_len] = '\0';
    if (*ptr == '/') ptr++;

    parent = curr;
    if (cJSON_IsObject(curr)) {
      cJSON* child = curr->child;
      curr = NULL;
      last_key = NULL;
      while (child) {
        if (strcmp(child->string, segment) == 0) {
          curr = child;
          last_key = child->string;
          break;
        }
        child = child->next;
      }
      last_idx = -1;
    } else if (cJSON_IsArray(curr)) {
      char* endptr = NULL;
      int idx = (int)strtol(segment, &endptr, 10);
      if (endptr == segment || *endptr != '\0') return NULL;
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

const char* cdsp_get_config_path(const dsp_engine_t* engine) {
  if (!engine) return NULL;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_config_path ? iface->get_config_path(iface->ctx) : NULL;
}

void cdsp_set_config_path(dsp_engine_t* engine, const char* path) {
  if (!engine) return;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (iface && iface->set_config_path) {
    iface->set_config_path(iface->ctx, path);
  }
}

bool cdsp_get_active_config_json(const dsp_engine_t* engine, char** out_json) {
  if (!engine || !out_json) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_active_config_json &&
         iface->get_active_config_json(iface->ctx, out_json);
}

bool cdsp_get_active_config_yaml(const dsp_engine_t* engine, char** out_yaml) {
  // Configs are JSON internally in CDSP-C
  return cdsp_get_active_config_json(engine, out_yaml);
}

bool cdsp_get_previous_config_json(const dsp_engine_t* engine, char** out_json) {
  if (!engine || !out_json) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface((dsp_engine_t*)engine);
  return iface && iface->get_previous_config_json &&
         iface->get_previous_config_json(iface->ctx, out_json);
}

bool cdsp_get_previous_config_yaml(const dsp_engine_t* engine, char** out_yaml) {
  return cdsp_get_previous_config_json(engine, out_yaml);
}

bool cdsp_set_config_json(dsp_engine_t* engine, const char* json_str,
                          cdsp_backend_error_t* out_err) {
  if (!engine) return false;
  dsp_engine_interface_t* iface = dsp_engine_get_interface(engine);
  if (!iface || !iface->set_config_json) return false;
  audio_backend_error_t berr = {0};
  bool ok = iface->set_config_json(iface->ctx, json_str, &berr);
  if (!ok && out_err) {
    out_err->type = (cdsp_backend_error_type_t)berr.type;
    strncpy(out_err->message, berr.message, sizeof(out_err->message) - 1);
    out_err->message[sizeof(out_err->message) - 1] = '\0';
  }
  return ok;
}

bool cdsp_set_config_yaml(dsp_engine_t* engine, const char* yaml_str,
                          cdsp_backend_error_t* out_err) {
  return cdsp_set_config_json(engine, yaml_str, out_err);
}

bool cdsp_engine_set_config_file(dsp_engine_t* engine, const char* path,
                                 int samplerate_override, int channels_override,
                                 const char* format_override, int extra_samples_override,
                                 cdsp_backend_error_t* out_err) {
  if (!engine || !path) return false;
  char* json = read_file_to_str(path);
  if (!json) {
    if (out_err) snprintf(out_err->message, sizeof(out_err->message), "Could not read file %s", path);
    return false;
  }

  dsp_config_t* parsed = NULL;
  config_error_t cerr = {0};
  if (config_loader_parse(json, &parsed, &cerr) != 0 || !parsed) {
    if (out_err) snprintf(out_err->message, sizeof(out_err->message), "Parsing failed: %s", cerr.message);
    free(json);
    return false;
  }

  // Apply overrides
  if (samplerate_override > 0) {
    parsed->devices.samplerate = samplerate_override;
  }
  if (channels_override > 0) {
    capture_device_config_set_channels(&parsed->devices.capture, channels_override);
  }
  if (extra_samples_override >= 0) {
    capture_device_config_set_extra_samples(&parsed->devices.capture, extra_samples_override);
  }
  if (format_override) {
#if defined(ENABLE_ALSA)
    if (parsed->devices.capture.type == AUDIO_BACKEND_TYPE_ALSA) {
      alsa_sample_format_t fmt = alsa_sample_format_from_string(format_override);
      if (fmt != ALSA_SAMPLE_FORMAT_INVALID) {
        parsed->devices.capture.cfg.alsa.format = fmt;
        parsed->devices.capture.cfg.alsa.has_format = true;
      }
    }
#endif
#if defined(ENABLE_COREAUDIO)
    if (parsed->devices.capture.type == AUDIO_BACKEND_TYPE_CORE_AUDIO) {
      coreaudio_sample_format_t fmt = coreaudio_sample_format_from_string(format_override);
      if (fmt != COREAUDIO_SAMPLE_FORMAT_INVALID) {
        capture_device_config_set_format(&parsed->devices.capture, fmt);
      }
    }
#endif
#if defined(ENABLE_WASAPI)
    if (parsed->devices.capture.type == AUDIO_BACKEND_TYPE_WASAPI) {
      wasapi_sample_format_t fmt = wasapi_sample_format_from_string(format_override);
      if (fmt != WASAPI_SAMPLE_FORMAT_INVALID) {
        parsed->devices.capture.cfg.wasapi.format = fmt;
        parsed->devices.capture.cfg.wasapi.has_format = true;
      }
    }
#endif
#if defined(ENABLE_ASIO)
    if (parsed->devices.capture.type == AUDIO_BACKEND_TYPE_ASIO) {
      asio_sample_format_t fmt = asio_sample_format_from_string(format_override);
      if (fmt != ASIO_SAMPLE_FORMAT_INVALID) {
        parsed->devices.capture.cfg.asio.format = fmt;
        parsed->devices.capture.cfg.asio.has_format = true;
      }
    }
#endif
    if (parsed->devices.capture.type == AUDIO_BACKEND_TYPE_FILE ||
        parsed->devices.capture.type == AUDIO_BACKEND_TYPE_STDIN_OUT) {
      binary_sample_format_t fmt = binary_sample_format_from_string(format_override);
      if (fmt != BINARY_SAMPLE_FORMAT_INVALID) {
        capture_device_config_set_file_format(&parsed->devices.capture, fmt);
      }
    }
  }

  // Set structural configuration on the engine
  audio_backend_error_t berr = {0};
  bool ok = dsp_engine_set_config_struct(engine, parsed, &berr);
  if (!ok && out_err) {
    strncpy(out_err->message, berr.message, sizeof(out_err->message) - 1);
    out_err->message[sizeof(out_err->message) - 1] = '\0';
    // dsp_engine_set_config_struct frees config on failure
  } else {
    // dsp_engine_set_config_struct takes ownership and frees on success as well
  }
  
  free(json);
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

  cJSON* node = locate_pointer(root, json_ptr, NULL, NULL, NULL);
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

bool cdsp_set_config_value(dsp_engine_t* engine, const char* json_ptr, const char* val_json,
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
  cJSON* target = locate_pointer(root, json_ptr, &parent, &key, &idx);
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

  // Basic object merge patch
  cJSON* child = patch->child;
  while (child) {
    cJSON* copy = cJSON_Duplicate(child, true);
    if (cJSON_HasObjectItem(root, child->string)) {
      cJSON_ReplaceItemInObject(root, child->string, copy);
    } else {
      cJSON_AddItemToObject(root, child->string, copy);
    }
    child = child->next;
  }
  cJSON_Delete(patch);

  char* updated_json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!updated_json) return false;

  bool ok = cdsp_set_config_json(engine, updated_json, out_err);
  free(updated_json);
  return ok;
}

bool cdsp_reload_config(dsp_engine_t* engine, cdsp_backend_error_t* out_err) {
  const char* path = cdsp_get_config_path(engine);
  if (!path) {
    if (out_err) strcpy(out_err->message, "No config file path set");
    return false;
  }
  char* json = read_file_to_str(path);
  if (!json) {
    if (out_err) snprintf(out_err->message, sizeof(out_err->message), "Could not read config file %s", path);
    return false;
  }
  bool ok = cdsp_set_config_json(engine, json, out_err);
  free(json);
  return ok;
}

bool cdsp_validate_config_json(const char* json_str, char** out_result, bool* is_error) {
  if (!json_str || !out_result || !is_error) return false;
  dsp_config_t* parsed = NULL;
  config_error_t cerr = {0};
  if (config_loader_parse(json_str, &parsed, &cerr) == 0 && parsed) {
    *out_result = strdup(json_str);
    *is_error = false;
    dsp_config_free(parsed);
    return true;
  } else {
    *out_result = strdup(cerr.message);
    *is_error = true;
    return false;
  }
}

bool cdsp_validate_config_yaml(const char* yaml_str, char** out_result, bool* is_error) {
  return cdsp_validate_config_json(yaml_str, out_result, is_error);
}

bool cdsp_validate_config_file(const char* path, char** out_result, bool* is_error) {
  if (!path) return false;
  char* json = read_file_to_str(path);
  if (!json) {
    if (out_result) *out_result = strdup("Could not read file");
    if (is_error) *is_error = true;
    return false;
  }
  bool ok = cdsp_validate_config_json(json, out_result, is_error);
  free(json);
  return ok;
}
