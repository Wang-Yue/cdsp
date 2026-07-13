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
      cJSON* in = cJSON_GetObjectItemCaseSensitive(channels_obj, "in");
      if (cJSON_IsNumber(in)) {
        m_conf->channels_in = in->valueint;
      }
      cJSON* out = cJSON_GetObjectItemCaseSensitive(channels_obj, "out");
      if (cJSON_IsNumber(out)) {
        m_conf->channels_out = out->valueint;
      }
    } else {
      cJSON* in = cJSON_GetObjectItemCaseSensitive(mixer_child, "channels_in");
      if (cJSON_IsNumber(in)) {
        m_conf->channels_in = in->valueint;
      }
      cJSON* out =
          cJSON_GetObjectItemCaseSensitive(mixer_child, "channels_out");
      if (cJSON_IsNumber(out)) {
        m_conf->channels_out = out->valueint;
      }
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

          cJSON* dest = cJSON_GetObjectItemCaseSensitive(map_el, "dest");
          if (cJSON_IsNumber(dest)) {
            mapping->dest = dest->valueint;
          }

          cJSON* mute = cJSON_GetObjectItemCaseSensitive(map_el, "mute");
          if (cJSON_IsBool(mute)) {
            mapping->mute = cJSON_IsTrue(mute);
          }

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

                cJSON* chan =
                    cJSON_GetObjectItemCaseSensitive(src_el, "channel");
                if (cJSON_IsNumber(chan)) {
                  src->channel = chan->valueint;
                }

                cJSON* gain = cJSON_GetObjectItemCaseSensitive(src_el, "gain");
                if (cJSON_IsNumber(gain)) {
                  src->gain = gain->valuedouble;
                  src->has_gain = true;
                }

                cJSON* scale =
                    cJSON_GetObjectItemCaseSensitive(src_el, "scale");
                if (cJSON_IsString(scale) && scale->valuestring) {
                  if (strcasecmp(scale->valuestring, "Linear") == 0)
                    src->scale = GAIN_SCALE_LINEAR;
                  else
                    src->scale = GAIN_SCALE_DB;
                } else {
                  src->scale = GAIN_SCALE_DB;
                }

                cJSON* inv =
                    cJSON_GetObjectItemCaseSensitive(src_el, "inverted");
                if (cJSON_IsBool(inv)) {
                  src->inverted = cJSON_IsTrue(inv);
                }

                cJSON* smute = cJSON_GetObjectItemCaseSensitive(src_el, "mute");
                if (cJSON_IsBool(smute)) {
                  src->mute = cJSON_IsTrue(smute);
                }
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
