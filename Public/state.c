#include "Public/state.h"
#include "Pipeline/state_file.h"

cdsp_state_t* cdsp_state_create(void) {
  return (cdsp_state_t*)dsp_state_create();
}

void cdsp_state_free(cdsp_state_t* state) {
  dsp_state_free((dsp_state_t*)state);
}

bool cdsp_state_load(const char* filename, cdsp_state_t* out_state) {
  return dsp_state_load(filename, (dsp_state_t*)out_state);
}

bool cdsp_state_save(const char* filename, const cdsp_state_t* state) {
  return dsp_state_save(filename, (const dsp_state_t*)state);
}

const char* cdsp_state_get_config_path(const cdsp_state_t* state) {
  return dsp_state_get_config_path((const dsp_state_t*)state);
}

void cdsp_state_set_config_path(cdsp_state_t* state, const char* path) {
  dsp_state_set_config_path((dsp_state_t*)state, path);
}

bool cdsp_state_has_config_path(const cdsp_state_t* state) {
  return dsp_state_has_config_path((const dsp_state_t*)state);
}

void cdsp_state_set_has_config_path(cdsp_state_t* state, bool has_path) {
  dsp_state_set_has_config_path((dsp_state_t*)state, has_path);
}

bool cdsp_state_get_mute(const cdsp_state_t* state, int index) {
  return dsp_state_get_mute((const dsp_state_t*)state, index);
}

void cdsp_state_set_mute(cdsp_state_t* state, int index, bool mute) {
  dsp_state_set_mute((dsp_state_t*)state, index, mute);
}

double cdsp_state_get_volume(const cdsp_state_t* state, int index) {
  return dsp_state_get_volume((const dsp_state_t*)state, index);
}

void cdsp_state_set_volume(cdsp_state_t* state, int index, double volume) {
  dsp_state_set_volume((dsp_state_t*)state, index, volume);
}
