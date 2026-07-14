#include "config_parse_mixers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Logging/app_logger.h"
#include "cJSON.h"
#include "config_parser_internal.h"
#include "configuration.h"

int config_parse_mixers(const cJSON* mixers_obj, dsp_config_t* config,
                        config_error_t* err) {
  if (!cJSON_IsObject(mixers_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "mixers must be an object");
    return -1;
  }
  int size = 0;
  cJSON* mixer_child = NULL;
  cJSON_ArrayForEach(mixer_child, mixers_obj) { size++; }
  if (size == 0) return 0;

  config->mixers =
      (named_mixer_config_t*)calloc(size, sizeof(named_mixer_config_t));
  if (!config->mixers) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->mixers_count = size;

  int m = 0;
  cJSON_ArrayForEach(mixer_child, mixers_obj) {
    named_mixer_config_t* nm = &config->mixers[m];
    strncpy(nm->name, mixer_child->string, sizeof(nm->name) - 1);

    if (!cJSON_IsObject(mixer_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Mixer definition must be an object");
      return -1;
    }

    mixer_config_t* m_conf = &nm->mixer;

    cJSON* channels_obj =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "channels");
    if (cJSON_IsObject(channels_obj)) {
      parse_json_size_t(channels_obj, "in", &m_conf->channels_in);
      parse_json_size_t(channels_obj, "out", &m_conf->channels_out);
    } else {
      parse_json_size_t(mixer_child, "channels_in", &m_conf->channels_in);
      parse_json_size_t(mixer_child, "channels_out", &m_conf->channels_out);
    }

    cJSON* mapping_arr =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "mapping");
    if (cJSON_IsArray(mapping_arr)) {
      int map_size = cJSON_GetArraySize(mapping_arr);
      m_conf->mapping =
          (mixer_mapping_t*)calloc(map_size, sizeof(mixer_mapping_t));
      if (!m_conf->mapping) {
        config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
        return -1;
      }
      m_conf->mapping_count = map_size;

      for (int mp = 0; mp < map_size; mp++) {
        cJSON* map_el = cJSON_GetArrayItem(mapping_arr, mp);
        if (cJSON_IsObject(map_el)) {
          mixer_mapping_t* mapping = &m_conf->mapping[mp];
          parse_json_size_t(map_el, "dest", &mapping->dest);
          parse_json_bool(map_el, "mute", &mapping->mute);

          cJSON* sources_arr =
              cJSON_GetObjectItemCaseSensitive(map_el, "sources");
          if (cJSON_IsArray(sources_arr)) {
            int src_size = cJSON_GetArraySize(sources_arr);
            mapping->sources =
                (mixer_source_t*)calloc(src_size, sizeof(mixer_source_t));
            if (!mapping->sources) {
              config_error_set(err, CONFIG_ERR_PARSE,
                               "Memory allocation failure");
              return -1;
            }
            mapping->sources_count = src_size;

            for (int s = 0; s < src_size; s++) {
              cJSON* src_el = cJSON_GetArrayItem(sources_arr, s);
              if (cJSON_IsObject(src_el)) {
                mixer_source_t* src = &mapping->sources[s];
                parse_json_size_t(src_el, "channel", &src->channel);
                src->has_gain = parse_json_double(src_el, "gain", &src->gain);
                char scale_buf[64];
                if (parse_json_str(src_el, "scale", scale_buf, sizeof(scale_buf))) {
                  src->scale = (strcasecmp(scale_buf, "Linear") == 0) ? GAIN_SCALE_LINEAR : GAIN_SCALE_DB;
                } else {
                  src->scale = GAIN_SCALE_DB;
                }
                parse_json_bool(src_el, "inverted", &src->inverted);
                parse_json_bool(src_el, "mute", &src->mute);
              }
            }
          }
        }
      }
    }
    m++;
  }
  return 0;
}
