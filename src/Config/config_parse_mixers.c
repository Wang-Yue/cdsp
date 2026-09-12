#include "Config/config_parse_mixers.h"

#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/config_parser_internal.h"
#include "Config/configuration.h"
#include "Config/filter_config_types.h"
#include "Config/mixer_config_types.h"

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
    const char* m_name = mixer_child->string ? mixer_child->string : "";
    strncpy(nm->name, m_name, sizeof(nm->name) - 1);
    nm->name[sizeof(nm->name) - 1] = '\0';

    if (!cJSON_IsObject(mixer_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Mixer definition must be an object");
      return -1;
    }

    static const char* const allowed_mixer_keys[] = {
        "channels",    "channels_in", "channels_out",   "mapping",
        "description", "labels",      "channel_labels", NULL};
    if (validate_unknown_fields(mixer_child, allowed_mixer_keys,
                                "mixer definition", err) != 0) {
      return -1;
    }

    mixer_config_t* m_conf = &nm->mixer;

    parse_json_str(mixer_child, "description", m_conf->description,
                   sizeof(m_conf->description));
    const cJSON* m_labels_node =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "labels");
    if (!m_labels_node) {
      m_labels_node =
          cJSON_GetObjectItemCaseSensitive(mixer_child, "channel_labels");
    }
    parse_labels_array(m_labels_node, &m_conf->labels, &m_conf->labels_count,
                       &m_conf->has_labels);

    cJSON* channels_obj =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "channels");
    if (cJSON_IsObject(channels_obj)) {
      static const char* const allowed_channels_keys[] = {"in", "out", NULL};
      if (validate_unknown_fields(channels_obj, allowed_channels_keys,
                                  "mixer channels", err) != 0) {
        return -1;
      }
      if (parse_json_size_t_strict(channels_obj, "in", "mixer channels",
                                   &m_conf->channels_in, NULL, err) != 0 ||
          parse_json_size_t_strict(channels_obj, "out", "mixer channels",
                                   &m_conf->channels_out, NULL, err) != 0) {
        return -1;
      }
    } else {
      if (parse_json_size_t_strict(mixer_child, "channels_in", "mixer",
                                   &m_conf->channels_in, NULL, err) != 0 ||
          parse_json_size_t_strict(mixer_child, "channels_out", "mixer",
                                   &m_conf->channels_out, NULL, err) != 0) {
        return -1;
      }
    }

    if (m_conf->channels_in == 0 || m_conf->channels_out == 0) {
      config_error_set(
          err, CONFIG_ERR_PARSE,
          "missing or invalid required field 'channels' in mixer '%s'",
          nm->name);
      return -1;
    }

    cJSON* mapping_arr =
        cJSON_GetObjectItemCaseSensitive(mixer_child, "mapping");
    if (!cJSON_IsArray(mapping_arr)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "missing required field 'mapping' in mixer '%s'",
                       nm->name);
      return -1;
    }
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
      if (!cJSON_IsObject(map_el)) {
        config_error_set(err, CONFIG_ERR_PARSE,
                         "mapping element in mixer '%s' must be an object",
                         nm->name);
        return -1;
      }
      static const char* const allowed_mapping_keys[] = {"dest", "sources",
                                                         "mute", NULL};
      if (validate_unknown_fields(map_el, allowed_mapping_keys,
                                  "mixer mapping item", err) != 0) {
        return -1;
      }
      static const char* const req_mapping[] = {"dest", NULL};
      if (require_json_fields(map_el, req_mapping, "mixer mapping item", NULL,
                              err) != 0) {
        return -1;
      }
      mixer_mapping_t* mapping = &m_conf->mapping[mp];
      if (parse_json_size_t_strict(map_el, "dest", "mixer mapping item",
                                   &mapping->dest, NULL, err) != 0) {
        return -1;
      }
      parse_json_bool(map_el, "mute", &mapping->mute);

      cJSON* sources_arr =
          cJSON_GetObjectItemCaseSensitive(map_el, "sources");
      if (sources_arr) {
        if (!cJSON_IsArray(sources_arr)) {
          config_error_set(err, CONFIG_ERR_PARSE,
                           "field 'sources' in mixer mapping must be an array");
          return -1;
        }
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
          if (!cJSON_IsObject(src_el)) {
            config_error_set(
                err, CONFIG_ERR_PARSE,
                "source element in mixer mapping must be an object");
            return -1;
          }
          static const char* const allowed_src_keys[] = {
              "channel", "gain", "inverted", "mute", "scale", NULL};
          if (validate_unknown_fields(src_el, allowed_src_keys,
                                      "mixer source item", err) != 0) {
            return -1;
          }
          static const char* const req_source[] = {"channel", NULL};
          if (require_json_fields(src_el, req_source, "mixer source item", NULL,
                                  err) != 0) {
            return -1;
          }
          mixer_source_t* src = &mapping->sources[s];
          if (parse_json_size_t_strict(src_el, "channel",
                                       "mixer source item", &src->channel,
                                       NULL, err) != 0) {
            return -1;
          }
          src->has_gain = parse_json_double(src_el, "gain", &src->gain);
          char scale_buf[64];
          if (parse_json_str(src_el, "scale", scale_buf,
                             sizeof(scale_buf))) {
            if (strcmp(scale_buf, "linear") == 0) {
              src->scale = GAIN_SCALE_LINEAR;
            } else if (strcmp(scale_buf, "dB") == 0) {
              src->scale = GAIN_SCALE_DB;
            } else {
              config_error_set(
                  err, CONFIG_ERR_PARSE,
                  "unknown variant '%s', expected one of 'linear', 'dB'",
                  scale_buf);
              return -1;
            }
          } else {
            src->scale = GAIN_SCALE_DB;
          }
          parse_json_bool(src_el, "inverted", &src->inverted);
          parse_json_bool(src_el, "mute", &src->mute);
        }
      }
    }
    m++;
  }
  return 0;
}
