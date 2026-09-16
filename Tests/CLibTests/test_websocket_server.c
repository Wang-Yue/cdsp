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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/processing_parameters.h"
#include "Backend/audio_backend.h"
#include "Backend/backend_error.h"
#include "Config/cJSON.h"
#include "Config/engine_config_types.h"
#include "Engine/dsp_engine.h"  // IWYU pragma: keep
#include "Server/websocket_server.h"
#include "Server/websocket_server_internal.h"
#include "Server/ws_framing.h"
#include "Utils/cdsp_time.h"
#include "cdsp/cdsp_pub_types.h"
#include "cdsp/general.h"
#include "cdsp/processing.h"

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
  size_t pb_ch = processing_parameters_get_playback_channels(mock_params);
  size_t cap_ch = processing_parameters_get_capture_channels(mock_params);
  out_vu->playback_channels = pb_ch;
  out_vu->capture_channels = cap_ch;

  if (out_vu->playback_rms && pb_ch > 0) {
    processing_parameters_get_playback_signal_rms(mock_params,
                                                  out_vu->playback_rms, pb_ch);
  }
  if (out_vu->playback_peak && pb_ch > 0) {
    processing_parameters_get_playback_signal_peak(
        mock_params, out_vu->playback_peak, pb_ch);
  }
  if (out_vu->capture_rms && cap_ch > 0) {
    processing_parameters_get_capture_signal_rms(mock_params,
                                                 out_vu->capture_rms, cap_ch);
  }
  if (out_vu->capture_peak && cap_ch > 0) {
    processing_parameters_get_capture_signal_peak(mock_params,
                                                  out_vu->capture_peak, cap_ch);
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
    processing_parameters_set_target_volume_for_fader(mock_params, db, fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(mock_params, db,
                                                         fader);
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
static bool mock_get_signal_levels_since(void* ctx, bool is_capture,
                                         bool is_rms, uint64_t since_ms,
                                         float* out_levels,
                                         size_t* out_channels) {
  (void)ctx;
  (void)since_ms;
  if (out_channels) *out_channels = 2;
  if (out_levels) {
    if (is_capture) {
      out_levels[0] = is_rms ? -6.0f : -3.0f;
      out_levels[1] = is_rms ? -7.0f : -4.0f;
    } else {
      out_levels[0] = is_rms ? -8.0f : -5.0f;
      out_levels[1] = is_rms ? -9.0f : -6.0f;
    }
  }
  return true;
}

static bool mock_spectrum_should_fail = false;
static const char* mock_spectrum_error = NULL;

static bool mock_get_spectrum(void* ctx, bool is_capture, const size_t* channel,
                              double min_freq, double max_freq, uint32_t n_bins,
                              spectrum_t* out_spec) {
  (void)ctx;
  (void)is_capture;
  (void)channel;
  (void)min_freq;
  (void)max_freq;
  if (mock_spectrum_should_fail) {
    if (out_spec && mock_spectrum_error) {
      snprintf(out_spec->error_message, sizeof(out_spec->error_message), "%s",
               mock_spectrum_error);
    }
    return false;
  }
  if (out_spec) {
    out_spec->count = n_bins;
    for (uint32_t i = 0; i < n_bins; i++) {
      if (out_spec->frequencies) out_spec->frequencies[i] = (float)(i * 10);
      if (out_spec->magnitudes) out_spec->magnitudes[i] = -20.0f;
    }
  }
  return true;
}

static uint64_t mock_get_chunk_generation(void* ctx, bool is_capture) {
  (void)ctx;
  if (!mock_params) return 0;
  return processing_parameters_get_chunk_generation(mock_params, is_capture);
}

static dsp_engine_t mock_engine = {
    .ctx = NULL,
    .get_status = mock_get_status,
    .get_capture_rate = mock_get_active_samplerate,
    .get_processing_status = mock_get_processing_status,
    .reset_clipped_samples = mock_reset_clipped_samples,
    .get_vu_levels = mock_get_vu_levels,
    .get_chunk_generation = mock_get_chunk_generation,
    .get_signal_levels_since = mock_get_signal_levels_since,
    .get_spectrum = mock_get_spectrum,
    .get_fader_volume = mock_get_fader_volume,
    .get_fader_mute = mock_is_fader_muted,
    .set_config_json = mock_set_config_json,
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
    cdsp_sleep_ms(10);
  }
  ASSERT_EQ(0, conn_res);

  // Send GetVersion command
  const char* cmd1 = "{\"command\":\"GetVersion\"}";
  send(sock, cmd1, strlen(cmd1), 0);

  cJSON* root1 = recv_json(sock);
  ASSERT_TRUE(root1 != NULL);
  ASSERT_STR_EQ("GetVersion", cJSON_GetObjectItem(root1, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root1, "result")->valuestring);
  ASSERT_STR_EQ(cdsp_get_version(),
                cJSON_GetObjectItem(root1, "value")->valuestring);
  cJSON_Delete(root1);

  // Send GetState command
  const char* cmd2 = "{\"command\":\"GetState\"}";
  send(sock, cmd2, strlen(cmd2), 0);

  cJSON* root2 = recv_json(sock);
  ASSERT_TRUE(root2 != NULL);
  ASSERT_STR_EQ("GetState", cJSON_GetObjectItem(root2, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root2, "result")->valuestring);
  ASSERT_STR_EQ("Inactive", cJSON_GetObjectItem(root2, "value")->valuestring);
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
  websocket_server_handle_command(server, 0, "{\"command\":\"GetVersion\"}",
                                  resp, sizeof(resp));
  cJSON* root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetVersion", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ(cdsp_get_version(),
                cJSON_GetObjectItem(root, "value")->valuestring);
  cJSON_Delete(root);

  websocket_server_handle_command(server, 0, "{\"command\":\"GetState\"}", resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetState", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ("Inactive", cJSON_GetObjectItem(root, "value")->valuestring);
  cJSON_Delete(root);

  websocket_server_handle_command(server, 0, "{\"command\":\"GetCaptureRate\"}",
                                  resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetCaptureRate",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_EQ(44100, cJSON_GetObjectItem(root, "value")->valueint);
  cJSON_Delete(root);

  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetConfigFilePath\"}", resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetConfigFilePath",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ("/tmp/config.json",
                cJSON_GetObjectItem(root, "value")->valuestring);
  cJSON_Delete(root);

  mock_params = processing_parameters_create(2, 2);
  ASSERT_TRUE(mock_params != NULL);

  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SetFaderExternalVolume\",\"fader\":0,\"value\":-6.0}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetFaderExternalVolume",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  float target_vol = processing_parameters_get_target_volume_for_fader(
      mock_params, FADER_MAIN);
  float current_vol = processing_parameters_get_current_volume_for_fader(
      mock_params, FADER_MAIN);
  ASSERT_FLOAT_EQ(-6.0f, target_vol);
  ASSERT_FLOAT_EQ(-6.0f, current_vol);

  // Test GetChannelLabels (no mixer in pipeline -> playback falls back to
  // capture labels)
  mock_active_config = strdup(
      "{\"devices\":{\"playback\":{\"labels\":[\"Left\",\"Right\"]},"
      "\"capture\":{\"labels\":[\"Mic\"]}}}");
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetChannelLabels\"}", resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetChannelLabels",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  cJSON* pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL);
  ASSERT_EQ(1, cJSON_GetArraySize(pb));
  ASSERT_STR_EQ("Mic", cJSON_GetArrayItem(pb, 0)->valuestring);
  cJSON* cap = cJSON_GetObjectItem(val, "capture");
  ASSERT_TRUE(cap != NULL);
  ASSERT_EQ(1, cJSON_GetArraySize(cap));
  ASSERT_STR_EQ("Mic", cJSON_GetArrayItem(cap, 0)->valuestring);
  cJSON_Delete(root);
  free(mock_active_config);
  mock_active_config = NULL;

  // Test SubscribeVuLevels (simple)
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SubscribeVuLevels\"}", resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SubscribeVuLevels",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
  ASSERT_TRUE(websocket_server_get_client_vu_subscribed(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_max_rate(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_attack(server, 0));
  ASSERT_DOUBLE_EQ(0.0, websocket_server_get_client_vu_release(server, 0));

  // Test SubscribeVuLevels (with arguments)
  websocket_server_set_client_vu_subscribed(server, 0, false);
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SubscribeVuLevels\",\"value\":{\"max_rate\":100.0,"
      "\"attack\":10.0,\"release\":100.0}}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SubscribeVuLevels",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
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

  // 1. Test ConfigValidationError translation
  simulated_error_type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
  simulated_error_message = "Failed to parse JSON";
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetConfigJson\",\"value\":\"{}\"}", resp,
      sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("ConfigValidationError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ("Failed to parse JSON",
                cJSON_GetObjectItem(root, "message")->valuestring);
  cJSON_Delete(root);

  // 2. Test DeviceNotFoundError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
  simulated_error_message = "hw:0 not found";
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetConfigJson\",\"value\":\"{}\"}", resp,
      sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("DeviceNotFoundError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ("hw:0 not found",
                cJSON_GetObjectItem(root, "message")->valuestring);
  cJSON_Delete(root);

  // 3. Test DeviceBusyError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_BUSY;
  simulated_error_message = "hw:0 in use";
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetConfigJson\",\"value\":\"{}\"}", resp,
      sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("DeviceBusyError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ("hw:0 in use",
                cJSON_GetObjectItem(root, "message")->valuestring);
  cJSON_Delete(root);

  // 4. Test capabilities DeviceNotFoundError translation
#if defined(ENABLE_COREAUDIO)
  const char* cap_cmd =
      "{\"command\":\"GetCaptureDeviceCapabilities\",\"value\":[\"coreaudio\", "
      "\"NonExistentDevice12345\"]}";
#elif defined(ENABLE_ALSA)
  const char* cap_cmd =
      "{\"command\":\"GetCaptureDeviceCapabilities\",\"value\":[\"alsa\", "
      "\"NonExistentDevice12345\"]}";
#elif defined(ENABLE_WASAPI)
  const char* cap_cmd =
      "{\"command\":\"GetCaptureDeviceCapabilities\",\"value\":[\"wasapi\", "
      "\"NonExistentDevice12345\"]}";
#endif
#if defined(ENABLE_COREAUDIO) || defined(ENABLE_ALSA) || defined(ENABLE_WASAPI)
  websocket_server_handle_command(server, 0, cap_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetCaptureDeviceCapabilities",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("DeviceNotFoundError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
#endif

  // 5. Test capabilities Generic DeviceError translation (unknown backend)
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetCaptureDeviceCapabilities\",\"value\":[\"unsupported_"
      "backend\", \"dummy\"]}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetCaptureDeviceCapabilities",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("DeviceError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  simulate_set_config_error = false;

  websocket_server_free(server);
}

TEST(test_websocket_patch_config) {
  simulate_set_config_error = false;
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");

  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_active_config = strdup(
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"RawFile\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2},\n"
      "    \"playback\": {\"type\": \"File\", \"filename\": \"/dev/null\", "
      "\"format\": \"S16_LE\", \"channels\": 2}\n"
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
      "{\"command\":\"PatchConfig\",\"value\":{"
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
  ASSERT_STR_EQ("PatchConfig", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
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
  cJSON* value;

  // 1. GetConfigJson value format (should be a JSON string, not parsed object)
  websocket_server_handle_command(server, 0, "{\"command\":\"GetConfigJson\"}",
                                  resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_String, value->type);
  cJSON* parsed_val = cJSON_Parse(value->valuestring);
  ASSERT_TRUE(parsed_val != NULL);
  ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(parsed_val, "my_config")));
  cJSON_Delete(parsed_val);
  cJSON_Delete(root);

  // 1b. GetConfig value format (should be a YAML string)
  websocket_server_handle_command(server, 0, "{\"command\":\"GetConfig\"}",
                                  resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetConfig", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_String, value->type);
  ASSERT_TRUE(strstr(value->valuestring, "my_config:") != NULL);
  cJSON_Delete(root);

  // 2. ReadConfigJson value format (should return input config string as value)
  const char* valid_cfg =
      "{\\\"devices\\\":{\\\"samplerate\\\":44100,\\\"chunksize\\\":1024,"
      "\\\"capture\\\":{\\\"type\\\":\\\"RawFile\\\",\\\"filename\\\":\\\"/dev/"
      "null\\\",\\\"format\\\":\\\"S16_LE\\\",\\\"channels\\\":2},"
      "\\\"playback\\\":{\\\"type\\\":\\\"File\\\",\\\"filename\\\":\\\"/dev/"
      "null\\\",\\\"format\\\":\\\"S16_LE\\\",\\\"channels\\\":2}}}";
  char read_cmd[1024];
  snprintf(read_cmd, sizeof(read_cmd),
           "{\"command\":\"ReadConfigJson\",\"value\":\"%s\"}", valid_cfg);
  websocket_server_handle_command(server, 0, read_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ReadConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
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
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetFaderVolume\",\"fader\":0}", resp,
      sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetFaderVolume",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_DOUBLE_EQ(0.0, cJSON_GetArrayItem(value, 1)->valuedouble);
  cJSON_Delete(root);

  // 4. GetFaderMute value format (should be [idx, mute] array)
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"GetFaderMute\",\"fader\":0}",
                                  resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetFaderMute",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_FALSE(cJSON_IsTrue(cJSON_GetArrayItem(value, 1)));
  cJSON_Delete(root);

  // 5. AdjustFaderVolume with optional limits:
  // {"command":"AdjustFaderVolume","fader":0,"value":2.5,"min":-30.0,"max":10.0}
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"AdjustFaderVolume\",\"fader\":0,\"value\":2.5,\"min\":-"
      "30.0,\"max\":10.0}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("AdjustFaderVolume",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  value = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(value != NULL);
  ASSERT_EQ(cJSON_Array, value->type);
  ASSERT_EQ(2, cJSON_GetArraySize(value));
  ASSERT_EQ(0, cJSON_GetArrayItem(value, 0)->valueint);
  ASSERT_DOUBLE_EQ(2.5, cJSON_GetArrayItem(value, 1)->valuedouble);
  cJSON_Delete(root);

  // 6. YAML Config Commands: SetConfig and ReadConfig / ValidateConfig
  const char* valid_yaml =
      "devices:\\n  samplerate: 44100\\n  chunksize: 1024\\n  capture:\\n    "
      "type: RawFile\\n    channels: 2\\n    filename: \\\"/dev/null\\\"\\n    "
      "format: S16_LE\\n  playback:\\n    type: File\\n    channels: 2\\n    "
      "filename: \\\"/dev/null\\\"\\n    format: S16_LE\\n";
  char yaml_cmd[1024];
  snprintf(yaml_cmd, sizeof(yaml_cmd),
           "{\"command\":\"SetConfig\",\"value\":\"%s\"}", valid_yaml);
  websocket_server_handle_command(server, 0, yaml_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetConfig", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  snprintf(yaml_cmd, sizeof(yaml_cmd),
           "{\"command\":\"ReadConfig\",\"value\":\"%s\"}", valid_yaml);
  websocket_server_handle_command(server, 0, yaml_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ReadConfig", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  free(mock_active_config);
  mock_active_config = NULL;

  websocket_server_free(server);
}

TEST(WebSocket_SignalLevelsSinceRPC) {
  websocket_server_t* server = websocket_server_create(8097, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024] = {0};

  // Test GetCaptureSignalPeakSince
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetCaptureSignalPeakSince\",\"value\":1.0}",
      resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  cJSON* val = cJSON_GetObjectItemCaseSensitive(res, "value");
  ASSERT_TRUE(cJSON_IsArray(val));
  ASSERT_EQ(2, cJSON_GetArraySize(val));
  ASSERT_NEAR(-3.0f, (float)cJSON_GetArrayItem(val, 0)->valuedouble, 1e-3);
  ASSERT_NEAR(-4.0f, (float)cJSON_GetArrayItem(val, 1)->valuedouble, 1e-3);
  cJSON_Delete(res);

  // Test GetPlaybackSignalRmsSince
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetPlaybackSignalRmsSince\",\"value\":1.0}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  val = cJSON_GetObjectItemCaseSensitive(res, "value");
  ASSERT_TRUE(cJSON_IsArray(val));
  ASSERT_EQ(2, cJSON_GetArraySize(val));
  ASSERT_NEAR(-8.0f, (float)cJSON_GetArrayItem(val, 0)->valuedouble, 1e-3);
  ASSERT_NEAR(-9.0f, (float)cJSON_GetArrayItem(val, 1)->valuedouble, 1e-3);
  cJSON_Delete(res);

  websocket_server_free(server);
}

TEST(WebSocket_NoAudioSignalLevelsReturnsEmptyArrays) {
  // Server without engine attached (e.g. idle/inactive or no audio flowing)
  websocket_server_t* server = websocket_server_create(8098, "127.0.0.1");
  ASSERT_TRUE(server != NULL);

  char resp[1024] = {0};

  // 1. GetCaptureSignalRms should return "value": []
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetCaptureSignalRms\"}", resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_TRUE(cJSON_IsArray(val));
  ASSERT_EQ(0, cJSON_GetArraySize(val));
  cJSON_Delete(res);

  // 2. GetSignalLevels should return all 4 keys as empty arrays []
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetSignalLevels\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_TRUE(cJSON_IsObject(val));

  cJSON* pb_rms = cJSON_GetObjectItem(val, "playback_rms");
  ASSERT_TRUE(pb_rms != NULL);
  ASSERT_TRUE(cJSON_IsArray(pb_rms));
  ASSERT_EQ(0, cJSON_GetArraySize(pb_rms));

  cJSON* pb_peak = cJSON_GetObjectItem(val, "playback_peak");
  ASSERT_TRUE(pb_peak != NULL);
  ASSERT_TRUE(cJSON_IsArray(pb_peak));
  ASSERT_EQ(0, cJSON_GetArraySize(pb_peak));

  cJSON* cap_rms = cJSON_GetObjectItem(val, "capture_rms");
  ASSERT_TRUE(cap_rms != NULL);
  ASSERT_TRUE(cJSON_IsArray(cap_rms));
  ASSERT_EQ(0, cJSON_GetArraySize(cap_rms));

  cJSON* cap_peak = cJSON_GetObjectItem(val, "capture_peak");
  ASSERT_TRUE(cap_peak != NULL);
  ASSERT_TRUE(cJSON_IsArray(cap_peak));
  ASSERT_EQ(0, cJSON_GetArraySize(cap_peak));

  cJSON_Delete(res);

  websocket_server_free(server);
}

TEST(WebSocket_StopSubscription) {
  websocket_server_t* server = websocket_server_create(8099, "127.0.0.1");
  ASSERT_TRUE(server != NULL);

  char resp[1024] = {0};

  // 1. With no active subscription, should return
  // {"reply":"Invalid","error":"No active subscription"}
  websocket_server_handle_command(
      server, 0, "{\"command\":\"StopSubscription\"}", resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("No active subscription",
                cJSON_GetObjectItem(res, "error")->valuestring);
  cJSON_Delete(res);

  // 2. Start a subscription (e.g. SubscribeState)
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"SubscribeState\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // 3. Now StopSubscription should succeed with Ok
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"StopSubscription\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("StopSubscription",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // 4. Calling StopSubscription again should return Invalid
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"StopSubscription\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("No active subscription",
                cJSON_GetObjectItem(res, "error")->valuestring);
  cJSON_Delete(res);

  websocket_server_free(server);
}

TEST(WebSocket_ReloadErrors) {
  websocket_server_t* server = websocket_server_create(8100, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024] = {0};

  // 1. Without config path set, should return InvalidRequestError "Config path
  // not given, cannot reload"
  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }
  websocket_server_handle_command(server, 0, "{\"command\":\"Reload\"}", resp,
                                  sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Reload", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("Config path not given, cannot reload",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  // 2. Set an invalid non-existent config path -> read error maps to
  // ConfigValidationError
  mock_config_path = strdup("/nonexistent/file/path.yml");
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"Reload\"}", resp,
                                  sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Reload", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("ConfigValidationError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }
  websocket_server_free(server);
}

TEST(WebSocket_SetConfigFilePath) {
  websocket_server_t* server = websocket_server_create(8105, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024] = {0};

  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }

  // 1. Non-existent file path returns InvalidValueError and does not set path
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SetConfigFilePath\",\"value\":\"/tmp/"
      "nonexistent_cfg_12345.yml\"}",
      resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("SetConfigFilePath",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidValueError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_TRUE(cJSON_GetObjectItem(res, "message") != NULL);
  cJSON_Delete(res);
  ASSERT_TRUE(mock_config_path == NULL);

  // 2. Valid file path returns Ok and sets path
  char json_path[256];
  snprintf(json_path, sizeof(json_path), "/tmp/test_set_config_%d.json",
           (int)getpid());
  const char* json_config =
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"RawFile\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2, \"filename\": "
      "\"/dev/null\", \"format\": \"S16_LE\"}\n"
      "  }\n"
      "}";
  FILE* f = fopen(json_path, "w");
  ASSERT_TRUE(f != NULL);
  fputs(json_config, f);
  fclose(f);

  char req[512];
  snprintf(req, sizeof(req),
           "{\"command\":\"SetConfigFilePath\",\"value\":\"%s\"}", json_path);
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, req, resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("SetConfigFilePath",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);
  ASSERT_TRUE(mock_config_path != NULL);
  ASSERT_STR_EQ(json_path, mock_config_path);

  remove(json_path);
  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }
  websocket_server_free(server);
}

TEST(WebSocket_GetConfigEmptyReturnsNullString) {
  websocket_server_t* server = websocket_server_create(8101, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024] = {0};

  if (mock_active_config) {
    free(mock_active_config);
    mock_active_config = NULL;
  }
  if (mock_prev_config) {
    free(mock_prev_config);
    mock_prev_config = NULL;
  }

  // 1. GetConfig with no config loaded -> Ok, value: "null\n"
  websocket_server_handle_command(server, 0, "{\"command\":\"GetConfig\"}",
                                  resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetConfig", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("null\n", cJSON_GetObjectItem(res, "value")->valuestring);
  cJSON_Delete(res);

  // 2. GetConfigJson with no config loaded -> Ok, value: "null"
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"GetConfigJson\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetConfigJson",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("null", cJSON_GetObjectItem(res, "value")->valuestring);
  cJSON_Delete(res);

  // 3. GetPreviousConfig with no previous config -> Ok, value: "null\n"
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetPreviousConfig\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetPreviousConfig",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("null\n", cJSON_GetObjectItem(res, "value")->valuestring);
  cJSON_Delete(res);

  websocket_server_free(server);
}

TEST(WebSocket_FaderVolumeErrorResponsesWithValue) {
  websocket_server_t* server = websocket_server_create(8102, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  mock_params = processing_parameters_create(2, 2);
  processing_parameters_set_current_volume_for_fader(mock_params, -12.5, 0);
  processing_parameters_set_target_volume_for_fader(mock_params, -12.5, 0);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024] = {0};
  cJSON* res;
  cJSON* val;

  // 1. GetFaderVolume with invalid fader -> InvalidFaderError, value: [10, 0.0]
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetFaderVolume\",\"fader\":10}", resp,
      sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetFaderVolume",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidFaderError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsArray(val));
  ASSERT_EQ(10, cJSON_GetArrayItem(val, 0)->valueint);
  ASSERT_NEAR(0.0, cJSON_GetArrayItem(val, 1)->valuedouble, 1e-4);
  cJSON_Delete(res);

  // 2. GetFaderMute with invalid fader -> InvalidFaderError, value: [10, false]
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"GetFaderMute\",\"fader\":10}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetFaderMute", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidFaderError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsArray(val));
  ASSERT_EQ(10, cJSON_GetArrayItem(val, 0)->valueint);
  ASSERT_FALSE(cJSON_IsTrue(cJSON_GetArrayItem(val, 1)));
  cJSON_Delete(res);

  // 3. ToggleFaderMute with invalid fader -> InvalidFaderError, value: [10,
  // false]
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"ToggleFaderMute\",\"fader\":10}", resp,
      sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("ToggleFaderMute",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidFaderError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsArray(val));
  ASSERT_EQ(10, cJSON_GetArrayItem(val, 0)->valueint);
  ASSERT_FALSE(cJSON_IsTrue(cJSON_GetArrayItem(val, 1)));
  cJSON_Delete(res);

  // 4. AdjustFaderVolume with invalid fader -> InvalidFaderError, value:
  // [10, 2.0]
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"AdjustFaderVolume\",\"fader\":10,\"value\":2.0}", resp,
      sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("AdjustFaderVolume",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidFaderError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsArray(val));
  ASSERT_EQ(10, cJSON_GetArrayItem(val, 0)->valueint);
  ASSERT_NEAR(2.0, cJSON_GetArrayItem(val, 1)->valuedouble, 1e-4);
  cJSON_Delete(res);

  // 5. AdjustVolume with max < min -> InvalidValueError, value: current volume
  // (-12.5)
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"AdjustVolume\",\"value\":1.0,\"min\":10.0,\"max\":0.0}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("AdjustVolume", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidValueError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("Max volume must be bigger than min volume",
                cJSON_GetObjectItem(res, "message")->valuestring);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsNumber(val));
  ASSERT_NEAR(-12.5, val->valuedouble, 1e-4);
  cJSON_Delete(res);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  websocket_server_free(server);
}

TEST(test_websocket_signal_peaks_since_start) {
  websocket_server_t* server = websocket_server_create(54329, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_params = processing_parameters_create(2, 2);
  float cap_peaks[2] = {-6.0205999f, -20.0f};  // ~0.5, ~0.1 linear
  float pb_peaks[2] = {0.0f, -6.0205999f};     // 1.0, ~0.5 linear
  processing_parameters_set_capture_signal_peak(mock_params, cap_peaks, 2);
  processing_parameters_set_playback_signal_peak(mock_params, pb_peaks, 2);

  char resp[1024];
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"GetSignalPeaksSinceStart\"}",
                                  resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetSignalPeaksSinceStart",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsObject(val));

  cJSON* cap = cJSON_GetObjectItem(val, "capture");
  ASSERT_TRUE(cap != NULL && cJSON_IsArray(cap));
  ASSERT_EQ(2, cJSON_GetArraySize(cap));
  ASSERT_FALSE(cJSON_IsNull(cJSON_GetArrayItem(cap, 0)));
  ASSERT_NEAR(0.5, cJSON_GetArrayItem(cap, 0)->valuedouble, 1e-3);
  ASSERT_NEAR(0.1, cJSON_GetArrayItem(cap, 1)->valuedouble, 1e-3);

  cJSON* pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL && cJSON_IsArray(pb));
  ASSERT_EQ(2, cJSON_GetArraySize(pb));
  ASSERT_FALSE(cJSON_IsNull(cJSON_GetArrayItem(pb, 0)));
  ASSERT_NEAR(1.0, cJSON_GetArrayItem(pb, 0)->valuedouble, 1e-3);
  ASSERT_NEAR(0.5, cJSON_GetArrayItem(pb, 1)->valuedouble, 1e-3);
  cJSON_Delete(res);

  // Reset signal peaks
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"ResetSignalPeaksSinceStart\"}", resp,
      sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("ResetSignalPeaksSinceStart",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // Set peaks to -inf (silence) and query again: values should be 0.0, NOT null
  float silence[2] = {-INFINITY, -INFINITY};
  processing_parameters_set_capture_signal_peak(mock_params, silence, 2);
  processing_parameters_set_playback_signal_peak(mock_params, silence, 2);

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"GetSignalPeaksSinceStart\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsObject(val));
  cap = cJSON_GetObjectItem(val, "capture");
  ASSERT_TRUE(cap != NULL && cJSON_IsArray(cap));
  ASSERT_EQ(2, cJSON_GetArraySize(cap));
  ASSERT_FALSE(cJSON_IsNull(cJSON_GetArrayItem(cap, 0)));
  ASSERT_NEAR(0.0, cJSON_GetArrayItem(cap, 0)->valuedouble, 1e-6);
  ASSERT_NEAR(0.0, cJSON_GetArrayItem(cap, 1)->valuedouble, 1e-6);

  pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL && cJSON_IsArray(pb));
  ASSERT_EQ(2, cJSON_GetArraySize(pb));
  ASSERT_FALSE(cJSON_IsNull(cJSON_GetArrayItem(pb, 0)));
  ASSERT_NEAR(0.0, cJSON_GetArrayItem(pb, 0)->valuedouble, 1e-6);
  ASSERT_NEAR(0.0, cJSON_GetArrayItem(pb, 1)->valuedouble, 1e-6);
  cJSON_Delete(res);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  websocket_server_free(server);
}

TEST(test_websocket_get_signal_range) {
  websocket_server_t* server = websocket_server_create(54330, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_params = processing_parameters_create(2, 2);
  // Capture peak -6.0206 dB (linear ~0.5), playback peak 0.0 dB (linear 1.0)
  float cap_peaks[2] = {-6.0205999f, -20.0f};
  float pb_peaks[2] = {0.0f, 0.0f};
  processing_parameters_set_capture_signal_peak(mock_params, cap_peaks, 2);
  processing_parameters_set_playback_signal_peak(mock_params, pb_peaks, 2);

  char resp[512];
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"GetSignalRange\"}",
                                  resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetSignalRange",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsNumber(val));
  // Range should be 2.0 * 0.5 = 1.0 from capture side, not 2.0 * 1.0 = 2.0 from
  // playback
  ASSERT_NEAR(1.0, val->valuedouble, 1e-3);
  cJSON_Delete(res);

  // Test Public API function directly
  double pub_range = cdsp_get_signal_range((dsp_engine_t*)&mock_engine);
  ASSERT_NEAR(1.0, pub_range, 1e-3);

  // When capture is silent (-INFINITY), range should be 0.0
  float silence[2] = {-INFINITY, -INFINITY};
  processing_parameters_set_capture_signal_peak(mock_params, silence, 2);
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"GetSignalRange\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL && cJSON_IsNumber(val));
  ASSERT_NEAR(0.0, val->valuedouble, 1e-6);
  cJSON_Delete(res);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  websocket_server_free(server);
}

TEST(WebSocket_ChannelLabelsMixerAndFallback) {
  websocket_server_t* server = websocket_server_create(8106, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024];

  // 1. Pipeline with multiple mixers: playback gets labels from the LAST mixer
  // in pipeline
  mock_active_config = strdup(
      "{\n"
      "  \"devices\": {\"capture\": {\"labels\": [\"Cap0\", \"Cap1\"]}},\n"
      "  \"mixers\": {\n"
      "    \"m1\": {\"labels\": [\"M1_0\", \"M1_1\"]},\n"
      "    \"m2\": {\"labels\": [\"M2_0\", \"M2_1\", \"M2_2\"]}\n"
      "  },\n"
      "  \"pipeline\": [\n"
      "    {\"type\": \"Mixer\", \"name\": \"m1\"},\n"
      "    {\"type\": \"Filter\", \"channels\": [0], \"names\": [\"gain\"]},\n"
      "    {\"type\": \"Mixer\", \"name\": \"m2\"}\n"
      "  ]\n"
      "}");

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetChannelLabels\"}", resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  cJSON* val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL);
  cJSON* pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL && cJSON_IsArray(pb));
  ASSERT_EQ(3, cJSON_GetArraySize(pb));
  ASSERT_STR_EQ("M2_0", cJSON_GetArrayItem(pb, 0)->valuestring);
  ASSERT_STR_EQ("M2_1", cJSON_GetArrayItem(pb, 1)->valuestring);
  ASSERT_STR_EQ("M2_2", cJSON_GetArrayItem(pb, 2)->valuestring);
  cJSON_Delete(res);
  free(mock_active_config);

  // 2. Last mixer has no labels: playback is null even if earlier mixer has
  // labels
  mock_active_config = strdup(
      "{\n"
      "  \"devices\": {\"capture\": {\"labels\": [\"Cap0\"]}},\n"
      "  \"mixers\": {\n"
      "    \"m1\": {\"labels\": [\"M1_0\"]},\n"
      "    \"m2\": {}\n"
      "  },\n"
      "  \"pipeline\": [\n"
      "    {\"type\": \"Mixer\", \"name\": \"m1\"},\n"
      "    {\"type\": \"Mixer\", \"name\": \"m2\"}\n"
      "  ]\n"
      "}");

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetChannelLabels\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL);
  pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL && cJSON_IsNull(pb));
  cJSON_Delete(res);
  free(mock_active_config);

  // 3. Mixer has empty array labels: [] -> returns []
  mock_active_config = strdup(
      "{\n"
      "  \"mixers\": {\"m\": {\"labels\": []}},\n"
      "  \"pipeline\": [{\"type\": \"Mixer\", \"name\": \"m\"}]\n"
      "}");

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetChannelLabels\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  val = cJSON_GetObjectItem(res, "value");
  ASSERT_TRUE(val != NULL);
  pb = cJSON_GetObjectItem(val, "playback");
  ASSERT_TRUE(pb != NULL && cJSON_IsArray(pb));
  ASSERT_EQ(0, cJSON_GetArraySize(pb));
  cJSON_Delete(res);
  free(mock_active_config);
  mock_active_config = NULL;

  websocket_server_free(server);
}

TEST(WebSocket_SpectrumValidationOrderAndErrors) {
  websocket_server_t* server = websocket_server_create(8107, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024];

  // 1. When processing is inactive (mock_params == NULL):
  // Arguments are validated FIRST before reporting ProcessingNotRunningError

  // 1a. n_bins < 2 fails with InvalidRequestError
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetSpectrum\",\"value\":{\"side\":\"capture\",\"min_"
      "freq\":20,\"max_freq\":20000,\"n_bins\":1}}",
      resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetSpectrum", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("n_bins must be at least 2",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  // 1b. min_freq >= max_freq fails with InvalidRequestError
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetSpectrum\",\"value\":{\"side\":\"capture\",\"min_"
      "freq\":500,\"max_freq\":100,\"n_bins\":1024}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("Invalid frequency range: min_freq must be > 0 and < max_freq",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  // 1c. Valid arguments but processing inactive -> ProcessingNotRunningError
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetSpectrum\",\"value\":{\"side\":\"capture\",\"min_"
      "freq\":20,\"max_freq\":20000,\"n_bins\":1024}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("ProcessingNotRunningError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // 2. SubscribeSpectrum validation order and max_rate check
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SubscribeSpectrum\",\"value\":{\"side\":\"capture\","
      "\"min_freq\":20,\"max_freq\":20000,\"n_bins\":1}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("n_bins must be at least 2",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SubscribeSpectrum\",\"value\":{\"side\":\"capture\","
      "\"min_freq\":20,\"max_freq\":20000,\"n_bins\":1024,\"max_rate\":-1.0}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("max_rate must be > 0",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  // 3. When processing is running, backend error message is propagated as
  // InvalidRequestError
  mock_params = processing_parameters_create(2, 2);
  mock_spectrum_should_fail = true;
  mock_spectrum_error = "Insufficient data in buffer";

  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetSpectrum\",\"value\":{\"side\":\"capture\",\"min_"
      "freq\":20,\"max_freq\":20000,\"n_bins\":1024}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("Insufficient data in buffer",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  mock_spectrum_error = "Channel 3 out of range (2 channels available)";
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetSpectrum\",\"value\":{\"side\":\"capture\","
      "\"channel\":3,\"min_freq\":20,\"max_freq\":20000,\"n_bins\":1024}}",
      resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("InvalidRequestError",
                cJSON_GetObjectItem(res, "result")->valuestring);
  ASSERT_STR_EQ("Channel 3 out of range (2 channels available)",
                cJSON_GetObjectItem(res, "message")->valuestring);
  cJSON_Delete(res);

  mock_spectrum_should_fail = false;
  mock_spectrum_error = NULL;
  processing_parameters_free(mock_params);
  mock_params = NULL;

  websocket_server_free(server);
}

TEST(WebSocket_StreamingExclusivity) {
  websocket_server_t* server = websocket_server_create(8108, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[1024];

  // 1. Subscribe to State
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"SubscribeState\"}",
                                  resp, sizeof(resp));
  cJSON* res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("SubscribeState",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // 2. While streaming, non-StopSubscription commands are rejected with Invalid
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"GetVersion\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Only StopSubscription is accepted while streaming is active",
                cJSON_GetObjectItem(res, "error")->valuestring);
  cJSON_Delete(res);

  // 3. Second subscription while streaming is also rejected
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SubscribeVuLevels\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Only StopSubscription is accepted while streaming is active",
                cJSON_GetObjectItem(res, "error")->valuestring);
  cJSON_Delete(res);

  // 4. StopSubscription ends streaming
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"StopSubscription\"}", resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("StopSubscription",
                cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  // 5. Commands now work again
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, "{\"command\":\"GetVersion\"}",
                                  resp, sizeof(resp));
  res = cJSON_Parse(resp);
  ASSERT_TRUE(res != NULL);
  ASSERT_STR_EQ("GetVersion", cJSON_GetObjectItem(res, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(res, "result")->valuestring);
  cJSON_Delete(res);

  websocket_server_free(server);
}

TEST(WebSocket_ReadAndValidateConfigDefaultsAndValidation) {
  websocket_server_t* server = websocket_server_create(8109, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char resp[2048];

  // 1. Valid config with ReadConfigJson: should expand optional fields to null
  // defaults
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"ReadConfigJson\",\"value\":\""
      "{\\\"devices\\\":{\\\"samplerate\\\":44100,\\\"chunksize\\\":1024,"
      "\\\"capture\\\":{\\\"type\\\":\\\"RawFile\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"},"
      "\\\"playback\\\":{\\\"type\\\":\\\"File\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"}}}\"}",
      resp, sizeof(resp));
  cJSON* root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ReadConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON* val_str = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val_str != NULL && cJSON_IsString(val_str));
  cJSON* expanded = cJSON_Parse(val_str->valuestring);
  ASSERT_TRUE(expanded != NULL);
  // Verify defaults expanded
  ASSERT_TRUE(cJSON_HasObjectItem(expanded, "title"));
  ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(expanded, "title")));
  ASSERT_TRUE(cJSON_HasObjectItem(expanded, "filters"));
  ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(expanded, "filters")));
  cJSON* dev = cJSON_GetObjectItem(expanded, "devices");
  ASSERT_TRUE(dev != NULL);
  ASSERT_TRUE(cJSON_HasObjectItem(dev, "queuelimit"));
  ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(dev, "queuelimit")));
  cJSON_Delete(expanded);
  cJSON_Delete(root);

  // 2. Config with invalid pipeline channel index:
  // ReadConfigJson succeeds with Ok (it only parses without pipeline
  // validation)
  const char* invalid_pipe_cfg =
      "{\"command\":\"ReadConfigJson\",\"value\":\""
      "{\\\"devices\\\":{\\\"samplerate\\\":44100,\\\"chunksize\\\":1024,"
      "\\\"capture\\\":{\\\"type\\\":\\\"RawFile\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"},"
      "\\\"playback\\\":{\\\"type\\\":\\\"File\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"}},"
      "\\\"pipeline\\\":[{\\\"type\\\":\\\"Filter\\\",\\\"channels\\\":[99],"
      "\\\"names\\\":[]}]}\"}";
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, invalid_pipe_cfg, resp,
                                  sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ReadConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  // 3. Same invalid pipeline config with ValidateConfigJson: fails with
  // ConfigValidationError
  const char* validate_cmd =
      "{\"command\":\"ValidateConfigJson\",\"value\":\""
      "{\\\"devices\\\":{\\\"samplerate\\\":44100,\\\"chunksize\\\":1024,"
      "\\\"capture\\\":{\\\"type\\\":\\\"RawFile\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"},"
      "\\\"playback\\\":{\\\"type\\\":\\\"File\\\",\\\"channels\\\":2,"
      "\\\"filename\\\":\\\"/dev/null\\\",\\\"format\\\":\\\"S16_LE\\\"}},"
      "\\\"pipeline\\\":[{\\\"type\\\":\\\"Filter\\\",\\\"channels\\\":[99],"
      "\\\"names\\\":[]}]}\"}";
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(server, 0, validate_cmd, resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ValidateConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("ConfigValidationError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_TRUE(cJSON_GetObjectItem(root, "value") != NULL);
  cJSON_Delete(root);

  // 4. Syntax error with ReadConfigJson: fails with ConfigReadError
  memset(resp, 0, sizeof(resp));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"ReadConfigJson\",\"value\":\"{invalid_json\"}",
      resp, sizeof(resp));
  root = cJSON_Parse(resp);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("ReadConfigJson",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("ConfigReadError",
                cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_TRUE(cJSON_GetObjectItem(root, "value") != NULL);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST(test_websocket_frame_parsing) {
  size_t payload_len = 0;
  size_t header_len = 0;
  unsigned char* mask = NULL;
  uint8_t opcode = 0;
  bool fin = false;

  // 1. Unfragmented unmasked text frame
  const unsigned char frame1[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
  bool ok = ws_parse_frame_header_ext(frame1, sizeof(frame1), &payload_len,
                                      &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(fin);
  ASSERT_EQ(1, (int)opcode);
  ASSERT_EQ(5, (int)payload_len);
  ASSERT_EQ(2, (int)header_len);
  ASSERT_TRUE(mask == NULL);

  // 2. Fragmented initial text frame (fin = false)
  const unsigned char frame2[] = {0x01, 0x03, 'a', 'b', 'c'};
  ok = ws_parse_frame_header_ext(frame2, sizeof(frame2), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_FALSE(fin);
  ASSERT_EQ(1, (int)opcode);
  ASSERT_EQ(3, (int)payload_len);
  ASSERT_EQ(2, (int)header_len);

  // 3. Continuation frame (fin = false, opcode = 0)
  const unsigned char frame3[] = {0x00, 0x02, 'd', 'e'};
  ok = ws_parse_frame_header_ext(frame3, sizeof(frame3), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_FALSE(fin);
  ASSERT_EQ(0, (int)opcode);
  ASSERT_EQ(2, (int)payload_len);

  // 4. Final continuation frame (fin = true, opcode = 0)
  const unsigned char frame4[] = {0x80, 0x01, 'f'};
  ok = ws_parse_frame_header_ext(frame4, sizeof(frame4), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(fin);
  ASSERT_EQ(0, (int)opcode);
  ASSERT_EQ(1, (int)payload_len);

  // 5. Masked frame
  const unsigned char frame5[] = {0x81, 0x85, 0x11, 0x22, 0x33, 0x44,
                                  0x00, 0x00, 0x00, 0x00, 0x00};
  ok = ws_parse_frame_header_ext(frame5, sizeof(frame5), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(fin);
  ASSERT_EQ(1, (int)opcode);
  ASSERT_EQ(5, (int)payload_len);
  ASSERT_EQ(6, (int)header_len);
  ASSERT_TRUE(mask != NULL);
  ASSERT_EQ(0x11, mask[0]);
  ASSERT_EQ(0x22, mask[1]);

  // 6. Extended 16-bit payload length (126)
  const unsigned char frame6[] = {0x82, 126, 0x01, 0x00};
  ok = ws_parse_frame_header_ext(frame6, sizeof(frame6), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_EQ(256, (int)payload_len);
  ASSERT_EQ(4, (int)header_len);

  // 7. Extended 64-bit payload length (127)
  const unsigned char frame7[] = {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0};
  ok = ws_parse_frame_header_ext(frame7, sizeof(frame7), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_TRUE(ok);
  ASSERT_EQ(65536, (int)payload_len);
  ASSERT_EQ(10, (int)header_len);

  // 8. Incomplete buffers
  ok = ws_parse_frame_header_ext(frame1, 1, &payload_len, &header_len, &mask,
                                 &opcode, &fin);
  ASSERT_FALSE(ok);
  ok = ws_parse_frame_header_ext(frame6, 3, &payload_len, &header_len, &mask,
                                 &opcode, &fin);
  ASSERT_FALSE(ok);

  // 9. Invalid opcode (e.g., 0x03)
  const unsigned char frame_bad[] = {0x83, 0x00};
  ok = ws_parse_frame_header_ext(frame_bad, sizeof(frame_bad), &payload_len,
                                 &header_len, &mask, &opcode, &fin);
  ASSERT_FALSE(ok);
}

static void test_send_ws_client_frame(socket_t sock, uint8_t opcode, bool fin,
                                      const char* payload, size_t len) {
  uint8_t header[14];
  size_t header_len = 0;
  header[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
  uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};

  if (len < 126) {
    header[1] = 0x80 | (uint8_t)len;
    memcpy(&header[2], mask_key, 4);
    header_len = 6;
  } else if (len <= 65535) {
    header[1] = 0x80 | 126;
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)(len & 0xFF);
    memcpy(&header[4], mask_key, 4);
    header_len = 8;
  } else {
    header[1] = 0x80 | 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (uint8_t)(((uint64_t)len >> ((7 - i) * 8)) & 0xFF);
    }
    memcpy(&header[10], mask_key, 4);
    header_len = 14;
  }

  send(sock, (const char*)header, (int)header_len, 0);
  if (len > 0 && payload) {
    char* masked = (char*)malloc(len);
    for (size_t i = 0; i < len; i++) {
      masked[i] = payload[i] ^ mask_key[i % 4];
    }
    send(sock, masked, (int)len, 0);
    free(masked);
  }
}

static char* test_recv_ws_frame(socket_t sock, uint8_t* out_opcode,
                                size_t* out_len) {
  unsigned char hdr[10];
  ssize_t n = recv(sock, (char*)hdr, 2, 0);
  if (n < 2) return NULL;
  uint8_t opcode = hdr[0] & 0x0F;
  if (out_opcode) *out_opcode = opcode;
  size_t payload_len = hdr[1] & 0x7F;
  if (payload_len == 126) {
    n = recv(sock, (char*)&hdr[2], 2, 0);
    if (n < 2) return NULL;
    payload_len = ((size_t)hdr[2] << 8) | hdr[3];
  } else if (payload_len == 127) {
    n = recv(sock, (char*)&hdr[2], 8, 0);
    if (n < 8) return NULL;
    uint64_t len64 = 0;
    for (int i = 0; i < 8; i++) {
      len64 = (len64 << 8) | hdr[2 + i];
    }
    payload_len = (size_t)len64;
  }
  if (out_len) *out_len = payload_len;
  char* payload = (char*)malloc(payload_len + 1);
  if (!payload) return NULL;
  size_t total = 0;
  while (total < payload_len) {
    n = recv(sock, payload + total, (int)(payload_len - total), 0);
    if (n <= 0) {
      free(payload);
      return NULL;
    }
    total += (size_t)n;
  }
  payload[payload_len] = '\0';
  return payload;
}

TEST(test_websocket_fragmentation_and_limits) {
  websocket_server_t* server = websocket_server_create(54325, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  bool started = websocket_server_start(server);
  ASSERT_TRUE(started);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(54325);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  socket_t sock = -1;
  int conn_res = -1;
  for (int retry = 0; retry < 50; retry++) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!IS_INVALID_SOCKET(sock)) {
      conn_res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
      if (conn_res == 0) break;
      CLOSE_SOCKET(sock);
    }
    cdsp_sleep_ms(10);
  }
  ASSERT_EQ(0, conn_res);

  // 1. Perform WebSocket Handshake
  const char* handshake =
      "GET / HTTP/1.1\r\n"
      "Host: 127.0.0.1:54325\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  send(sock, handshake, (int)strlen(handshake), 0);

  char hs_resp[1024];
  size_t hs_len = 0;
  while (hs_len < sizeof(hs_resp) - 1) {
    ssize_t n = recv(sock, hs_resp + hs_len, 1, 0);
    if (n <= 0) break;
    hs_len++;
    hs_resp[hs_len] = '\0';
    if (strstr(hs_resp, "\r\n\r\n")) break;
  }
  ASSERT_TRUE(strstr(hs_resp, "101 Switching Protocols") != NULL);

  // 2. Send fragmented command in two frames:
  // Frame 1: FIN=0, opcode=0x01 (Text), payload='{"command":'
  const char* part1 = "{\"command\":";
  test_send_ws_client_frame(sock, 0x01, false, part1, strlen(part1));

  // Frame 2: FIN=1, opcode=0x00 (Continuation), payload='"GetVersion"}'
  const char* part2 = "\"GetVersion\"}";
  test_send_ws_client_frame(sock, 0x00, true, part2, strlen(part2));

  // Read response frame
  uint8_t resp_opcode = 0;
  size_t resp_len = 0;
  char* resp_text = test_recv_ws_frame(sock, &resp_opcode, &resp_len);
  ASSERT_TRUE(resp_text != NULL);
  ASSERT_EQ(0x01, (int)resp_opcode);

  cJSON* root = cJSON_Parse(resp_text);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetVersion", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_STR_EQ(cdsp_get_version(),
                cJSON_GetObjectItem(root, "value")->valuestring);
  cJSON_Delete(root);
  free(resp_text);

  // 3. Test frame exceeding 16 MiB limit triggers close code 1009
  uint8_t bad_hdr[14];
  bad_hdr[0] = 0x81;                        // FIN=1, Text
  bad_hdr[1] = 0x80 | 127;                  // Masked, 64-bit length
  uint64_t huge_len = 20ULL * 1024 * 1024;  // 20 MiB > 16 MiB limit
  for (int i = 0; i < 8; i++) {
    bad_hdr[2 + i] = (uint8_t)((huge_len >> ((7 - i) * 8)) & 0xFF);
  }
  bad_hdr[10] = 0;
  bad_hdr[11] = 0;
  bad_hdr[12] = 0;
  bad_hdr[13] = 0;
  send(sock, (const char*)bad_hdr, 14, 0);

  // Expect Close frame with code 1009
  char* close_payload = test_recv_ws_frame(sock, &resp_opcode, &resp_len);
  ASSERT_TRUE(close_payload != NULL);
  ASSERT_EQ(0x08, (int)resp_opcode);
  ASSERT_EQ(2, (int)resp_len);
  uint16_t close_code =
      ((uint8_t)close_payload[0] << 8) | (uint8_t)close_payload[1];
  ASSERT_EQ(1009, (int)close_code);
  free(close_payload);

  CLOSE_SOCKET(sock);
  websocket_server_stop(server);
  websocket_server_free(server);
}

TEST(test_websocket_event_cadence_and_generations) {
  mock_params = processing_parameters_create(2, 2);
  ASSERT_TRUE(mock_params != NULL);

  websocket_server_t* server = websocket_server_create(54326, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  bool started = websocket_server_start(server);
  ASSERT_TRUE(started);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(54326);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  socket_t sock = -1;
  int conn_res = -1;
  for (int retry = 0; retry < 50; retry++) {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (!IS_INVALID_SOCKET(sock)) {
      conn_res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
      if (conn_res == 0) break;
      CLOSE_SOCKET(sock);
    }
    cdsp_sleep_ms(10);
  }
  ASSERT_EQ(0, conn_res);

#ifdef _WIN32
  DWORD timeout_ms = 2000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms,
             sizeof(timeout_ms));
#else
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

  // Perform Handshake
  const char* handshake =
      "GET / HTTP/1.1\r\n"
      "Host: 127.0.0.1:54326\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  send(sock, handshake, (int)strlen(handshake), 0);

  char hs_resp[1024];
  size_t hs_len = 0;
  while (hs_len < sizeof(hs_resp) - 1) {
    ssize_t n = recv(sock, hs_resp + hs_len, 1, 0);
    if (n <= 0) break;
    hs_len++;
    hs_resp[hs_len] = '\0';
    if (strstr(hs_resp, "\r\n\r\n")) break;
  }
  ASSERT_TRUE(strstr(hs_resp, "101 Switching Protocols") != NULL);

  // 1. Subscribe to State
  const char* sub_state = "{\"command\":\"SubscribeState\"}";
  test_send_ws_client_frame(sock, 0x01, true, sub_state, strlen(sub_state));

  uint8_t opcode = 0;
  size_t len = 0;
  char* frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  cJSON* root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SubscribeState",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // Transition state: make mock_params NULL so mock_get_status returns INACTIVE
  processing_parameters_free(mock_params);
  mock_params = NULL;

  // Immediate StateEvent should arrive promptly
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("StateEvent", cJSON_GetObjectItem(root, "reply")->valuestring);
  cJSON* val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_STR_EQ("Inactive", cJSON_GetObjectItem(val, "state")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // Stop state subscription
  const char* stop_sub = "{\"command\":\"StopSubscription\"}";
  test_send_ws_client_frame(sock, 0x01, true, stop_sub, strlen(stop_sub));
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("StopSubscription",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // 2. Subscribe to SignalLevels (playback)
  mock_params = processing_parameters_create(2, 2);
  const char* sub_sig =
      "{\"command\":\"SubscribeSignalLevels\",\"value\":\"playback\"}";
  test_send_ws_client_frame(sock, 0x01, true, sub_sig, strlen(sub_sig));

  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SubscribeSignalLevels",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // Update playback levels with a new chunk to bump playback generation
  audio_chunk_t* chunk = audio_chunk_create(64, 2);
  ASSERT_TRUE(chunk != NULL);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(chunk, ch);
    for (size_t s = 0; s < 64; s++) w[s] = 0.5f;
  }
  audio_chunk_set_valid_frames(chunk, 64);
  processing_parameters_update_playback_levels(mock_params, chunk);

  // Read SignalLevelsEvent triggered by generation increment
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SignalLevelsEvent",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_STR_EQ("playback", cJSON_GetObjectItem(val, "side")->valuestring);
  cJSON* pb_rms = cJSON_GetObjectItem(val, "rms");
  ASSERT_TRUE(pb_rms != NULL && cJSON_GetArraySize(pb_rms) == 2);
  cJSON_Delete(root);
  free(frame);
  audio_chunk_free(chunk);

  // Stop SignalLevels subscription
  test_send_ws_client_frame(sock, 0x01, true, stop_sub, strlen(stop_sub));
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("StopSubscription",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // 3. VU subscription with capture-only pipeline (pb_channels == 0,
  // cap_channels == 2)
  processing_parameters_free(mock_params);
  mock_params = processing_parameters_create(2, 0);

  const char* sub_vu =
      "{\"command\":\"SubscribeVuLevels\",\"value\":{\"max_rate\":100.0}}";
  test_send_ws_client_frame(sock, 0x01, true, sub_vu, strlen(sub_vu));
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SubscribeVuLevels",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);
  free(frame);

  // Update capture levels with a chunk
  audio_chunk_t* cap_chunk = audio_chunk_create(64, 2);
  ASSERT_TRUE(cap_chunk != NULL);
  for (size_t ch = 0; ch < 2; ch++) {
    mutable_waveform_t w = audio_chunk_get_channel(cap_chunk, ch);
    for (size_t s = 0; s < 64; s++) w[s] = 0.25f;
  }
  audio_chunk_set_valid_frames(cap_chunk, 64);
  processing_parameters_update_capture_levels(mock_params, cap_chunk);

  // Read VuLevelsEvent: verify capture has 2 channels and playback has 0
  // channels
  frame = test_recv_ws_frame(sock, &opcode, &len);
  ASSERT_TRUE(frame != NULL);
  root = cJSON_Parse(frame);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("VuLevelsEvent",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  cJSON* vu_cap_pk = cJSON_GetObjectItem(val, "capture_peak");
  ASSERT_TRUE(vu_cap_pk != NULL && cJSON_GetArraySize(vu_cap_pk) == 2);
  cJSON* vu_pb_pk = cJSON_GetObjectItem(val, "playback_peak");
  ASSERT_TRUE(vu_pb_pk != NULL && cJSON_GetArraySize(vu_pb_pk) == 0);
  cJSON_Delete(root);
  free(frame);
  audio_chunk_free(cap_chunk);

  CLOSE_SOCKET(sock);
  websocket_server_stop(server);
  websocket_server_free(server);

  if (mock_params) {
    processing_parameters_free(mock_params);
    mock_params = NULL;
  }
}

TEST(WebSocket_DefaultUpdateInterval) {
  websocket_server_t* server = websocket_server_create(54335, "127.0.0.1");
  ASSERT_TRUE(server != NULL);

  char response[512] = {0};
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"GetUpdateInterval\"}",
                                  response, sizeof(response));

  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetUpdateInterval",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  ASSERT_EQ(1000, cJSON_GetObjectItem(root, "value")->valueint);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST(WebSocket_GetSignalLevelsSince_ClampsRange) {
  websocket_server_t* server = websocket_server_create(54336, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);
  mock_params = processing_parameters_create(2, 2);

  char response[512] = {0};

  // 1. Negative seconds should clamp to 0 without error or overflow
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetCaptureSignalPeakSince\",\"value\":-50.0}",
      response, sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetCaptureSignalPeakSince",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  // 2. Large seconds > 600 should clamp to 600 without underflow
  memset(response, 0, sizeof(response));
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"GetCaptureSignalPeakSince\",\"value\":99999.0}", response,
      sizeof(response));
  root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetCaptureSignalPeakSince",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  processing_parameters_free(mock_params);
  mock_params = NULL;
  websocket_server_free(server);
}

TEST(WebSocket_SetUpdateInterval_RejectsFloatAndNegative) {
  websocket_server_t* server = websocket_server_create(54337, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char response[512] = {0};
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetUpdateInterval\",\"value\":1.5}", response,
      sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(root, "reply")->valuestring);
  cJSON_Delete(root);

  memset(response, 0, sizeof(response));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetUpdateInterval\",\"value\":-10}", response,
      sizeof(response));
  root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(root, "reply")->valuestring);
  cJSON_Delete(root);

  memset(response, 0, sizeof(response));
  websocket_server_handle_command(
      server, 0, "{\"command\":\"SetUpdateInterval\",\"value\":250}", response,
      sizeof(response));
  root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetUpdateInterval",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST(WebSocket_SubscribeSignalLevels_RejectsInvalidSide) {
  websocket_server_t* server = websocket_server_create(54338, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char response[512] = {0};
  websocket_server_handle_command(
      server, 0,
      "{\"command\":\"SubscribeSignalLevels\",\"value\":\"unknown_side\"}",
      response, sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("Invalid", cJSON_GetObjectItem(root, "reply")->valuestring);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST(WebSocket_GetConfigValue_PreservesStringTypes) {
  websocket_server_t* server = websocket_server_create(54339, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  mock_active_config = strdup(
      "{\"title\":\"true\",\"devices\":{\"capture\":{\"device\":\"1\"}}}");

  char response[512] = {0};
  websocket_server_handle_command(
      server, 0, "{\"command\":\"GetConfigValue\",\"value\":\"/title\"}",
      response, sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetConfigValue",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_TRUE(cJSON_IsString(val));
  ASSERT_STR_EQ("true", val->valuestring);
  cJSON_Delete(root);

  free(mock_active_config);
  mock_active_config = NULL;
  websocket_server_free(server);
}

TEST(WebSocket_SetVolume_InfinityClamped) {
  websocket_server_t* server = websocket_server_create(54340, "127.0.0.1");
  websocket_server_set_engine(server, (dsp_engine_t*)&mock_engine);

  char response[512] = {0};
  // 1e400 evaluates to +INFINITY in IEEE 754 float
  websocket_server_handle_command(server, 0,
                                  "{\"command\":\"SetVolume\",\"value\":1e400}",
                                  response, sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("SetVolume", cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST(WebSocket_GetRateAdjust_DefaultsToZero) {
  websocket_server_t* server = websocket_server_create(54341, "127.0.0.1");
  // Engine is NULL, so query fails and should return 0.0 default matching
  // upstream
  websocket_server_set_engine(server, NULL);

  char response[512] = {0};
  websocket_server_handle_command(server, 0, "{\"command\":\"GetRateAdjust\"}",
                                  response, sizeof(response));
  cJSON* root = cJSON_Parse(response);
  ASSERT_TRUE(root != NULL);
  ASSERT_STR_EQ("GetRateAdjust",
                cJSON_GetObjectItem(root, "reply")->valuestring);
  ASSERT_STR_EQ("Ok", cJSON_GetObjectItem(root, "result")->valuestring);
  cJSON* val = cJSON_GetObjectItem(root, "value");
  ASSERT_TRUE(val != NULL);
  ASSERT_NEAR(0.0, val->valuedouble, 1e-6);
  cJSON_Delete(root);

  websocket_server_free(server);
}

TEST_MAIN()
