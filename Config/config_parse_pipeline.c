#include "config_parse_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"
#include "cJSON.h"
#include "config_parser_internal.h"
#include "configuration.h"

int config_parse_pipeline(const cJSON* pipe_arr, dsp_config_t* config,
                          config_error_t* err) {
  if (!cJSON_IsArray(pipe_arr)) {
    config_error_set(err, CONFIG_ERR_PARSE, "pipeline must be an array");
    return -1;
  }
  int size = cJSON_GetArraySize(pipe_arr);
  if (size == 0) return 0;

  config->pipeline = (pipeline_step_t*)calloc(size, sizeof(pipeline_step_t));
  if (!config->pipeline) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->pipeline_count = size;

  for (int s = 0; s < size; s++) {
    cJSON* step_obj = cJSON_GetArrayItem(pipe_arr, s);
    if (!cJSON_IsObject(step_obj)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Pipeline step must be an object");
      return -1;
    }
    pipeline_step_t* step = &config->pipeline[s];

    cJSON* item;

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "type");
    if (cJSON_IsString(item) && item->valuestring) {
      if (strcmp(item->valuestring, "Filter") == 0)
        step->type = PIPELINE_STEP_TYPE_FILTER;
      else if (strcmp(item->valuestring, "Mixer") == 0)
        step->type = PIPELINE_STEP_TYPE_MIXER;
      else if (strcmp(item->valuestring, "Processor") == 0)
        step->type = PIPELINE_STEP_TYPE_PROCESSOR;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "name");
    if (cJSON_IsString(item) && item->valuestring) {
      strncpy(step->name, item->valuestring, sizeof(step->name) - 1);
      step->has_name = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "channel");
    if (cJSON_IsNumber(item)) {
      step->channel = item->valueint;
      step->has_channel = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(step_obj, "bypassed");
    if (cJSON_IsBool(item)) {
      step->bypassed = cJSON_IsTrue(item);
    }

    cJSON* names_arr = cJSON_GetObjectItemCaseSensitive(step_obj, "names");
    bool dummy;
    parse_labels_array(names_arr, &step->names, &step->names_count, &dummy);

    cJSON* channels_arr =
        cJSON_GetObjectItemCaseSensitive(step_obj, "channels");
    step->channels = parse_int_array(channels_arr, &step->channels_count);
  }
  return 0;
}
