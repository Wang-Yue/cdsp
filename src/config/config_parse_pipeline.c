#include "config/config_parse_pipeline.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "config/cJSON.h"
#include "config/config_parser_internal.h"
#include "config/configuration.h"

int config_parse_pipeline(const cJSON *pipe_arr, dsp_config_t *config,
                          config_error_t *err) {
  if (!cJSON_IsArray(pipe_arr)) {
    config_error_set(err, CONFIG_ERR_PARSE, "pipeline must be an array");
    return -1;
  }
  int size = cJSON_GetArraySize(pipe_arr);
  if (size == 0)
    return 0;

  config->pipeline =
      (pipeline_step_config_t *)calloc(size, sizeof(pipeline_step_config_t));
  if (!config->pipeline) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->pipeline_count = size;

  for (int s = 0; s < size; s++) {
    cJSON *step_obj = cJSON_GetArrayItem(pipe_arr, s);
    if (!cJSON_IsObject(step_obj)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Pipeline step must be an object");
      return -1;
    }

    static const char *const allowed_step_keys[] = {
        "type",     "name",     "names",       "channel",
        "channels", "bypassed", "description", NULL};
    if (validate_unknown_fields(step_obj, allowed_step_keys, "pipeline step",
                                err) != 0) {
      return -1;
    }

    pipeline_step_config_t *step = &config->pipeline[s];

    char type_str[64];
    if (parse_json_str(step_obj, "type", type_str, sizeof(type_str))) {
      if (strcmp(type_str, "Filter") == 0)
        step->type = PIPELINE_STEP_TYPE_FILTER;
      else if (strcmp(type_str, "Mixer") == 0)
        step->type = PIPELINE_STEP_TYPE_MIXER;
      else if (strcmp(type_str, "Processor") == 0)
        step->type = PIPELINE_STEP_TYPE_PROCESSOR;
      else {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "Pipeline step %d: invalid type '%s'", s, type_str);
        return -1;
      }
    } else {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Pipeline step %d: missing 'type'", s);
      return -1;
    }

    if (step->type == PIPELINE_STEP_TYPE_FILTER) {
      static const char *const allowed_filter_step_keys[] = {
          "type", "names", "channels", "bypassed", "description", NULL};
      if (validate_unknown_fields(step_obj, allowed_filter_step_keys,
                                  "Filter pipeline step", err) != 0) {
        return -1;
      }
      if (!cJSON_HasObjectItem(step_obj, "names")) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "missing field 'names' in Filter pipeline step");
        return -1;
      }
    } else if (step->type == PIPELINE_STEP_TYPE_MIXER ||
               step->type == PIPELINE_STEP_TYPE_PROCESSOR) {
      const char *sname = (step->type == PIPELINE_STEP_TYPE_MIXER)
                              ? "Mixer pipeline step"
                              : "Processor pipeline step";
      static const char *const allowed_named_step_keys[] = {
          "type", "name", "bypassed", "description", NULL};
      if (validate_unknown_fields(step_obj, allowed_named_step_keys, sname,
                                  err) != 0) {
        return -1;
      }
      static const char *const req_named[] = {"name", NULL};
      if (require_json_fields(step_obj, req_named, sname, NULL, err) != 0) {
        return -1;
      }
    }

    if (parse_json_str_strict(step_obj, "description", "pipeline step",
                              step->description, sizeof(step->description),
                              NULL, err) != 0) {
      return -1;
    }

    if (parse_json_str_strict(step_obj, "name", "pipeline step", step->name,
                              sizeof(step->name), &step->has_name, err) != 0) {
      return -1;
    }
    if (parse_json_size_t_strict(step_obj, "channel", "pipeline step",
                                 &step->channel, &step->has_channel,
                                 err) != 0) {
      return -1;
    }
    bool bypassed_dummy = false;
    if (parse_json_bool_strict(step_obj, "bypassed", "pipeline step",
                               &step->bypassed, &bypassed_dummy, err) != 0) {
      return -1;
    }

    cJSON *names_arr = cJSON_GetObjectItemCaseSensitive(step_obj, "names");
    if (names_arr) {
      if (parse_labels_array_strict(names_arr, &step->names, &step->names_count,
                                    &step->has_names) != 0) {
        if (err) {
          config_error_set(err, CONFIG_ERR_INVALID_PIPELINE,
                           "Invalid 'names' array in pipeline step: elements "
                           "must be strings");
        }
        return -1;
      }
    }

    cJSON *channels_arr =
        cJSON_GetObjectItemCaseSensitive(step_obj, "channels");
    if (channels_arr) {
      step->has_channels = true;
      if (parse_size_t_array_strict(channels_arr, "channels", "pipeline step",
                                    &step->channels, &step->channels_count,
                                    err) != 0) {
        return -1;
      }
    }
  }
  return 0;
}
