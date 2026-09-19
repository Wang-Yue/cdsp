#include "config/config_diff.h"

#include <stdlib.h>
#include <string.h>

#include "config_gen.h"

static bool safe_streq(const char *s1, const char *s2) {
  if (s1 == s2)
    return true;
  if (!s1 || !s2)
    return false;
  return strcmp(s1, s2) == 0;
}

config_change_type_t config_diff(const dsp_config_t *current,
                                 const dsp_config_t *new_conf) {
  if (!current || !new_conf) {
    return CONFIG_CHANGE_DEVICES;
  }

  // 1. Devices config diff
  if (!devices_config_equal(&current->devices, &new_conf->devices)) {
    return CONFIG_CHANGE_DEVICES;
  }

  // 2. Pipeline steps diff
  if (current->pipeline_count != new_conf->pipeline_count) {
    return CONFIG_CHANGE_PIPELINE;
  }
  for (size_t i = 0; i < current->pipeline_count; i++) {
    if (!pipeline_step_config_equal(&current->pipeline[i],
                                    &new_conf->pipeline[i])) {
      return CONFIG_CHANGE_PIPELINE;
    }
  }

  // 3. Mixers diff
  if (current->mixers_count != new_conf->mixers_count) {
    return CONFIG_CHANGE_MIXER_PARAMETERS;
  }
  for (size_t i = 0; i < new_conf->mixers_count; i++) {
    mixer_config_t *old_m =
        dsp_config_get_mixer(current, new_conf->mixers[i].name);
    if (!old_m || !mixer_config_equal(old_m, &new_conf->mixers[i].mixer)) {
      return CONFIG_CHANGE_MIXER_PARAMETERS;
    }
  }
  for (size_t i = 0; i < current->mixers_count; i++) {
    if (!dsp_config_get_mixer(new_conf, current->mixers[i].name)) {
      return CONFIG_CHANGE_MIXER_PARAMETERS;
    }
  }

  // 4. Filters & Processors diff
  bool params_changed = false;

  for (size_t i = 0; i < new_conf->filters_count; i++) {
    const named_filter_config_t *new_nf = &new_conf->filters[i];
    const named_filter_config_t *old_nf = NULL;
    for (size_t j = 0; j < current->filters_count; j++) {
      if (strcmp(current->filters[j].name, new_nf->name) == 0) {
        old_nf = &current->filters[j];
        break;
      }
    }
    if (!old_nf) {
      continue;
    }
    if (old_nf->filter.type != new_nf->filter.type) {
      return CONFIG_CHANGE_PIPELINE;
    }
    if (!filter_config_equal(&old_nf->filter, &new_nf->filter) ||
        !safe_streq(old_nf->description, new_nf->description)) {
      if (old_nf->filter.type == FILTER_TYPE_LOOKAHEAD_LIMITER) {
        return CONFIG_CHANGE_PIPELINE;
      }
      params_changed = true;
    }
  }

  for (size_t i = 0; i < new_conf->processors_count; i++) {
    const named_processor_config_t *new_np = &new_conf->processors[i];
    const named_processor_config_t *old_np = NULL;
    for (size_t j = 0; j < current->processors_count; j++) {
      if (strcmp(current->processors[j].name, new_np->name) == 0) {
        old_np = &current->processors[j];
        break;
      }
    }
    if (!old_np) {
      continue;
    }
    if (old_np->processor.type != new_np->processor.type) {
      return CONFIG_CHANGE_PIPELINE;
    }
    if (!processor_config_equal(&old_np->processor, &new_np->processor) ||
        !safe_streq(old_np->description, new_np->description)) {
      params_changed = true;
    }
  }

  if (params_changed || !safe_streq(current->title, new_conf->title) ||
      !safe_streq(current->description, new_conf->description)) {
    return CONFIG_CHANGE_FILTER_PARAMETERS;
  }

  return CONFIG_CHANGE_NONE;
}
