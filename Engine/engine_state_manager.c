#include "engine_state_manager.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Pipeline/state_file.h"

struct engine_state_manager {
  /** Target volumes for faders in dB. */
  double fader_volumes[FADER_COUNT];
  /** Target mute states for faders. */
  bool fader_mutes[FADER_COUNT];
  /** Path to the active configuration file. */
  char active_config_path[1024];
  /** True if active config path is set. */
  bool has_active_config_path;
  /** Path to the state persistence file. */
  char state_file_path[1024];
  /** True if state file path is set. */
  bool has_state_file_path;
  /** True if there are unsaved state changes. */
  bool dirty;
  /** Monotonic counter incremented on every state modification. */
  uint64_t change_counter;
  /** Mutex for protecting state variables. */
  pthread_mutex_t mutex;
};

engine_state_manager_t* engine_state_manager_create(void) {
  engine_state_manager_t* mgr =
      (engine_state_manager_t*)calloc(1, sizeof(engine_state_manager_t));
  if (!mgr) return NULL;

  mgr->has_active_config_path = false;
  mgr->has_state_file_path = false;
  mgr->dirty = false;
  mgr->change_counter = 0;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mgr->mutex, &attr);
  pthread_mutexattr_destroy(&attr);

  return mgr;
}

void engine_state_manager_free(engine_state_manager_t* mgr) {
  if (!mgr) return;
  pthread_mutex_destroy(&mgr->mutex);
  free(mgr);
}

void engine_state_manager_set_fader_volume(engine_state_manager_t* mgr,
                                           fader_t fader, float db) {
  if (!mgr || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&mgr->mutex);
  mgr->fader_volumes[fader] = (double)db;
  mgr->dirty = true;
  mgr->change_counter++;
  pthread_mutex_unlock(&mgr->mutex);
}

float engine_state_manager_get_fader_volume(const engine_state_manager_t* mgr,
                                            fader_t fader) {
  if (!mgr || fader < 0 || fader >= FADER_COUNT) return 0.0f;
  pthread_mutex_lock((pthread_mutex_t*)&mgr->mutex);
  float vol = (float)mgr->fader_volumes[fader];
  pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
  return vol;
}

void engine_state_manager_set_fader_mute(engine_state_manager_t* mgr,
                                         fader_t fader, bool mute) {
  if (!mgr || fader < 0 || fader >= FADER_COUNT) return;
  pthread_mutex_lock(&mgr->mutex);
  mgr->fader_mutes[fader] = mute;
  mgr->dirty = true;
  mgr->change_counter++;
  pthread_mutex_unlock(&mgr->mutex);
}

bool engine_state_manager_is_fader_muted(const engine_state_manager_t* mgr,
                                         fader_t fader) {
  if (!mgr || fader < 0 || fader >= FADER_COUNT) return false;
  pthread_mutex_lock((pthread_mutex_t*)&mgr->mutex);
  bool mute = mgr->fader_mutes[fader];
  pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
  return mute;
}

void engine_state_manager_set_state_file(engine_state_manager_t* mgr,
                                         const char* path) {
  if (!mgr) return;
  pthread_mutex_lock(&mgr->mutex);
  if (path && path[0]) {
    strncpy(mgr->state_file_path, path, sizeof(mgr->state_file_path) - 1);
    mgr->state_file_path[sizeof(mgr->state_file_path) - 1] = '\0';
    mgr->has_state_file_path = true;
  } else {
    mgr->state_file_path[0] = '\0';
    mgr->has_state_file_path = false;
  }
  pthread_mutex_unlock(&mgr->mutex);
}

const char* engine_state_manager_get_state_file(
    const engine_state_manager_t* mgr) {
  if (!mgr) return NULL;
  static _Thread_local char tls_path[1024];
  pthread_mutex_lock((pthread_mutex_t*)&mgr->mutex);
  if (mgr->has_state_file_path) {
    strncpy(tls_path, mgr->state_file_path, sizeof(tls_path) - 1);
    tls_path[sizeof(tls_path) - 1] = '\0';
    pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
    return tls_path;
  }
  pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
  return NULL;
}

void engine_state_manager_set_config_path(engine_state_manager_t* mgr,
                                          const char* path) {
  if (!mgr) return;
  pthread_mutex_lock(&mgr->mutex);
  if (path && path[0]) {
    strncpy(mgr->active_config_path, path, sizeof(mgr->active_config_path) - 1);
    mgr->active_config_path[sizeof(mgr->active_config_path) - 1] = '\0';
    mgr->has_active_config_path = true;
  } else {
    mgr->active_config_path[0] = '\0';
    mgr->has_active_config_path = false;
  }
  mgr->dirty = true;
  mgr->change_counter++;
  pthread_mutex_unlock(&mgr->mutex);
}

char* engine_state_manager_get_config_path(const engine_state_manager_t* mgr) {
  if (!mgr) return NULL;
  pthread_mutex_lock((pthread_mutex_t*)&mgr->mutex);
  char* path = NULL;
  if (mgr->has_active_config_path) {
    path = strdup(mgr->active_config_path);
  }
  pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
  return path;
}

bool engine_state_manager_is_dirty(const engine_state_manager_t* mgr) {
  if (!mgr) return false;
  pthread_mutex_lock((pthread_mutex_t*)&mgr->mutex);
  bool res = mgr->dirty;
  pthread_mutex_unlock((pthread_mutex_t*)&mgr->mutex);
  return res;
}

void engine_state_manager_sync_to_processing_parameters(
    const engine_state_manager_t* mgr, processing_parameters_t* params) {
  if (!mgr || !params) return;
  for (int i = 0; i < FADER_COUNT; i++) {
    double vol = engine_state_manager_get_fader_volume(mgr, (fader_t)i);
    bool mute = engine_state_manager_is_fader_muted(mgr, (fader_t)i);
    processing_parameters_set_target_volume_for_fader(params, vol, (fader_t)i);
    processing_parameters_set_current_volume_for_fader(params, vol, (fader_t)i);
    processing_parameters_set_muted_for_fader(params, mute, (fader_t)i);
  }
}

void engine_state_manager_save_if_needed(engine_state_manager_t* mgr) {
  if (!mgr) return;

  pthread_mutex_lock(&mgr->mutex);
  bool is_dirty = mgr->dirty;
  bool has_file = mgr->has_state_file_path;
  uint64_t saved_counter = mgr->change_counter;
  pthread_mutex_unlock(&mgr->mutex);

  if (!has_file || !is_dirty) return;

  dsp_state_t* state = dsp_state_create();
  if (!state) return;

  char* cfg_path = engine_state_manager_get_config_path(mgr);
  if (cfg_path) {
    if (cfg_path[0]) {
      dsp_state_set_config_path(state, cfg_path);
    }
    free(cfg_path);
  }

  for (int i = 0; i < FADER_COUNT; i++) {
    dsp_state_set_volume(
        state, i, engine_state_manager_get_fader_volume(mgr, (fader_t)i));
    dsp_state_set_mute(state, i,
                       engine_state_manager_is_fader_muted(mgr, (fader_t)i));
  }

  const char* s_path = engine_state_manager_get_state_file(mgr);
  if (s_path && dsp_state_save(s_path, state)) {
    pthread_mutex_lock(&mgr->mutex);
    if (mgr->change_counter == saved_counter) {
      mgr->dirty = false;
    }
    pthread_mutex_unlock(&mgr->mutex);
  }
  dsp_state_free(state);
}
