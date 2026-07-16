#include "Public/devices.h"
#include <stdlib.h>
#include <string.h>
#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"

bool cdsp_get_available_devices(const char* backend, bool is_input,
                                cdsp_device_info_t** out_devices, size_t* out_count) {
  if (!backend || !out_devices || !out_count) return false;
  
  audio_device_t devs[128];
  int count = dsp_engine_get_available_devices(backend, is_input, devs, 128);
  if (count < 0) {
    *out_count = 0;
    *out_devices = NULL;
    return false;
  }

  *out_count = (size_t)count;
  if (count > 0) {
    cdsp_device_info_t* list = (cdsp_device_info_t*)malloc(count * sizeof(cdsp_device_info_t));
    if (!list) {
      *out_count = 0;
      *out_devices = NULL;
      return false;
    }
    for (int i = 0; i < count; i++) {
      strncpy(list[i].identifier, devs[i].name, sizeof(list[i].identifier) - 1);
      list[i].identifier[sizeof(list[i].identifier) - 1] = '\0';
      strncpy(list[i].name, devs[i].name, sizeof(list[i].name) - 1);
      list[i].name[sizeof(list[i].name) - 1] = '\0';
      list[i].has_name = true;
    }
    *out_devices = list;
  } else {
    *out_devices = NULL;
  }

  return true;
}

bool cdsp_get_device_capabilities(const char* backend, const char* device, bool is_capture,
                                  cdsp_device_descriptor_t** out_desc, cdsp_device_error_t* out_err) {
  if (!backend || !device || !out_desc) return false;

  device_error_t err = {0};
  audio_device_descriptor_t* desc = dsp_engine_get_device_capabilities(backend, device, is_capture, &err);
  if (!desc) {
    if (out_err) {
      out_err->type = (cdsp_device_error_type_t)err.type;
      strncpy(out_err->message, err.message, sizeof(out_err->message) - 1);
      out_err->message[sizeof(out_err->message) - 1] = '\0';
    }
    return false;
  }

  cdsp_device_descriptor_t* pub = (cdsp_device_descriptor_t*)malloc(sizeof(cdsp_device_descriptor_t));
  if (!pub) {
    dsp_engine_free_device_capabilities(desc);
    return false;
  }

  strncpy(pub->name, desc->name, sizeof(pub->name) - 1);
  pub->name[sizeof(pub->name) - 1] = '\0';
  pub->description[0] = '\0'; // Not used internally in C port

  pub->capability_sets_count = desc->capability_sets_count;
  if (desc->capability_sets_count > 0 && desc->capability_sets) {
    pub->capability_sets = (cdsp_device_capability_set_t*)malloc(desc->capability_sets_count * sizeof(cdsp_device_capability_set_t));
    if (!pub->capability_sets) {
      free(pub);
      dsp_engine_free_device_capabilities(desc);
      return false;
    }

    for (size_t i = 0; i < desc->capability_sets_count; i++) {
      cdsp_device_capability_set_t* pub_set = &pub->capability_sets[i];
      const device_capability_set_t* int_set = &desc->capability_sets[i];

      // Populate default mode string (internal set does not track it in C port)
      strcpy(pub_set->mode, "Unified");

      pub_set->capabilities_count = int_set->capabilities_count;
      if (int_set->capabilities_count > 0 && int_set->capabilities) {
        pub_set->capabilities = (cdsp_channel_capability_t*)malloc(int_set->capabilities_count * sizeof(cdsp_channel_capability_t));
        if (!pub_set->capabilities) {
          pub_set->capabilities_count = 0;
          continue;
        }

        for (size_t j = 0; j < int_set->capabilities_count; j++) {
          cdsp_channel_capability_t* pub_cap = &pub_set->capabilities[j];
          const channel_capability_t* int_cap = &int_set->capabilities[j];

          pub_cap->channels = int_cap->channels;
          pub_cap->samplerates_count = int_cap->samplerates_count;
          
          if (int_cap->samplerates_count > 0 && int_cap->samplerates) {
            pub_cap->samplerates = (cdsp_samplerate_capability_t*)malloc(int_cap->samplerates_count * sizeof(cdsp_samplerate_capability_t));
            if (!pub_cap->samplerates) {
              pub_cap->samplerates_count = 0;
              continue;
            }

            for (size_t k = 0; k < int_cap->samplerates_count; k++) {
              cdsp_samplerate_capability_t* pub_sr = &pub_cap->samplerates[k];
              const samplerate_capability_t* int_sr = &int_cap->samplerates[k];

              pub_sr->samplerate = int_sr->samplerate;
              pub_sr->formats_count = int_sr->formats_count;

              if (int_sr->formats_count > 0 && int_sr->formats) {
                pub_sr->formats = (char**)malloc(int_sr->formats_count * sizeof(char*));
                if (!pub_sr->formats) {
                  pub_sr->formats_count = 0;
                  continue;
                }

                for (size_t l = 0; l < int_sr->formats_count; l++) {
                  pub_sr->formats[l] = int_sr->formats[l] ? strdup(int_sr->formats[l]) : NULL;
                }
              } else {
                pub_sr->formats = NULL;
              }
            }
          } else {
            pub_cap->samplerates = NULL;
          }
        }
      } else {
        pub_set->capabilities = NULL;
      }
    }
  } else {
    pub->capability_sets = NULL;
  }

  *out_desc = pub;
  dsp_engine_free_device_capabilities(desc);
  return true;
}

void cdsp_free_device_capabilities(cdsp_device_descriptor_t* desc) {
  if (!desc) return;
  if (desc->capability_sets_count > 0 && desc->capability_sets) {
    for (size_t i = 0; i < desc->capability_sets_count; i++) {
      cdsp_device_capability_set_t* set = &desc->capability_sets[i];
      if (set->capabilities_count > 0 && set->capabilities) {
        for (size_t j = 0; j < set->capabilities_count; j++) {
          cdsp_channel_capability_t* cap = &set->capabilities[j];
          if (cap->samplerates_count > 0 && cap->samplerates) {
            for (size_t k = 0; k < cap->samplerates_count; k++) {
              cdsp_samplerate_capability_t* sr = &cap->samplerates[k];
              if (sr->formats_count > 0 && sr->formats) {
                for (size_t l = 0; l < sr->formats_count; l++) {
                  if (sr->formats[l]) free(sr->formats[l]);
                }
                free(sr->formats);
              }
            }
            free(cap->samplerates);
          }
        }
        free(set->capabilities);
      }
    }
    free(desc->capability_sets);
  }
  free(desc);
}
