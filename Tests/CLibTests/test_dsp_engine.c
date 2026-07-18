#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "Engine/dsp_engine.h"
#include "Engine/engine_capture_loop.h"
#include "Engine/engine_playback_loop.h"
#include "Engine/engine_processing_loop.h"
#include "Engine/engine_shared_state.h"
#include "Audio/audio_chunk.h"
#include "Backend/file_backend.h"
#include "Backend/generator_capture.h"
#include "Logging/app_logger.h"
#include "Pipeline/config_loader.h"
#include "Pipeline/pipeline.h"
#include "Public/config.h"
#include "Public/devices.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/signal_levels.h"
#include "Public/volume.h"
#include "Utils/cdsp_time.h"
#include "test_support.h"

static void run_e2e_test_config(const char* json, const char* backend_name) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  cdsp_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = cdsp_set_config_json(engine, json, &err);
  if (!success) {
    printf(
        "⚠️ [E2E Warning] Skipping E2E test for backend '%s' (Initialization "
        "failed: %s)\n",
        backend_name, err.message);
    if (engine && engine->free) engine->free(engine->ctx);
    return;
  }

  cdsp_sleep_ms(100);

  cdsp_vu_levels_t vu = {0};
  cdsp_get_vu_levels(engine, &vu);
  cdsp_free_vu_levels(&vu);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
  printf("✅ [E2E Success] Backend '%s' ran successfully\n", backend_name);
}

TEST(DSPEngineCreateFree) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineDeviceCapabilities) {
  cdsp_device_info_t* devs = NULL;
  size_t count = 0;
  bool ok = cdsp_get_available_devices("coreaudio", false, &devs, &count);
  ASSERT_TRUE(ok);

  // Test freeing NULL descriptor (should be a safe no-op)
  cdsp_free_device_capabilities(NULL);

  if (count > 0 && devs) {
    cdsp_device_descriptor_t* desc = NULL;
    if (cdsp_get_device_capabilities("coreaudio", devs[0].name, false, &desc,
                                     NULL)) {
      if (desc) {
        cdsp_free_device_capabilities(desc);
      }
    }
    free(devs);
  }
}

TEST(DSPEngineSetConfigAndReload) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

#if defined(__linux__)
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels_in\": 2,\n"
      "            \"channels_out\": 2,\n"
      "            \"mapping\": [{\n"
      "                \"dest\": 0,\n"
      "                \"sources\": [{\"channel\": 0, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }, {\n"
      "                \"dest\": 1,\n"
      "                \"sources\": [{\"channel\": 1, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";
#elif defined(_WIN32)
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        }\n"
      "    }\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels_in\": 2,\n"
      "            \"channels_out\": 2,\n"
      "            \"mapping\": [{\n"
      "                \"dest\": 0,\n"
      "                \"sources\": [{\"channel\": 0, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }, {\n"
      "                \"dest\": 1,\n"
      "                \"sources\": [{\"channel\": 1, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";
#else
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels_in\": 2,\n"
      "            \"channels_out\": 2,\n"
      "            \"mapping\": [{\n"
      "                \"dest\": 0,\n"
      "                \"sources\": [{\"channel\": 0, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }, {\n"
      "                \"dest\": 1,\n"
      "                \"sources\": [{\"channel\": 1, \"gain\": 0.0, "
      "\"inverted\": "
      "false, \"mute\": false}]\n"
      "            }]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";
#endif

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success1 = engine->set_config_json(engine->ctx, json1, &err);
  if (!success1) {
    printf("ERROR: json1 set_config failed: %s\n", err.message);
  }
  ASSERT_TRUE(success1);

  bool success2 = engine->set_config_json(engine->ctx, json2, &err);
  if (!success2) {
    printf("ERROR: json2 set_config failed: %s\n", err.message);
  }
  ASSERT_TRUE(success2);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->mixers_count);
  ASSERT_EQ(1, active->pipeline_count);
  dsp_config_free(active);
  free(active_json);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineHotParameterReload) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

#if defined(__linux__)
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -6.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";
#elif defined(_WIN32)
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -6.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";
#else
  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -6.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";
#endif

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success1 = engine->set_config_json(engine->ctx, json1, &err);
  ASSERT_TRUE(success1);

  bool success2 = engine->set_config_json(engine->ctx, json2, &err);
  ASSERT_TRUE(success2);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->filters_count);
  ASSERT_EQ(-3.0, active->filters[0].filter.parameters.gain.gain);
  dsp_config_free(active);
  free(active_json);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineSetConfigStruct) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
#elif defined(_WIN32)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2,\n"
      "            \"polling\": true\n"
      "        }\n"
      "    }\n"
      "}";
#else
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
#endif

  dsp_config_t* parsed = NULL;
  config_error_t cerr;
  int parse_res = config_loader_parse(json, &parsed, &cerr);
  ASSERT_EQ(0, parse_res);
  ASSERT_TRUE(parsed != NULL);

  const char* json_override =
#if defined(__linux__)
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 4\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 4\n"
      "        }\n"
      "    }\n"
      "}";
#elif defined(_WIN32)
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Wasapi\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
#else
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 4\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 4\n"
      "        }\n"
      "    }\n"
      "}";
#endif

  audio_backend_error_t berr;
  bool ok = engine->set_config_json(engine->ctx, json_override, &berr);
  ASSERT_TRUE(ok);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr2 = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr2));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(48000, active->devices.samplerate);
#if defined(_WIN32)
  ASSERT_EQ(2, capture_device_config_get_channels(&active->devices.capture));
#else
  ASSERT_EQ(4, capture_device_config_get_channels(&active->devices.capture));
#endif
  dsp_config_free(active);
  free(active_json);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineE2E_ALSA) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Alsa\",\n"
      "            \"device\": \"null\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "ALSA");
#endif
}

TEST(DSPEngineE2E_PipeWire) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Pipewire\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Pipewire\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "PipeWire");
#endif
}

TEST(DSPEngineE2E_CoreAudio) {
#if defined(__APPLE__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"CoreAudio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "CoreAudio");
#endif
}

// Real-world scenario simulated:
// Capturing a synthetic signal generator (like a test tone calibration tool)
// and recording/saving the output directly to a file (offline capturing).
TEST(DSPEngineE2E_GeneratorFile) {
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_out_%d.raw", getpid());
  remove(out_file);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 44100,\n"
           "        \"chunksize\": 512,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 2,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Sine\",\n"
           "                \"freq\": 1000.0,\n"
           "                \"level\": -6.0\n"
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);
  run_e2e_test_config(json, "Generator -> File");
  remove(out_file);
}

// Real-world scenario simulated:
// Offline batch processing (e.g., loading an audio file from disk, applying DSP filters,
// and saving/writing the processed result to another file as fast as CPU permits).
TEST(DSPEngineE2E_FileFile) {
  char in_file[256];
  char out_file[256];
  snprintf(in_file, sizeof(in_file), "/tmp/e2e_in_%d.raw", getpid());
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_out_%d.raw", getpid());
  remove(in_file);
  remove(out_file);

  FILE* f = fopen(in_file, "wb");
  ASSERT_TRUE(f != NULL);
  int16_t input_samples[1024 * 2];
  for (int i = 0; i < 1024 * 2; i++) {
    input_samples[i] = (int16_t)i;
  }
  fwrite(input_samples, sizeof(int16_t), 1024 * 2, f);
  fclose(f);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 44100,\n"
           "        \"chunksize\": 512,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}",
           in_file, out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  for (int i = 0; i < 50; i++) {
    if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_INACTIVE) break;
    cdsp_sleep_ms(10);
  }

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);

  FILE* out_f = fopen(out_file, "rb");
  ASSERT_TRUE(out_f != NULL);
  int16_t output_samples[1024 * 2];
  size_t read_count = fread(output_samples, sizeof(int16_t), 1024 * 2, out_f);
  ASSERT_EQ(1024 * 2, read_count);
  fclose(out_f);

  for (int i = 0; i < 1024 * 2; i++) {
    ASSERT_EQ(input_samples[i], output_samples[i]);
  }

  remove(in_file);
  remove(out_file);
}

// Real-world scenario simulated:
// High-performance offline rendering. Verifies that when realtime constraints are not
// requested, the engine processes samples unthrottled far faster than real-time speed.
TEST(DSPEngineE2E_GeneratorFile_SpeedTest) {
  char out_filename[256];
  snprintf(out_filename, sizeof(out_filename), "/tmp/e2e_out_speed_%d.raw",
           getpid());
  remove(out_filename);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 44100,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 256,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 2,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Sine\",\n"
           "                \"freq\": 1000.0,\n"
           "                \"level\": -6.0\n"
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}",
           out_filename);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  ASSERT_TRUE(engine->set_config_json(engine->ctx, json, &err));

  // Sleep for 1500ms in simulated time (~100ms real wall-clock time) to let
  // threads spawn and stream stably on virtual machines.
  cdsp_sleep_ms(1500);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);

  // Check the size of the output file
  FILE* f = fopen(out_filename, "rb");
  ASSERT_TRUE(f != NULL);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fclose(f);

  // At real-time (44.1kHz stereo 16-bit = 176.4KB/sec), 1500ms simulated time
  // is ~264.6KB. Unthrottled generation should easily produce > 400KB of audio
  // in that window (>1.5x to 50x realtime).
  ASSERT_TRUE(size > 400000);

  remove(out_filename);
  printf(
      "✅ [E2E Success] Generator -> File ran unthrottled (produced %ld bytes "
      "in simulated test window)\n",
      size);
}

#if defined(ENABLE_ASIO)
TEST(DSPEngineASIOSetConfigAndReload) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"mixers\": {\n"
      "        \"mymixer\": {\n"
      "            \"channels_in\": 2,\n"
      "            \"channels_out\": 2,\n"
      "            \"mapping\": [{\n"
      "                \"dest\": 0,\n"
      "                \"sources\": [{\"channel\": 0, \"gain\": 0.0, "
      "\"inverted\": false, \"mute\": false}]\n"
      "            }, {\n"
      "                \"dest\": 1,\n"
      "                \"sources\": [{\"channel\": 1, \"gain\": 0.0, "
      "\"inverted\": false, \"mute\": false}]\n"
      "            }]\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Mixer\",\n"
      "        \"name\": \"mymixer\"\n"
      "    }]\n"
      "}";

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success1 = engine->set_config_json(engine->ctx, json1, &err);
  if (!success1) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO SetConfigAndReload test (Failed to set "
        "config: %s)\n",
        err.message);
    if (engine && engine->free) engine->free(engine->ctx);
    return;
  }
  ASSERT_TRUE(success1);

  bool success2 = engine->set_config_json(engine->ctx, json2, &err);
  ASSERT_TRUE(success2);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->mixers_count);
  ASSERT_EQ(1, active->pipeline_count);
  dsp_config_free(active);
  free(active_json);

  dsp_engine_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineASIOHotParameterReload) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char* json1 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -6.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";

  const char* json2 =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    },\n"
      "    \"filters\": {\n"
      "        \"mygain\": {\n"
      "            \"type\": \"Gain\",\n"
      "            \"parameters\": {\n"
      "                \"gain\": -3.0\n"
      "            }\n"
      "        }\n"
      "    },\n"
      "    \"pipeline\": [{\n"
      "        \"type\": \"Filter\",\n"
      "        \"channel\": 0,\n"
      "        \"names\": [\"mygain\"]\n"
      "    }]\n"
      "}";

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success1 = engine->set_config_json(engine->ctx, json1, &err);
  if (!success1) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO HotParameterReload test (Failed to set "
        "config: %s)\n",
        err.message);
    if (engine && engine->free) engine->free(engine->ctx);
    return;
  }
  ASSERT_TRUE(success1);

  bool success2 = engine->set_config_json(engine->ctx, json2, &err);
  ASSERT_TRUE(success2);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->filters_count);
  ASSERT_EQ(-3.0, active->filters[0].filter.parameters.gain.gain);
  dsp_config_free(active);
  free(active_json);

  dsp_engine_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}

TEST(DSPEngineASIOSetConfigStruct) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  dsp_config_t* parsed = NULL;
  config_error_t cerr;
  int parse_res = config_loader_parse(json, &parsed, &cerr);
  ASSERT_EQ(0, parse_res);
  ASSERT_TRUE(parsed != NULL);

  // Apply overrides
  parsed->devices.samplerate = 48000;
  capture_device_config_set_channels(&parsed->devices.capture, 2);

  const char* json_override =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 48000,\n"
      "        \"chunksize\": 1024,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Asio\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  audio_backend_error_t berr;
  bool ok = engine->set_config_json(engine->ctx, json_override, &berr);
  if (!ok) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO SetConfigStruct test (Failed to set "
        "config struct: %s)\n",
        berr.message);
    if (engine && engine->free) engine->free(engine->ctx);
    return;
  }
  ASSERT_TRUE(ok);

  char* active_json = NULL;
  ASSERT_TRUE(engine->get_active_config_json(engine->ctx, &active_json));
  ASSERT_TRUE(active_json != NULL);
  dsp_config_t* active = NULL;
  config_error_t cerr2 = {0};
  ASSERT_EQ(0, config_loader_parse(active_json, &active, &cerr2));
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(48000, active->devices.samplerate);
  ASSERT_EQ(2, capture_device_config_get_channels(&active->devices.capture));
  dsp_config_free(active);
  free(active_json);

  dsp_engine_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
}
#endif

static void run_e2e_file_file_test(bool capture_rt, bool playback_rt,
                                   int total_frames, bool test_time) {
  char in_file[256];
  char out_file[256];
  snprintf(in_file, sizeof(in_file), "/tmp/e2e_rt_in_%d.raw", getpid());
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_rt_out_%d.raw", getpid());
  remove(in_file);
  remove(out_file);

  // Write test input (S16_LE mono)
  FILE* f = fopen(in_file, "wb");
  ASSERT_TRUE(f != NULL);
  int16_t* input_samples = malloc(total_frames * sizeof(int16_t));
  for (int i = 0; i < total_frames; i++) {
    input_samples[i] = (int16_t)(i % 32768);
  }
  fwrite(input_samples, sizeof(int16_t), total_frames, f);
  fclose(f);

  // Build JSON configuration
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 128,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": %s\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": %s\n"
           "        }\n"
           "    }\n"
           "}",
           in_file, capture_rt ? "true" : "false", out_file,
           playback_rt ? "true" : "false");

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  uint64_t t0_ns = cdsp_time_now_ns();

  // Wait for the engine to finish processing the file
  // Timeout is 35.0s in simulated time (running 15x faster -> ~2.33s of real
  // time)
  while (1) {
    if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_INACTIVE) {
      break;
    }
    double cur_elapsed = (double)(cdsp_time_now_ns() - t0_ns) / 1000000000.0;
    if (cur_elapsed >= 35.0) {
      break;
    }
    cdsp_sleep_ms(5);
  }

  // Stop the engine and wait for playback/processing threads to finish draining
  cdsp_stop(engine);

  double elapsed = (double)(cdsp_time_now_ns() - t0_ns) / 1000000000.0;

  if (engine && engine->free) engine->free(engine->ctx);

  // Verify the output content matches input
  FILE* out_f = fopen(out_file, "rb");
  ASSERT_TRUE(out_f != NULL);
  int16_t* output_samples = malloc(total_frames * sizeof(int16_t));
  size_t read_count =
      fread(output_samples, sizeof(int16_t), total_frames, out_f);
  ASSERT_EQ((size_t)total_frames, read_count);
  fclose(out_f);

  for (int i = 0; i < total_frames; i++) {
    ASSERT_EQ(input_samples[i], output_samples[i]);
  }

  free(input_samples);
  free(output_samples);
  remove(in_file);
  remove(out_file);

  if (test_time) {
    // If either capture or playback is realtime, the simulated elapsed time
    // should be close to 30.0s
    bool is_rt = capture_rt || playback_rt;
    if (is_rt) {
      ASSERT_TRUE(elapsed >= 28.0);
      ASSERT_TRUE(elapsed < 60.0);
    } else {
      // Non-realtime should take very little simulated time
      ASSERT_TRUE(elapsed < 15.0);
    }
  }
}

// Real-world scenario simulated:
// Fully offline file processing (e.g. converting a track format or offline DSP offline processing).
// Runs as fast as CPU permits (unthrottled).
TEST(DSPEngineE2E_FileFile_Realtime_FF) {
  run_e2e_file_file_test(false, false, 480000, true);
}

// Real-world scenario simulated:
// Recording from a physical hardware capture device (mic/line-in, which delivers chunks in real-time)
// and saving/writing the output to a local raw PCM file on disk (throttled by capture speed).
TEST(DSPEngineE2E_FileFile_Realtime_TF) {
  run_e2e_file_file_test(true, false, 480000, true);
}

// Real-world scenario simulated:
// Playing back a local music file from disk (e.g., raw PCM audio track)
// and outputting it to a real-world physical DAC device in real-time (throttled by playback DAC speed).
TEST(DSPEngineE2E_FileFile_Realtime_FT) {
  run_e2e_file_file_test(false, true, 480000, true);
}

// Real-world scenario simulated:
// Live real-time audio routing (e.g. mic capture -> DSP processing -> DAC playback).
// Both ends run in real-time (fully throttled).
TEST(DSPEngineE2E_FileFile_Realtime_TT) {
  run_e2e_file_file_test(true, true, 480000, true);
}

struct stop_thread_args {
  dsp_engine_t* engine;
  volatile bool done;
};

static void* stop_thread_func(void* arg) {
  struct stop_thread_args* args = (struct stop_thread_args*)arg;
  cdsp_stop(args->engine);
  args->done = true;
  return NULL;
}

// Real-world scenario simulated:
// Host application stop command race conditions. Simulates a user clicking "Stop" in the UI
// while the playback thread concurrently runs into a device error or shutdown.
// Verifies that cdsp_stop() does not block indefinitely (deadlock) when threads teardown.
TEST(DSPEngineE2E_DeadlockGuard) {
  char in_file[256];
  char out_file[256];
  snprintf(in_file, sizeof(in_file), "/tmp/e2e_deadlock_in_%d.raw", getpid());
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_deadlock_out_%d.raw",
           getpid());
  remove(in_file);
  remove(out_file);

  // Write test input (S16_LE mono, 22.4 simulated seconds @ 16000Hz = 358400
  // frames) This size is specifically chosen (between 1x and 2x queuelimit of
  // 500 chunks) to ensure capture thread exits while processing thread is
  // blocked.
  int total_frames = 358400;
  FILE* f = fopen(in_file, "wb");
  ASSERT_TRUE(f != NULL);
  int16_t* input_samples = malloc(total_frames * sizeof(int16_t));
  for (int i = 0; i < total_frames; i++) {
    input_samples[i] = (int16_t)(i % 32768);
  }
  fwrite(input_samples, sizeof(int16_t), total_frames, f);
  fclose(f);

  // Build JSON configuration with large queuelimit (500)
  // Capture is non-realtime (File), Playback is realtime (File)
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 500,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": false\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           in_file, out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  // Sleep for 100ms real time. Since capture is non-realtime, it will finish
  // reading the entire file (5M frames) and reach EOF/exit immediately. The
  // processing thread will be blocked enqueuing to full processed_queue because
  // playback is realtime (takes ~20s real time under 15x scale to drain).
  cdsp_sleep_ms(100);

  // Stop the engine in a separate thread. If the deadlock bug is present, the
  // thread will hang forever. We wait up to 2.0 seconds (200 * 10ms) for it to
  // complete.
  pthread_t stop_thread;
  struct stop_thread_args stop_args = {.engine = engine, .done = false};
  pthread_create(&stop_thread, NULL, stop_thread_func, &stop_args);

  for (int i = 0; i < 200; i++) {
    if (stop_args.done) {
      break;
    }
    cdsp_sleep_ms(10);  // 10ms
  }

  // Assert that the stop thread completed within 2.0 seconds (no deadlock)
  ASSERT_TRUE(stop_args.done);

  if (stop_args.done) {
    pthread_join(stop_thread, NULL);
  } else {
    pthread_detach(stop_thread);
  }

  if (engine && engine->free) engine->free(engine->ctx);
  free(input_samples);
  remove(in_file);
  remove(out_file);
}

extern volatile bool g_generator_mock_hang;
extern volatile int g_pipeline_swaps_count;

// Real-world scenario simulated:
// Physical audio hardware driver crash/lockup (synchronous lockup inside backend read call).
// Verifies that the external watchdog stall detector running on the main controller thread
// successfully flags the engine state as STALLED instead of remaining stuck in RUNNING.
TEST(DSPEngine_WatchdogStall_Hang_Vulnerability) {
  g_generator_mock_hang = false;

  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/watchdog_hang_out_%d.raw", getpid());
  remove(out_file);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Noise\",\n"
           "                \"level\": -20.0\n"
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  cdsp_sleep_ms(100);
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_RUNNING);

  // Trigger the infinite hang in the capture thread
  g_generator_mock_hang = true;

  // Sleep for 1000ms real time (which is > 0.5s watchdog timeout)
  cdsp_sleep_ms(1000);

  // Poll the engine to trigger the external watchdog check!
  cdsp_engine_poll(engine);

  // Assert that it transitioned to STALLED (verifying the fix!)
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_STALLED);

  // Release the mock hang so the thread can exit cleanly
  g_generator_mock_hang = false;
  cdsp_sleep_ms(50);

  cdsp_stop(engine);

  if (engine && engine->free) engine->free(engine->ctx);
  remove(out_file);
}

// Real-world scenario simulated:
// Staging a filter configuration hot-reload while the audio pipeline is auto-paused (e.g. during silence).
// Verifies that the reload and structural pipeline swap apply immediately without waiting
// for the audio signal to resume.
TEST(DSPEngine_PausedState_PipelineSwap_Delay_Vulnerability) {
  g_pipeline_swaps_count = 0;

  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/paused_reload_out_%d.raw", getpid());
  remove(out_file);

  char json_initial[2048];
  snprintf(json_initial, sizeof(json_initial),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"silence_threshold\": -50.0,\n"
           "        \"silence_timeout_s\": 0.1,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Noise\",\n"
           "                \"level\": -100.0\n"
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);

  char json_new[2048];
  snprintf(json_new, sizeof(json_new),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"silence_threshold\": -50.0,\n"
           "        \"silence_timeout_s\": 0.1,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Noise\",\n"
           "                \"level\": -100.0\n"
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    },\n"
           "    \"filters\": {\n"
           "        \"mygain\": {\n"
           "            \"type\": \"Gain\",\n"
           "            \"parameters\": {\n"
           "                \"gain\": -1.0\n"
           "            }\n"
           "        }\n"
           "    },\n"
           "    \"pipeline\": [\n"
           "        {\n"
           "            \"type\": \"Filter\",\n"
           "            \"channel\": 0,\n"
           "            \"names\": [\"mygain\"]\n"
           "        }\n"
           "    ]\n"
           "}",
           out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json_initial, &err);
  ASSERT_TRUE(success);

  // Wait for auto-pause to trigger
  bool paused = false;
  for (int i = 0; i < 50; i++) {
    if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_PAUSED) {
      paused = true;
      break;
    }
    cdsp_sleep_ms(10);
  }
  ASSERT_TRUE(paused);

  // Reload config with filters to trigger structural reload/swap
  success = engine->set_config_json(engine->ctx, json_new, &err);
  ASSERT_TRUE(success);

  cdsp_sleep_ms(300);

  // Assert that swap HAS occurred (verifying the fix!)
  ASSERT_EQ(1, g_pipeline_swaps_count);

  cdsp_stop(engine);

  if (engine && engine->free) engine->free(engine->ctx);
  remove(out_file);
}

// Real-world scenario simulated:
// Auto-pause and auto-resume. Simulates the audio signal dropping below the threshold
// to trigger fader-mute auto-pause, then a configuration reload with loud noise occurs,
// causing the system to automatically resume back to RUNNING.
TEST(DSPEngineE2E_AutoPauseResume) {
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_autopause_%d.raw", getpid());
  remove(out_file);

  char json_silent[1024];
  snprintf(json_silent, sizeof(json_silent),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"silence_threshold\": -50.0,\n"
           "        \"silence_timeout_s\": 0.1,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Sine\",\n"
           "                \"freq\": 1000.0,\n"
           "                \"level\": -100.0\n" // Silent
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);

  char json_loud[1024];
  snprintf(json_loud, sizeof(json_loud),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"silence_threshold\": -50.0,\n"
           "        \"silence_timeout_s\": 0.1,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Sine\",\n"
           "                \"freq\": 1000.0,\n"
           "                \"level\": -20.0\n" // Loud
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json_silent, &err);
  ASSERT_TRUE(success);

  // Wait for auto-pause to trigger
  bool paused = false;
  for (int i = 0; i < 50; i++) {
    cdsp_engine_poll(engine);
    if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_PAUSED) {
      paused = true;
      break;
    }
    cdsp_sleep_ms(10);
  }
  ASSERT_TRUE(paused);

  // Apply new config with loud noise
  success = engine->set_config_json(engine->ctx, json_loud, &err);
  ASSERT_TRUE(success);

  // Wait for auto-resume to trigger
  bool resumed = false;
  for (int i = 0; i < 50; i++) {
    cdsp_engine_poll(engine);
    if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_RUNNING) {
      resumed = true;
      break;
    }
    cdsp_sleep_ms(10);
  }
  ASSERT_TRUE(resumed);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
  remove(out_file);
}

// Real-world scenario simulated:
// Client app volume/mute changes. Simulates adjusting the playback fader gain (volume)
// and toggling mute on the fly, verifying that level meters reflect changes instantly.
TEST(DSPEngineE2E_FaderVolumeMuteControl) {
  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/e2e_fader_%d.raw", getpid());
  remove(out_file);

  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"capture\": {\n"
           "            \"type\": \"Generator\",\n"
           "            \"channels\": 1,\n"
           "            \"signal\": {\n"
           "                \"type\": \"Sine\",\n"
           "                \"freq\": 1000.0,\n"
           "                \"level\": -6.0\n"\
           "            }\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        }\n"
           "    }\n"
           "}",
           out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  cdsp_sleep_ms(100);

  // Fetch initial VU levels
  cdsp_vu_levels_t vu = {0};
  ASSERT_TRUE(cdsp_get_vu_levels(engine, &vu));
  ASSERT_TRUE(vu.capture_channels == 1);
  ASSERT_TRUE(vu.playback_channels == 1);
  ASSERT_TRUE(vu.capture_peak[0] > -20.0);
  ASSERT_TRUE(vu.playback_peak[0] > -20.0);
  cdsp_free_vu_levels(&vu);

  // Mute main fader
  cdsp_set_fader_mute(engine, CDSP_FADER_MAIN, true);
  ASSERT_TRUE(cdsp_get_fader_mute(engine, CDSP_FADER_MAIN));
  cdsp_sleep_ms(100);

  // Fetch muted VU levels - playback fader is post-mute, so it should be silent
  memset(&vu, 0, sizeof(vu));
  ASSERT_TRUE(cdsp_get_vu_levels(engine, &vu));
  ASSERT_TRUE(vu.playback_peak[0] < -150.0); // Muted
  ASSERT_TRUE(vu.capture_peak[0] > -20.0);    // Capture is pre-fader, still loud
  cdsp_free_vu_levels(&vu);

  // Unmute main fader and set fader volume
  cdsp_set_fader_mute(engine, CDSP_FADER_MAIN, false);
  ASSERT_FALSE(cdsp_get_fader_mute(engine, CDSP_FADER_MAIN));
  cdsp_set_fader_volume(engine, CDSP_FADER_MAIN, -12.0f, true);
  ASSERT_EQ(-12.0f, cdsp_get_fader_volume(engine, CDSP_FADER_MAIN));

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
  remove(out_file);
}

// Real-world scenario simulated:
// Graceful File-to-File rendering completion (EOF Queue Draining Flow - Section 3.5).
// Verifies that when the input file reaches EOF, the capture and processing threads
// terminate early, but the engine state remains RUNNING while the playback thread is still
// draining the queue in real-time. Once fully played, the state transitions to INACTIVE.
TEST(DSPEngineE2E_GracefulTeardown_Sequence) {
  char in_file[256];
  char out_file[256];
  snprintf(in_file, sizeof(in_file), "/tmp/teardown_in_%d.raw", getpid());
  snprintf(out_file, sizeof(out_file), "/tmp/teardown_out_%d.raw", getpid());
  remove(in_file);
  remove(out_file);

  // Write exactly 1024 frames of stereo 16-bit audio (1024 * 2 samples)
  // At chunk size 512, this represents exactly 2 chunks of audio.
  FILE* f = fopen(in_file, "wb");
  ASSERT_TRUE(f != NULL);
  int16_t input_samples[1024 * 2] = {0};
  fwrite(input_samples, sizeof(int16_t), 1024 * 2, f);
  fclose(f);

  // Set low samplerate (1000Hz) and chunk size (512), with realtime playback.
  // Each chunk takes 512 / 1000 = 512ms to play. Total playback time = 1024ms.
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 1000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2,\n"
           "            \"realtime\": true\n" // Playback throttled to real-time!
           "        }\n"
           "    }\n"
           "}",
           in_file, out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  // 1. Sleep for 200ms.
  // Capture and Processing threads should have finished reading/processing the 2 chunks
  // and exited. Playback is still playing chunk 1 (takes 512ms).
  cdsp_sleep_ms(200);

  // State must still be RUNNING (not inactive!) because queue is draining.
  cdsp_engine_poll(engine);
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_RUNNING);

  // 2. Sleep for another 1000ms (total 1200ms, which is > 1024ms total playback time).
  // Playback has finished playing both chunks and exited.
  cdsp_sleep_ms(1000);

  // State must transition to INACTIVE after draining.
  cdsp_engine_poll(engine);
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_INACTIVE);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
  remove(in_file);
  remove(out_file);
}

// Real-world scenario simulated:
// Immediate startup failure abort (Section 3.6).
// Simulates configure failure (e.g. invalid capture file path).
// Verifies that initialization fails cleanly and immediately transitions the engine state to INACTIVE.
TEST(DSPEngineE2E_StartupFailure_Abort) {
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 16000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"/nonexistent_directory/nonexistent_file.raw\",\n" // Invalid file path!
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"/tmp/startup_fail_out.raw\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 2\n"
           "        }\n"
           "    }\n"
           "}");

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  
  // The configuration is syntactically valid and threads spawn successfully
  ASSERT_TRUE(success);

  // Wait for the capture thread to start and fail to open the nonexistent file backend
  cdsp_sleep_ms(100);

  // Engine state must transition to INACTIVE
  cdsp_engine_poll(engine);
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_INACTIVE);

  // Stop reason must be set to capture error
  cdsp_stop_reason_t stop_reason;
  cdsp_get_stop_reason(engine, &stop_reason);
  ASSERT_EQ(stop_reason.type, CDSP_STOP_REASON_CAPTURE_ERROR);

  if (engine && engine->free) engine->free(engine->ctx);
}

// Real-world scenario simulated:
// Real-time hardware capture under queue drop pressure (Section 3.2 & Section 1.7.2 Rule 5).
// When the captured SPSC queue reaches 100% capacity, incoming real-time audio chunks are dropped.
// Verifies that un-enqueued chunks are retained in loop->pending_chunk so round-robin pool index
// does not advance or wrap around into active in-flight queued buffers, avoiding audio corruption.
TEST(DSPEngineE2E_RealtimeQueueDrop_DataIntegrity) {
  // Shared state with queue depth 2
  engine_shared_state_t* shared = engine_shared_state_create(2, 2);
  ASSERT_TRUE(shared != NULL);

  // Pool with capacity 4 (chunk0, chunk1, chunk2, chunk3)
  round_robin_chunk_pool_t* pool = round_robin_chunk_pool_create(4, 64, 1);
  ASSERT_TRUE(pool != NULL);

  capture_device_config_t cap_cfg;
  memset(&cap_cfg, 0, sizeof(cap_cfg));
  cap_cfg.type = AUDIO_BACKEND_TYPE_GENERATOR;
  cap_cfg.cfg.generator.channels = 1;
  cap_cfg.cfg.generator.signal.type = SIGNAL_TYPE_SINE;
  cap_cfg.cfg.generator.signal.frequency = 1000.0;
  cap_cfg.cfg.generator.signal.level = 0.0;

  backend_error_t berr;
  backend_error_init(&berr, BACKEND_ERROR_NONE, "");
  capture_backend_t* cap_backend = generator_capture_create(&cap_cfg, 48000, 64, NULL, &berr);
  ASSERT_TRUE(cap_backend != NULL);

  engine_capture_loop_config_t loop_cfg = {
      .shared = shared,
      .capture = cap_backend,
      .playback = NULL,
      .processing_params = NULL,
      .dop_decoder = NULL,
      .chunk_pool = pool,
      .chunk_size = 64,
      .channels = 1,
      .samplerate = 48000,
      .silence_threshold_db = -100.0,
      .silence_timeout_seconds = 0.0,
      .stop_on_rate_change = false,
      .rate_measure_interval_s = 0.0,
  };

  engine_capture_loop_t* loop = engine_capture_loop_create(&loop_cfg);
  ASSERT_TRUE(loop != NULL);

  // Fill captured_queue (depth 2) with chunk0 and chunk1
  audio_chunk_t* chunk0 = round_robin_chunk_pool_next(pool);
  audio_chunk_t* chunk1 = round_robin_chunk_pool_next(pool);
  ASSERT_TRUE(engine_shared_state_enqueue_captured(shared, chunk0));
  ASSERT_TRUE(engine_shared_state_enqueue_captured(shared, chunk1));

  // Now captured_queue is FULL.
  // Run 10 drop iterations. In real-time mode, enqueue fails for each chunk.
  // Now captured_queue is FULL.
  // Run 10 capture steps on full queue. In real-time mode, enqueue fails (drops).
  for (int i = 0; i < 10; i++) {
    engine_capture_loop_step(loop);
  }

  // Dequeue chunk0 sitting in captured_queue
  audio_chunk_t* dequeued0 = engine_shared_state_dequeue_captured_blocking(shared);
  ASSERT_EQ(chunk0, dequeued0);

  // Verify that chunk0 is NOT returned as the next available pool chunk after drop iterations!
  // Without pending_chunk retention, pool wrap-around causes next_pool_chunk == chunk0 (overwriting active queue buffer).
  audio_chunk_t* next_pool_chunk = round_robin_chunk_pool_next(pool);
  ASSERT_NE(chunk0, next_pool_chunk);

  engine_capture_loop_free(loop);
  round_robin_chunk_pool_free(pool);
  engine_shared_state_free(shared);
}

static void* proc_thread_worker(void* arg) {
  engine_processing_loop_t* loop = (engine_processing_loop_t*)arg;
  engine_processing_loop_run(loop);
  return NULL;
}

// Real-world scenario simulated:
// Non-realtime session immediate abort during backend error or user stop (Section 3.6).
// When a non-realtime worker thread (e.g. file conversion) is waiting on a full queue while an
// immediate abort occurs, verifies that worker threads break out of the outer processing loop
// immediately rather than draining all remaining buffered chunks in captured_queue.
TEST(DSPEngineE2E_NonRealtimeImmediateAbort_ExitsImmediately) {
  // Shared state with queue depth 4
  engine_shared_state_t* shared = engine_shared_state_create(4, 4);
  ASSERT_TRUE(shared != NULL);

  round_robin_chunk_pool_t* pool = round_robin_chunk_pool_create(8, 64, 1);
  ASSERT_TRUE(pool != NULL);

  processing_parameters_t* params = processing_parameters_create(1, 1);
  ASSERT_TRUE(params != NULL);

  dsp_config_t dcfg;
  memset(&dcfg, 0, sizeof(dcfg));
  dcfg.devices.samplerate = 48000;
  dcfg.devices.chunksize = 64;
  capture_device_config_set_channels(&dcfg.devices.capture, 1);
  dcfg.devices.playback.type = AUDIO_BACKEND_TYPE_FILE;
  dcfg.devices.playback.cfg.raw_file.channels = 1;

  pipeline_t* pipe = pipeline_create(&dcfg, params, 64, NULL);
  ASSERT_TRUE(pipe != NULL);

  engine_processing_loop_config_t loop_cfg = {
      .shared = shared,
      .processing_params = params,
      .pipeline_rate = 48000,
      .resampler = NULL,
      .pipeline = pipe,
      .dsd_encoder = NULL,
      .resampler_scratch = NULL,
      .pipeline_scratch = NULL,
      .scratch_pool = pool,
      .on_chunk_captured = NULL,
      .on_chunk_captured_ctx = NULL,
      .on_chunk_processed = NULL,
      .on_chunk_processed_ctx = NULL,
      .is_realtime = false,
  };

  engine_processing_loop_t* loop = engine_processing_loop_create(&loop_cfg);
  ASSERT_TRUE(loop != NULL);

  // Fill processed_queue to capacity (4 chunks) so processing thread blocks when trying to enqueue
  for (int i = 0; i < 4; i++) {
    audio_chunk_t* pchunk = round_robin_chunk_pool_next(pool);
    ASSERT_TRUE(engine_shared_state_enqueue_processed(shared, pchunk));
  }

  // Fill captured_queue with 4 chunks to process
  for (int i = 0; i < 4; i++) {
    audio_chunk_t* cchunk = round_robin_chunk_pool_next(pool);
    ASSERT_TRUE(engine_shared_state_enqueue_captured(shared, cchunk));
  }

  pthread_t thread;
  pthread_create(&thread, NULL, proc_thread_worker, loop);

  // Allow processing thread to dequeue 1 captured chunk and enter while (!enqueue_processed) sleep loop
  cdsp_sleep_ms(10);

  // Request immediate abort (STOP_REASON_PLAYBACK_ERROR)
  processing_stop_reason_t stop_reason = {.type = STOP_REASON_PLAYBACK_ERROR};
  snprintf(stop_reason.message, sizeof(stop_reason.message), "Mock error abort");
  engine_shared_state_request_stop(shared, stop_reason);

  pthread_join(thread, NULL);

  // Assert that processing thread exited immediately without draining the remaining captured chunks!
  size_t remaining_captured = spsc_queue_get_count(engine_shared_state_get_captured_queue(shared));
  ASSERT_TRUE(remaining_captured > 0);

  engine_processing_loop_free(loop);
  processing_parameters_free(params);
  round_robin_chunk_pool_free(pool);
  engine_shared_state_free(shared);
}

// Real-world scenario simulated:
// Silence Auto-Pause & Resume bug on non-hardware inputs (Section 3.3).
// Proves that when auto-pause triggers on a File capture stream, file_capture_read()
// checks is_paused == true and immediately returns false without reading frames.
// As a result, when loud non-silent audio arrives in the input file stream, the engine
// remains permanently stuck in PAUSED and signal auto-resume fails.
TEST(DSPEngineE2E_SilenceAutoPause_FileBackend_AutoResumeBug) {
  char in_file[256];
  char out_file[256];
  snprintf(in_file, sizeof(in_file), "/tmp/silence_bug_in_%d.raw", getpid());
  snprintf(out_file, sizeof(out_file), "/tmp/silence_bug_out_%d.raw", getpid());
  remove(in_file);
  remove(out_file);

  // Write 10000 frames of mono 16-bit audio:
  // Part 1 (frames 0..4999): Zero silence samples
  // Part 2 (frames 5000..9999): Maximum amplitude loud samples (32767)
  FILE* f = fopen(in_file, "wb");
  ASSERT_TRUE(f != NULL);
  int16_t samples[10000] = {0};
  for (int i = 5000; i < 10000; i++) {
    samples[i] = 32767;
  }
  fwrite(samples, sizeof(int16_t), 10000, f);
  fclose(f);

  // Samplerate: 8000Hz, chunksize: 512, realtime capture: true, realtime playback: false.
  // 5000 silent frames / 8000 = 0.625 seconds of silence.
  // silence_timeout_s = 0.2s (triggers auto-pause after ~3 chunks of silence).
  char json[1024];
  snprintf(json, sizeof(json),
           "{\n"
           "    \"devices\": {\n"
           "        \"samplerate\": 8000,\n"
           "        \"chunksize\": 512,\n"
           "        \"queuelimit\": 16,\n"
           "        \"silence_threshold_db\": -40.0,\n"
           "        \"silence_timeout_s\": 0.2,\n"
           "        \"capture\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1,\n"
           "            \"realtime\": true\n"
           "        },\n"
           "        \"playback\": {\n"
           "            \"type\": \"File\",\n"
           "            \"filename\": \"%s\",\n"
           "            \"format\": \"S16_LE\",\n"
           "            \"channels\": 1\n"
           "        }\n"
           "    }\n"
           "}",
           in_file, out_file);

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = engine->set_config_json(engine->ctx, json, &err);
  ASSERT_TRUE(success);

  // 1. Wait 350ms for silence timeout to trigger PAUSED state
  cdsp_sleep_ms(350);
  cdsp_engine_poll(engine);
  ASSERT_EQ(cdsp_get_state(engine), CDSP_PROCESSING_STATE_PAUSED);

  // 2. Wait another 500ms while the input file reaches the loud non-silent audio frames (frames 5000+)
  cdsp_sleep_ms(500);
  cdsp_engine_poll(engine);

  // EXPECTED DESIGN BEHAVIOR: Loud audio should resume streaming -> RUNNING
  // ACTUAL BUG BEHAVIOR: File capture backend returns false on read when is_paused == true,
  // preventing frame reading, so the engine stays stuck in PAUSED!
  cdsp_processing_state_t state_after_loud_signal = cdsp_get_state(engine);

  cdsp_stop(engine);
  if (engine && engine->free) engine->free(engine->ctx);
  remove(in_file);
  remove(out_file);

  // Assert that state auto-resumes to RUNNING when loud audio arrives
  ASSERT_EQ(state_after_loud_signal, CDSP_PROCESSING_STATE_RUNNING);
}

// Real-world scenario simulated:
// Immediate Abort Teardown queue draining bug in playback loop (Section 3.6).
// Proves that when an immediate error abort (STOP_REASON_PLAYBACK_ERROR) occurs,
// engine_playback_loop_run() currently lacks a should_stop() check inside its
// while (dequeue_processed_blocking) loop, causing it to drain and render all queued chunks
// to DAC/file instead of aborting immediately.
TEST(DSPEngineE2E_ImmediateAbort_PlaybackDrainingBug) {
  engine_shared_state_t* shared = engine_shared_state_create(8, 8);
  ASSERT_TRUE(shared != NULL);

  round_robin_chunk_pool_t* pool = round_robin_chunk_pool_create(8, 64, 1);
  ASSERT_TRUE(pool != NULL);

  processing_parameters_t* params = processing_parameters_create(1, 1);
  ASSERT_TRUE(params != NULL);

  char out_file[256];
  snprintf(out_file, sizeof(out_file), "/tmp/abort_drain_out_%d.raw", getpid());
  remove(out_file);

  playback_device_config_t play_cfg;
  memset(&play_cfg, 0, sizeof(play_cfg));
  play_cfg.type = AUDIO_BACKEND_TYPE_FILE;
  play_cfg.cfg.raw_file.channels = 1;
  play_cfg.cfg.raw_file.format = BINARY_SAMPLE_FORMAT_S16_LE;
  snprintf(play_cfg.cfg.raw_file.filename, sizeof(play_cfg.cfg.raw_file.filename), "%s", out_file);

  backend_error_t berr;
  playback_backend_t* pb = file_playback_create(&play_cfg, 48000, 64, NULL, &berr);
  ASSERT_TRUE(pb != NULL);

  engine_playback_loop_config_t loop_cfg = {
      .shared = shared,
      .capture = NULL,
      .playback = pb,
      .processing_params = params,
      .dsd_encoder = NULL,
      .chunk_size = 64,
      .pipeline_rate = 48000,
      .target_level = 0,
      .rate_adjust_enabled = false,
      .adjust_period = 0.0,
  };

  engine_playback_loop_t* loop = engine_playback_loop_create(&loop_cfg);
  ASSERT_TRUE(loop != NULL);

  // Push 4 chunks into processed_queue
  for (int i = 0; i < 4; i++) {
    audio_chunk_t* chunk = round_robin_chunk_pool_next(pool);
    audio_chunk_set_valid_frames(chunk, 64);
    ASSERT_TRUE(engine_shared_state_enqueue_processed(shared, chunk));
  }

  // Request immediate abort (STOP_REASON_PLAYBACK_ERROR)
  processing_stop_reason_t stop_reason = {.type = STOP_REASON_PLAYBACK_ERROR};
  snprintf(stop_reason.message, sizeof(stop_reason.message), "Hardware DAC failure");
  engine_shared_state_request_stop(shared, stop_reason);

  // Run playback loop
  engine_playback_loop_run(loop);

  // According to Section 3.6 (Immediate Abort Teardown), the playback loop MUST NOT drain
  // queued chunks when should_stop() is true during an error abort.
  size_t remaining_processed = spsc_queue_get_count(engine_shared_state_get_processed_queue(shared));

  engine_playback_loop_free(loop);
  processing_parameters_free(params);
  round_robin_chunk_pool_free(pool);
  engine_shared_state_free(shared);
  remove(out_file);

  // Assert that remaining chunks were NOT drained and written to DAC during immediate abort
  ASSERT_TRUE(remaining_processed > 0);
}

// Real-world scenario simulated:
// Concurrent graceful EOF completion and hardware DAC/playback failure during teardown (Section 4).
// Proves that when Thread A (capture EOF) wins CAS on stop_once but has not yet finished publishing
// STOP_REASON_DONE under stop_reason_mutex, a concurrent request_stop(STOP_REASON_PLAYBACK_ERROR)
// from Thread B (hardware DAC failure) enters the LOSER branch and reads STOP_REASON_NONE.
// Current code assumes STOP_REASON_NONE != STOP_REASON_DONE, skipping error publication and state INACTIVE
// transition, silently dropping the hardware error and leaving session worker threads hanging.
TEST(DSPEngine_Repro_CAS_Publication_Window_Race) {
  engine_shared_state_t* shared = engine_shared_state_create(16, 16);
  ASSERT_TRUE(shared != NULL);

  // Step 1: Simulate Thread A winning CAS with unpublished / non-error stop (STOP_REASON_NONE)
  engine_shared_state_request_stop(
      shared, (processing_stop_reason_t){.type = STOP_REASON_NONE});

  // Step 2: Thread B requests a critical hardware error stop while stop_once is true
  processing_stop_reason_t err_reason = {.type = STOP_REASON_PLAYBACK_ERROR};
  snprintf(err_reason.message, sizeof(err_reason.message), "Hardware DAC failure");
  engine_shared_state_request_stop(shared, err_reason);

  // Expected behavior: Critical hardware error MUST be published and state MUST be INACTIVE
  ASSERT_EQ(STOP_REASON_PLAYBACK_ERROR,
            engine_shared_state_get_stop_reason(shared).type);
  ASSERT_TRUE(engine_shared_state_should_stop(shared));
  ASSERT_EQ(PROCESSING_STATE_INACTIVE, engine_shared_state_get_state(shared));

  engine_shared_state_free(shared);
}

// Real-world scenario simulated:
// Main thread application cleanup following successful EOF audio rendering completion (Section 4 & Section 3.5).
// Proves that when a graceful EOF teardown finishes (stop_reason set to STOP_REASON_DONE), a subsequent
// dsp_session_stop_and_free() or dsp_engine_stop() call on the main thread passing default STOP_REASON_NONE
// triggers current condition (reason.type != STOP_REASON_DONE), overwriting STOP_REASON_DONE with
// STOP_REASON_NONE and destroying completion diagnostic records.
TEST(DSPEngine_Repro_StopReason_None_Overwriting_Done) {
  engine_shared_state_t* shared = engine_shared_state_create(16, 16);
  ASSERT_TRUE(shared != NULL);

  // Step 1: Graceful EOF teardown finishes, setting stop_reason to STOP_REASON_DONE
  processing_stop_reason_t eof_reason = {.type = STOP_REASON_DONE};
  engine_shared_state_request_stop(shared, eof_reason);

  ASSERT_EQ(STOP_REASON_DONE, engine_shared_state_get_stop_reason(shared).type);

  // Step 2: Main thread calls cleanup with default STOP_REASON_NONE
  processing_stop_reason_t default_reason = {.type = STOP_REASON_NONE};
  engine_shared_state_request_stop(shared, default_reason);

  // Expected behavior: STOP_REASON_DONE MUST NOT be overwritten by STOP_REASON_NONE
  ASSERT_EQ(STOP_REASON_DONE, engine_shared_state_get_stop_reason(shared).type);

  engine_shared_state_free(shared);
}

static void* capture_backpressure_worker_func(void* arg) {
  engine_capture_loop_t* loop = (engine_capture_loop_t*)arg;
  engine_capture_loop_step(loop);
  return NULL;
}

// Real-world scenario simulated:
// Non-realtime audio processing (e.g. offline File rendering or Signal Generator capture) experiencing downstream DSP or Playback backpressure (Section 3.4).
// Proves that when non-realtime capture fills captured_queue, the capture thread blocks in a 1ms retry sleep loop waiting for space.
// Current code only updates last_capture_time_ns at chunk entry and omits refreshes inside the retry loop,
// causing last_capture_time_ns to become stale (>0.5s) during backpressure and falsely triggering a Watchdog Stall
// (PROCESSING_STATE_STALLED) on the main controller thread despite the capture thread being completely healthy.
TEST(DSPEngine_Repro_FalsePositiveWatchdogStall_NonRealtimeBackpressure) {

  processing_parameters_t* params = processing_parameters_create(2, 2);
  ASSERT_TRUE(params != NULL);

  capture_device_config_t dev_cfg = {
      .type = AUDIO_BACKEND_TYPE_GENERATOR,
      .cfg.generator =
          {
              .channels = 2,
              .signal =
                  {
                      .type = SIGNAL_TYPE_SINE,
                      .frequency = 440.0,
                      .level = 0.5,
                  },
          },
  };
  backend_error_t berr = {0};
  capture_backend_t* cap_backend =
      generator_capture_create(&dev_cfg, 48000, 64, params, &berr);
  ASSERT_TRUE(cap_backend != NULL);
  ASSERT_TRUE(capture_backend_open(cap_backend, &berr));

  engine_shared_state_t* shared = engine_shared_state_create(4, 4);
  round_robin_chunk_pool_t* pool = round_robin_chunk_pool_create(8, 64, 2);

  engine_capture_loop_config_t config = {
      .capture = cap_backend,
      .shared = shared,
      .processing_params = params,
      .chunk_pool = pool,
      .dop_decoder = NULL,
      .samplerate = 48000,
      .channels = 2,
      .chunk_size = 64,
  };
  engine_capture_loop_t* capture_loop = engine_capture_loop_create(&config);
  ASSERT_TRUE(capture_loop != NULL);

  engine_shared_state_set_state(shared, PROCESSING_STATE_RUNNING);

  // Fill captured_queue completely so next enqueue inside loop_step will block in retry loop
  for (int i = 0; i < 4; i++) {
    audio_chunk_t* c = round_robin_chunk_pool_next(pool);
    audio_chunk_set_valid_frames(c, 64);
    ASSERT_TRUE(engine_shared_state_enqueue_captured(shared, c));
  }

  // Start capture worker thread which calls engine_capture_loop_step() and blocks
  pthread_t thread;
  pthread_create(&thread, NULL, capture_backpressure_worker_func, capture_loop);

  // Sleep for 600ms while capture thread is waiting on backpressure
  cdsp_sleep_ms(600);

  uint64_t last_time = engine_shared_state_get_last_capture_time(shared);
  uint64_t now = cdsp_time_now_ns();
  double elapsed_sec = (double)(now - last_time) / 1000000000.0;

  // Cleanup worker thread
  engine_shared_state_request_stop(
      shared, (processing_stop_reason_t){.type = STOP_REASON_DONE});
  pthread_join(thread, NULL);

  engine_capture_loop_free(capture_loop);
  capture_backend_close(cap_backend);
  capture_backend_free(cap_backend);
  processing_parameters_free(params);
  round_robin_chunk_pool_free(pool);
  engine_shared_state_free(shared);

  // Assert that last_capture_time was refreshed during backpressure sleep loop
  ASSERT_TRUE(elapsed_sec < 0.3);
}

TEST_MAIN()



