#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET(s) closesocket(s)
#define IS_INVALID_SOCKET(s) ((s) == INVALID_SOCKET)
typedef SOCKET socket_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET(s) close(s)
#define IS_INVALID_SOCKET(s) ((s) < 0)
typedef int socket_t;
#endif
#include <string.h>

#include "Utils/cdsp_time.h"
#define sleep_ms(ms) cdsp_sleep_ms(ms)

#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Backend/backend_error.h"
#include "Config/cJSON.h"
#include "Engine/dsp_engine.h"
#include "Public/cdsp_pub_types.h"
#include "Public/config.h"
#include "Public/devices.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/spectrum.h"
#include "Public/volume.h"
#include "Server/websocket_server.h"

static void test_handle_command(websocket_server_t* server, int client_idx,
                                const char* command_text, char* out_response,
                                size_t max_len) {
  dyn_string_t ds;
  dyn_string_init(&ds, max_len);
  websocket_server_handle_command(server, client_idx, command_text, &ds);
  if (ds.data) {
    strncpy(out_response, ds.data, max_len - 1);
    out_response[max_len - 1] = '\0';
  } else {
    out_response[0] = '\0';
  }
  dyn_string_free(&ds);
}

#define websocket_server_handle_command test_handle_command

#include "test_support.h"

static processing_parameters_t* mock_params = NULL;

static bool mock_get_status(void* ctx, state_update_t* out_status) {
  (void)ctx;
  if (out_status) {
    out_status->state =
        mock_params ? PROCESSING_STATE_RUNNING : PROCESSING_STATE_INACTIVE;
    out_status->stop_reason.type = STOP_REASON_NONE;
  }
  return true;
}

static int mock_get_active_samplerate(void* ctx) {
  (void)ctx;
  return 44100;
}

static bool mock_get_processing_status(void* ctx, double* out_rate_adjust,
                                       double* out_buffer_level,
                                       uint64_t* out_clipped_samples,
                                       double* out_processing_load,
                                       double* out_resampler_load) {
  (void)ctx;
  if (!mock_params) return false;
  if (out_rate_adjust)
    *out_rate_adjust = processing_parameters_get_rate_adjust(mock_params);
  if (out_buffer_level)
    *out_buffer_level = processing_parameters_get_buffer_level(mock_params);
  if (out_clipped_samples)
    *out_clipped_samples =
        processing_parameters_get_clipped_samples(mock_params);
  if (out_processing_load)
    *out_processing_load =
        processing_parameters_get_processing_load(mock_params);
  if (out_resampler_load)
    *out_resampler_load = processing_parameters_get_resampler_load(mock_params);
  return true;
}

static void mock_reset_clipped_samples(void* ctx) {
  (void)ctx;
  if (mock_params) {
    processing_parameters_reset_clipped_samples(mock_params);
  }
}

static bool mock_get_vu_levels(void* ctx, vu_levels_t* out_vu) {
  (void)ctx;
  if (!mock_params || !out_vu) return false;
  out_vu->playback_channels =
      processing_parameters_get_playback_channels(mock_params);
  out_vu->capture_channels =
      processing_parameters_get_capture_channels(mock_params);

  if (out_vu->playback_channels > 0) {
    out_vu->playback_rms =
        (double*)calloc(out_vu->playback_channels, sizeof(double));
    out_vu->playback_peak =
        (double*)calloc(out_vu->playback_channels, sizeof(double));
    processing_parameters_get_playback_signal_rms(
        mock_params, out_vu->playback_rms, out_vu->playback_channels);
    processing_parameters_get_playback_signal_peak(
        mock_params, out_vu->playback_peak, out_vu->playback_channels);
  }
  if (out_vu->capture_channels > 0) {
    out_vu->capture_rms =
        (double*)calloc(out_vu->capture_channels, sizeof(double));
    out_vu->capture_peak =
        (double*)calloc(out_vu->capture_channels, sizeof(double));
    processing_parameters_get_capture_signal_rms(
        mock_params, out_vu->capture_rms, out_vu->capture_channels);
    processing_parameters_get_capture_signal_peak(
        mock_params, out_vu->capture_peak, out_vu->capture_channels);
  }
  return true;
}

static float mock_get_fader_volume(void* ctx, fader_t fader) {
  (void)ctx;
  if (mock_params) {
    return (float)processing_parameters_get_target_volume_for_fader(mock_params,
                                                                    fader);
  }
  return 0.0f;
}

static bool mock_is_fader_muted(void* ctx, fader_t fader) {
  (void)ctx;
  if (mock_params) {
    return processing_parameters_is_muted_for_fader(mock_params, fader);
  }
  return false;
}

static audio_backend_error_type_t simulated_error_type =
    AUDIO_BACKEND_ERR_COMMAND_SEND;
static const char* simulated_error_message = "Simulated error message";

static device_error_type_t simulated_cap_error_type = DEVICE_ERROR_OTHER;
static const char* simulated_cap_error_message = "Simulated cap error";
static bool simulate_cap_error = false;

static bool mock_get_device_capabilities(void* ctx, const char* backend,
                                         const char* device, bool is_capture,
                                         audio_device_descriptor_t** out_desc,
                                         device_error_t* out_err) {
  (void)ctx;
  (void)backend;
  (void)device;
  (void)is_capture;
  (void)out_desc;

  if (simulate_cap_error) {
    if (out_err) {
      device_error_init(out_err, simulated_cap_error_type,
                        simulated_cap_error_message);
    }
    return false;
  }
  return true;
}

static bool simulate_set_config_error = false;
static char* received_config_json = NULL;

static bool mock_set_config_json(void* ctx, const char* json_str,
                                 audio_backend_error_t* out_err) {
  (void)ctx;
  if (simulate_set_config_error) {
    if (out_err) {
      out_err->type = simulated_error_type;
      snprintf(out_err->message, sizeof(out_err->message), "%s",
               simulated_error_message);
    }
    return false;
  }
  if (received_config_json) free(received_config_json);
  received_config_json = strdup(json_str);
  return true;
}

static void mock_set_fader_volume(void* ctx, fader_t fader, float db,
                                  bool instant) {
  (void)ctx;
  if (mock_params) {
    processing_parameters_set_target_volume_for_fader(mock_params, (double)db,
                                                      fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(mock_params,
                                                         (double)db, fader);
    }
  }
}

static void mock_set_fader_mute(void* ctx, fader_t fader, bool mute) {
  (void)ctx;
  if (mock_params) {
    processing_parameters_set_muted_for_fader(mock_params, mute, fader);
  }
}

static char* mock_active_config = NULL;
static char* mock_prev_config = NULL;
static char* mock_state_file_path = NULL;
static char* mock_config_path = NULL;
static bool mock_dirty = false;

static const char* mock_get_state_file(void* ctx) {
  (void)ctx;
  return mock_state_file_path;
}
static bool mock_is_state_dirty(void* ctx) {
  (void)ctx;
  return mock_dirty;
}
static char* mock_get_config_path(void* ctx) {
  (void)ctx;
  return mock_config_path ? strdup(mock_config_path) : NULL;
}
static void mock_set_config_path(void* ctx, const char* path) {
  (void)ctx;
  if (mock_config_path) free(mock_config_path);
  mock_config_path = path ? strdup(path) : NULL;
}
static bool mock_get_active_config_json(void* ctx, char** out_json) {
  (void)ctx;
  if (mock_active_config) {
    *out_json = strdup(mock_active_config);
    return true;
  }
  *out_json = NULL;
  return false;
}
static bool mock_get_previous_config_json(void* ctx, char** out_json) {
  (void)ctx;
  if (mock_prev_config) {
    *out_json = strdup(mock_prev_config);
    return true;
  }
  *out_json = NULL;
  return false;
}
static dsp_engine_t mock_engine;

cdsp_processing_state_t cdsp_get_state(const dsp_engine_t* engine) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  state_update_t status = {0};
  if (mock && mock->get_status && mock->get_status(mock->ctx, &status)) {
    return (cdsp_processing_state_t)status.state;
  }
  return CDSP_PROCESSING_STATE_INACTIVE;
}

void cdsp_get_stop_reason(const dsp_engine_t* engine,
                          cdsp_stop_reason_t* out_reason) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  state_update_t status = {0};
  if (mock && mock->get_status && mock->get_status(mock->ctx, &status)) {
    out_reason->type = (cdsp_stop_reason_type_t)status.stop_reason.type;
    strncpy(out_reason->message, status.stop_reason.message,
            sizeof(out_reason->message) - 1);
    out_reason->format_change_rate = status.stop_reason.format_change_rate;
  }
}

int cdsp_get_capture_rate(const dsp_engine_t* engine) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_capture_rate) {
    return mock->get_capture_rate(mock->ctx);
  }
  return 0;
}

bool cdsp_get_processing_status(const dsp_engine_t* engine,
                                double* out_rate_adjust,
                                double* out_buffer_level,
                                uint64_t* out_clipped_samples,
                                double* out_processing_load,
                                double* out_resampler_load) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_processing_status) {
    return mock->get_processing_status(mock->ctx, out_rate_adjust,
                                       out_buffer_level, out_clipped_samples,
                                       out_processing_load, out_resampler_load);
  }
  return false;
}

void cdsp_reset_clipped_samples(dsp_engine_t* engine) {
  dsp_engine_t* mock = (dsp_engine_t*)engine;
  if (mock && mock->reset_clipped_samples) {
    mock->reset_clipped_samples(mock->ctx);
  }
}

bool cdsp_get_vu_levels(const dsp_engine_t* engine, cdsp_vu_levels_t* out_vu) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (!mock || !mock->get_vu_levels || !out_vu) return false;
  vu_levels_t raw_vu = {0};
  if (mock->get_vu_levels(mock->ctx, &raw_vu)) {
    out_vu->playback_rms = raw_vu.playback_rms;
    out_vu->playback_peak = raw_vu.playback_peak;
    out_vu->capture_rms = raw_vu.capture_rms;
    out_vu->capture_peak = raw_vu.capture_peak;
    out_vu->playback_channels = raw_vu.playback_channels;
    out_vu->capture_channels = raw_vu.capture_channels;
    return true;
  }
  return false;
}

void cdsp_free_vu_levels(cdsp_vu_levels_t* levels) {
  if (levels) {
    if (levels->playback_rms) free(levels->playback_rms);
    if (levels->playback_peak) free(levels->playback_peak);
    if (levels->capture_rms) free(levels->capture_rms);
    if (levels->capture_peak) free(levels->capture_peak);
    memset(levels, 0, sizeof(cdsp_vu_levels_t));
  }
}

bool cdsp_get_available_devices(const char* backend, bool is_input,
                                cdsp_device_info_t** out_devices,
                                size_t* out_count) {
  *out_devices = NULL;
  *out_count = 0;
  return true;
}

bool cdsp_get_device_capabilities(const char* backend, const char* device,
                                  bool is_capture,
                                  cdsp_device_descriptor_t** out_desc,
                                  cdsp_device_error_t* out_err) {
  audio_device_descriptor_t* raw_desc = NULL;
  device_error_t raw_err = {0};
  bool ok = mock_engine.get_device_capabilities(
      mock_engine.ctx, backend, device, is_capture, &raw_desc, &raw_err);
  if (ok) {
    if (out_desc) {
      cdsp_device_descriptor_t* d =
          (cdsp_device_descriptor_t*)malloc(sizeof(cdsp_device_descriptor_t));
      memset(d, 0, sizeof(cdsp_device_descriptor_t));
      *out_desc = d;
    }
  } else {
    if (out_err) {
      switch (raw_err.type) {
        case DEVICE_ERROR_NOT_FOUND:
          out_err->type = CDSP_DEVICE_ERROR_NOT_FOUND;
          break;
        case DEVICE_ERROR_BUSY:
          out_err->type = CDSP_DEVICE_ERROR_BUSY;
          break;
        default:
          out_err->type = CDSP_DEVICE_ERROR_UNKNOWN;
          break;
      }
      strncpy(out_err->message, raw_err.message, sizeof(out_err->message) - 1);
      out_err->message[sizeof(out_err->message) - 1] = '\0';
    }
  }
  return ok;
}

void cdsp_free_device_capabilities(cdsp_device_descriptor_t* desc) {
  if (desc) free(desc);
}

bool cdsp_get_spectrum(dsp_engine_t* engine, bool is_capture, uint32_t channel,
                       double min_freq, double max_freq, size_t n_bins,
                       cdsp_spectrum_t* out_spec) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (!mock || !mock->get_spectrum) return false;
  spectrum_t raw_spec = {0};
  if (mock->get_spectrum(mock->ctx, is_capture, channel, min_freq, max_freq,
                         n_bins, &raw_spec)) {
    out_spec->count = raw_spec.count;
    if (raw_spec.count > 0) {
      double* freqs = (double*)malloc(raw_spec.count * sizeof(double));
      double* mags = (double*)malloc(raw_spec.count * sizeof(double));
      for (size_t i = 0; i < raw_spec.count; i++) {
        freqs[i] = raw_spec.frequencies[i];
        mags[i] = raw_spec.magnitudes[i];
      }
      out_spec->frequencies = freqs;
      out_spec->magnitudes = mags;
    } else {
      out_spec->frequencies = NULL;
      out_spec->magnitudes = NULL;
    }
    return true;
  }
  return false;
}

void cdsp_free_spectrum(cdsp_spectrum_t* spec) {
  if (spec) {
    if (spec->frequencies) free(spec->frequencies);
    if (spec->magnitudes) free(spec->magnitudes);
  }
}

void cdsp_stop(dsp_engine_t* engine) {
  dsp_engine_t* mock = (dsp_engine_t*)engine;
  if (mock && mock->stop) {
    mock->stop(mock->ctx);
  }
}

float cdsp_get_fader_volume(const dsp_engine_t* engine, cdsp_fader_t fader) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_fader_volume) {
    return mock->get_fader_volume(mock->ctx, (fader_t)fader);
  }
  return 0.0f;
}

bool cdsp_get_fader_mute(const dsp_engine_t* engine, cdsp_fader_t fader) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_fader_mute) {
    return mock->get_fader_mute(mock->ctx, (fader_t)fader);
  }
  return false;
}

void cdsp_set_fader_volume(dsp_engine_t* engine, cdsp_fader_t fader, float db,
                           bool instant) {
  dsp_engine_t* mock = (dsp_engine_t*)engine;
  if (mock && mock->set_fader_volume) {
    mock->set_fader_volume(mock->ctx, (fader_t)fader, db, instant);
  }
}

void cdsp_set_fader_mute(dsp_engine_t* engine, cdsp_fader_t fader, bool mute) {
  dsp_engine_t* mock = (dsp_engine_t*)engine;
  if (mock && mock->set_fader_mute) {
    mock->set_fader_mute(mock->ctx, (fader_t)fader, mute);
  }
}

const char* cdsp_get_state_file_path(const dsp_engine_t* engine) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_state_file_path) {
    return mock->get_state_file_path(mock->ctx);
  }
  return NULL;
}

bool cdsp_get_state_file_updated(const dsp_engine_t* engine) {
  const dsp_engine_t* mock = (const dsp_engine_t*)engine;
  if (mock && mock->get_state_file_updated) {
    return mock->get_state_file_updated(mock->ctx);
  }
  return true;
}

static dsp_engine_t mock_engine = {
    .ctx = NULL,
    .get_status = mock_get_status,
    .get_capture_rate = mock_get_active_samplerate,
    .get_processing_status = mock_get_processing_status,
    .reset_clipped_samples = mock_reset_clipped_samples,
    .get_vu_levels = mock_get_vu_levels,
    .get_fader_volume = mock_get_fader_volume,
    .get_fader_mute = mock_is_fader_muted,
    .set_config_json = mock_set_config_json,
    .get_device_capabilities = mock_get_device_capabilities,
    .set_fader_volume = mock_set_fader_volume,
    .set_fader_mute = mock_set_fader_mute,
    .get_state_file_path = mock_get_state_file,
    .get_state_file_updated = mock_is_state_dirty,
    .get_config_file_path = mock_get_config_path,
    .set_config_file_path = mock_set_config_path,
    .get_active_config_json = mock_get_active_config_json,
    .get_previous_config_json = mock_get_previous_config_json};

static cJSON* recv_json(socket_t sock) {
  char buf[4096];
  size_t total = 0;
  while (total < sizeof(buf) - 1) {
    char c;
    ssize_t n = recv(sock, &c, 1, 0);
    if (n <= 0) break;
    buf[total++] = c;
    buf[total] = '\0';
    cJSON* root = cJSON_Parse(buf);
    if (root != NULL) {
      return root;
    }
  }
  return NULL;
}

TEST(test_websocket_commands) {
  websocket_server_t* server = websocket_server_create(54321, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  bool started = websocket_server_start(server);
  ASSERT_TRUE(started);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(54321);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  socket_t sock;
  int conn_res = -1;
  for (int retry = 0; retry < 50; retry++) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!IS_INVALID_SOCKET(sock)) {
      conn_res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
      if (conn_res == 0) {
        break;
      }
      CLOSE_SOCKET(sock);
    }
    sleep_ms(10);
  }
  ASSERT_EQ(0, conn_res);

  // Send GetVersion command
  const char* cmd1 = "\"GetVersion\"";
  send(sock, cmd1, strlen(cmd1), 0);

  cJSON* root1 = recv_json(sock);
  ASSERT_TRUE(root1 != NULL);
  cJSON* val1 = cJSON_GetObjectItem(root1, "GetVersion");
  ASSERT_TRUE(val1 != NULL);
  cJSON* res1 = cJSON_GetObjectItem(val1, "result");
  ASSERT_TRUE(res1 != NULL);
  ASSERT_STR_EQ("Ok", res1->valuestring);
  cJSON* ver1 = cJSON_GetObjectItem(val1, "value");
  ASSERT_TRUE(ver1 != NULL);
  ASSERT_STR_EQ("CamillaDSP-C-Embedded 2.0.0", ver1->valuestring);
  cJSON_Delete(root1);

  // Send GetState command
  const char* cmd2 = "\"GetState\"";
  send(sock, cmd2, strlen(cmd2), 0);

  cJSON* root2 = recv_json(sock);
  ASSERT_TRUE(root2 != NULL);
  cJSON* val2 = cJSON_GetObjectItem(root2, "GetState");
  ASSERT_TRUE(val2 != NULL);
  cJSON* res2 = cJSON_GetObjectItem(val2, "result");
  ASSERT_TRUE(res2 != NULL);
  ASSERT_STR_EQ("Ok", res2->valuestring);
  cJSON* state2 = cJSON_GetObjectItem(val2, "value");
  ASSERT_TRUE(state2 != NULL);
  ASSERT_STR_EQ("Inactive", state2->valuestring);
  cJSON_Delete(root2);

  CLOSE_SOCKET(sock);
  websocket_server_stop(server);
  websocket_server_free(server);
}

TEST(test_websocket_handle_command_direct) {
  mock_config_path = strdup("/tmp/config.json");
  websocket_server_t* server = websocket_server_create(54322, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[4096];
  websocket_server_handle_command(server, 0, "\"GetVersion\"", resp,
                                  sizeof(resp));
  cJSON* root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cJSON* cmd = cJSON_GetObjectItem(root, "GetVersion");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  ASSERT_STR_EQ("CamillaDSP-C-Embedded 2.0.0",
                cJSON_GetObjectItem(cmd, "value")->valuestring);
  cJSON_Delete(root);

  websocket_server_handle_command(server, 0, "\"GetState\"", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetState");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  ASSERT_STR_EQ("Inactive", cJSON_GetObjectItem(cmd, "value")->valuestring);
  cJSON_Delete(root);

  websocket_server_handle_command(server, 0, "\"GetConfigFilePath\"", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetConfigFilePath");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  ASSERT_STR_EQ("/tmp/config.json",
                cJSON_GetObjectItem(cmd, "value")->valuestring);
  cJSON_Delete(root);

  mock_params = processing_parameters_create(2, 2);
  ASSERT_TRUE(mock_params != NULL);

  websocket_server_handle_command(
      server, 0, "{\"SetFaderExternalVolume\":[0,-6.0]}", resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SetFaderExternalVolume");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);

  double target_vol = processing_parameters_get_target_volume_for_fader(
      mock_params, FADER_MAIN);
  double current_vol = processing_parameters_get_current_volume_for_fader(
      mock_params, FADER_MAIN);
  ASSERT_DOUBLE_EQ(-6.0, target_vol);
  ASSERT_DOUBLE_EQ(-6.0, current_vol);

  // Test GetChannelLabels
  mock_active_config = strdup(
      "{\"devices\":{\"playback\":{\"labels\":[\"Left\",\"Right\"]},"
      "\"capture\":{\"labels\":[\"Mic\"]}}}");
  websocket_server_handle_command(server, 0, "\"GetChannelLabels\"", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetChannelLabels");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(val != NULL);
  cJSON* pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL);
  ASSERT_EQ(2, cJSON_GetArraySize(pb));
  ASSERT_STR_EQ("Left", cJSON_GetArrayItem(pb, 0)->valuestring);
  ASSERT_STR_EQ("Right", cJSON_GetArrayItem(pb, 1)->valuestring);
  cJSON* cap = cJSON_GetObjectItem(val, "capture");
  ASSERT_TRUE(cap != NULL);
  ASSERT_EQ(1, cJSON_GetArraySize(cap));
  ASSERT_STR_EQ("Mic", cJSON_GetArrayItem(cap, 0)->valuestring);
  cJSON_Delete(root);
  free(mock_active_config);
  mock_active_config = NULL;

  // Test SubscribeVuLevels (simple)
  websocket_server_handle_command(server, 0, "\"SubscribeVuLevels\"", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SubscribeVuLevels");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);
  ASSERT_TRUE(websocket_server_get_client_vu_subscribed(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_max_rate(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_attack(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_release(server, 0));

  // Test SubscribeVuLevels (with arguments)
  websocket_server_set_client_vu_subscribed(server, 0, false);
  websocket_server_handle_command(server, 0,
                                  "{\"SubscribeVuLevels\":{\"max_rate\":100.0,"
                                  "\"attack\":10.0,\"release\":100.0}}",
                                  resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SubscribeVuLevels");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);
  ASSERT_TRUE(websocket_server_get_client_vu_subscribed(server, 0));
  ASSERT_DOUBLE_EQ(100.0, websocket_server_get_client_vu_max_rate(server, 0));
  ASSERT_DOUBLE_EQ(10.0, websocket_server_get_client_vu_attack(server, 0));
  ASSERT_DOUBLE_EQ(100.0, websocket_server_get_client_vu_release(server, 0));

  processing_parameters_free(mock_params);
  mock_params = NULL;

  websocket_server_free(server);
  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }
}

TEST(test_backend_error_description) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_DEVICE_NOT_FOUND, "Test device");
  char buf[256];
  backend_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Device not found: Test device", buf);
}

TEST(test_websocket_error_translation) {
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);
  simulate_set_config_error = true;

  char resp[4096];
  cJSON* root;
  cJSON* cmd;
  cJSON* result;
  cJSON* err_val;

  // 1. Test ConfigValidationError translation
  simulated_error_type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
  simulated_error_message = "Failed to parse JSON";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SetConfigJson");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "ConfigValidationError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("Failed to parse JSON", err_val->valuestring);
  cJSON_Delete(root);

  // 2. Test DeviceNotFoundError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
  simulated_error_message = "hw:0 not found";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SetConfigJson");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "DeviceNotFoundError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("hw:0 not found", err_val->valuestring);
  cJSON_Delete(root);

  // 3. Test DeviceBusyError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_BUSY;
  simulated_error_message = "hw:0 in use";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SetConfigJson");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "DeviceBusyError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("hw:0 in use", err_val->valuestring);
  cJSON_Delete(root);

  // 4. Test capabilities DeviceNotFoundError translation
  simulate_cap_error = true;
  simulated_cap_error_type = DEVICE_ERROR_NOT_FOUND;
  simulated_cap_error_message = "hw:0 not found";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetCaptureDeviceCapabilities");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "DeviceNotFoundError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("hw:0 not found", err_val->valuestring);
  cJSON_Delete(root);

  // 5. Test capabilities DeviceBusyError translation
  simulated_cap_error_type = DEVICE_ERROR_BUSY;
  simulated_cap_error_message = "hw:0 busy";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetCaptureDeviceCapabilities");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "DeviceBusyError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("hw:0 busy", err_val->valuestring);
  cJSON_Delete(root);

  // 6. Test capabilities Generic DeviceError translation
  simulated_cap_error_type = DEVICE_ERROR_OTHER;
  simulated_cap_error_message = "hw:0 bad driver";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetCaptureDeviceCapabilities");
  ASSERT_TRUE(cmd != NULL);
  result = cJSON_GetObjectItem(cmd, "result");
  ASSERT_TRUE(result != NULL);
  err_val = cJSON_GetObjectItem(result, "DeviceError");
  ASSERT_TRUE(err_val != NULL);
  ASSERT_STR_EQ("hw:0 bad driver", err_val->valuestring);
  cJSON_Delete(root);

  simulate_cap_error = false;
  simulate_set_config_error = false;

  websocket_server_free(server);
}

TEST(test_websocket_patch_config) {
  simulate_set_config_error = false;
  simulate_cap_error = false;
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_active_config = strdup(
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "  },\n"
      "  \"filters\": {\n"
      "    \"mygain\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": {\"gain\": -6.0}\n"
      "    }\n"
      "  }\n"
      "}");

  char resp[4096];
  const char* patch_cmd =
      "{\"PatchConfig\":{"
      "  \"filters\":{"
      "    \"mygain\":{"
      "      \"parameters\":{\"gain\":-3.0}"
      "    }"
      "  }"
      "}}";

  if (received_config_json) {
    free(received_config_json);
    received_config_json = NULL;
  }

  websocket_server_handle_command(server, 0, patch_cmd, resp, sizeof(resp));
  cJSON* root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cJSON* cmd = cJSON_GetObjectItem(root, "PatchConfig");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);

  ASSERT_TRUE(received_config_json != NULL);

  cJSON* rx_config = cJSON_Parse(received_config_json);
  ASSERT_TRUE(rx_config != NULL);
  cJSON* filters = cJSON_GetObjectItem(rx_config, "filters");
  ASSERT_TRUE(filters != NULL);
  cJSON* mygain = cJSON_GetObjectItem(filters, "mygain");
  ASSERT_TRUE(mygain != NULL);
  cJSON* params = cJSON_GetObjectItem(mygain, "parameters");
  ASSERT_TRUE(params != NULL);
  cJSON* gain = cJSON_GetObjectItem(params, "gain");
  ASSERT_TRUE(gain != NULL);
  ASSERT_DOUBLE_EQ(-3.0, gain->valuedouble);
  cJSON_Delete(rx_config);

  free(mock_active_config);
  mock_active_config = NULL;

  if (received_config_json) {
    free(received_config_json);
    received_config_json = NULL;
  }

  websocket_server_free(server);
}

TEST(test_websocket_format_alignments) {
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_active_config = strdup("{\"my_config\": true}");
  char resp[4096];
  cJSON* root;
  cJSON* cmd;
  cJSON* value;

  // 1. GetConfig value format (should be a JSON string, not parsed object)
  websocket_server_handle_command(server, 0, "\"GetConfig\"", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetConfig");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  value = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_String, value->type);
  cJSON* parsed_val = cJSON_Parse(value->valuestring);
  ASSERT_TRUE(parsed_val != NULL);
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(parsed_val, "my_config")));
  cJSON_Delete(parsed_val);
  cJSON_Delete(root);

  // 2. ReadConfigJson value format (should return input config string as value)
  const char* valid_cfg =
      "{\\\"devices\\\":{\\\"samplerate\\\":44100,\\\"chunksize\\\":1024,"
      "\\\"capture\\\":{\\\"type\\\":\\\"File\\\",\\\"channels\\\":2},"
      "\\\"playback\\\":{\\\"type\\\":\\\"File\\\",\\\"channels\\\":2}}}";
  char read_cmd[1024];
  snprintf(read_cmd, sizeof(read_cmd), "{\"ReadConfigJson\":\"%s\"}",
           valid_cfg);
  websocket_server_handle_command(server, 0, read_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "ReadConfigJson");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  value = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_String, value->type);
  cJSON* parsed_cfg = cJSON_Parse(value->valuestring);
  ASSERT_TRUE(parsed_cfg != NULL);
  cJSON* devices = cJSON_GetObjectItem(parsed_cfg, "devices");
  ASSERT_TRUE(devices != NULL);
  ASSERT_EQ(44100, cJSON_GetObjectItem(devices, "samplerate")->valueint);
  cJSON_Delete(parsed_cfg);
  cJSON_Delete(root);

  // 3. GetFaderVolume value format (should be [idx, vol] array)
  mock_params = processing_parameters_create(2, 2);
  websocket_server_handle_command(server, 0, "{\"GetFaderVolume\":0}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetFaderVolume");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  value = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_DOUBLE_EQ(0.0, cJSON_GetArrayItem(value, 1)->valuedouble);
  cJSON_Delete(root);

  // 4. GetFaderMute value format (should be [idx, mute] array)
  websocket_server_handle_command(server, 0, "{\"GetFaderMute\":0}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "GetFaderMute");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  value = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_FALSE(cJSON_IsTrue(cJSON_GetArrayItem(value, 1)));
  cJSON_Delete(root);

  // 5. AdjustFaderVolume with optional limits in nested array format: [0, [2.5,
  // -30.0, 10.0]]
  websocket_server_handle_command(
      server, 0, "{\"AdjustFaderVolume\":[0, [2.5, -30.0, 10.0]]}", resp,
      sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "AdjustFaderVolume");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  value = cJSON_GetObjectItem(cmd, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_DOUBLE_EQ(2.5, cJSON_GetArrayItem(value, 1)->valuedouble);
  cJSON_Delete(root);

  // 6. YAML Config Commands: SetConfig and ReadConfig / ValidateConfig
  const char* valid_yaml =
      "devices:\\n  samplerate: 44100\\n  chunksize: 1024\\n  capture:\\n    "
      "type: File\\n    channels: 2\\n    filename: \\\"/dev/null\\\"\\n    "
      "format: S16LE\\n  playback:\\n    type: File\\n    channels: 2\\n    "
      "filename: \\\"/dev/null\\\"\\n    format: S16LE\\n";
  char yaml_cmd[1024];
  snprintf(yaml_cmd, sizeof(yaml_cmd), "{\"SetConfig\":\"%s\"}", valid_yaml);
  websocket_server_handle_command(server, 0, yaml_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "SetConfig");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);

  snprintf(yaml_cmd, sizeof(yaml_cmd), "{\"ReadConfig\":\"%s\"}", valid_yaml);
  websocket_server_handle_command(server, 0, yaml_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  cmd = cJSON_GetObjectItem(root, "ReadConfig");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(cmd, "result")->valuestring);
  cJSON_Delete(root);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  free(mock_active_config);
  mock_active_config = NULL;

  websocket_server_free(server);
}

TEST_MAIN()
