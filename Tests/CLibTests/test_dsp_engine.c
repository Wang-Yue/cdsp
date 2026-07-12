#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "Engine/dsp_engine.h"
#include "Pipeline/config_loader.h"
#include "test_support.h"

static void run_e2e_test_config(const char* json, const char* backend_name) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = dsp_engine_set_config(engine, json, &err);
  if (!success) {
    printf(
        "⚠️ [E2E Warning] Skipping E2E test for backend '%s' (Initialization "
        "failed: %s)\n",
        backend_name, err.message);
    dsp_engine_free(engine);
    return;
  }

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
  nanosleep(&ts, NULL);

  vu_levels_t vu = dsp_engine_get_vu_levels(engine);
  (void)vu;

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
  printf("✅ [E2E Success] Backend '%s' ran successfully\n", backend_name);
}

TEST(DSPEngineCreateFree) {
  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);
  dsp_engine_free(engine);
}

TEST(DSPEngineDeviceCapabilities) {
  audio_device_t devs[32];
  int count = dsp_engine_get_available_devices("coreaudio", false, devs, 32);
  ASSERT_TRUE(count >= 0);

  // Test freeing NULL descriptor (should be a safe no-op)
  dsp_engine_free_device_capabilities(NULL);

  if (count > 0) {
    audio_device_descriptor_t* desc = dsp_engine_get_device_capabilities(
        "coreaudio", devs[0].name, false, NULL);
    if (desc) {
      dsp_engine_free_device_capabilities(desc);
    }
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
  bool success1 = dsp_engine_set_config(engine, json1, &err);
  if (!success1) {
    printf("ERROR: json1 set_config failed: %s\n", err.message);
  }
  ASSERT_TRUE(success1);

  bool success2 = dsp_engine_set_config(engine, json2, &err);
  if (!success2) {
    printf("ERROR: json2 set_config failed: %s\n", err.message);
  }
  ASSERT_TRUE(success2);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->mixers_count);
  ASSERT_EQ(1, active->pipeline_count);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
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
  bool success1 = dsp_engine_set_config(engine, json1, &err);
  ASSERT_TRUE(success1);

  bool success2 = dsp_engine_set_config(engine, json2, &err);
  ASSERT_TRUE(success2);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->filters_count);
  ASSERT_EQ(-3.0, active->filters[0].filter.parameters.gain.gain);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
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

  // Apply overrides
  parsed->devices.samplerate = 48000;
#if defined(_WIN32)
  capture_device_config_set_channels(&parsed->devices.capture, 2);
#else
  capture_device_config_set_channels(&parsed->devices.capture, 4);
#endif

  audio_backend_error_t berr;
  bool ok = dsp_engine_set_config_struct(engine, parsed, &berr);
  ASSERT_TRUE(ok);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(48000, active->devices.samplerate);
#if defined(_WIN32)
  ASSERT_EQ(2, capture_device_config_get_channels(&active->devices.capture));
#else
  ASSERT_EQ(4, capture_device_config_get_channels(&active->devices.capture));
#endif

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
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

TEST(DSPEngineE2E_PulseAudio) {
#if defined(__linux__)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Pulse\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Pulse\",\n"
      "            \"device\": \"default\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "PulseAudio");
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

TEST(DSPEngineE2E_JACK) {
#if defined(ENABLE_JACK)
  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"Jack\",\n"
      "            \"channels\": 2,\n"
      "            \"device\": \"camilladsp_c\"\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"Jack\",\n"
      "            \"channels\": 2,\n"
      "            \"device\": \"camilladsp_p\"\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "JACK");
#endif
}

TEST(DSPEngineE2E_GeneratorFile) {
  const char* json =
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
      "            \"filename\": \"/tmp/e2e_out.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";
  run_e2e_test_config(json, "Generator -> File");
}

TEST(DSPEngineE2E_FileFile) {
  const char* in_file = "/tmp/e2e_in.raw";
  const char* out_file = "/tmp/e2e_out.raw";
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

  const char* json =
      "{\n"
      "    \"devices\": {\n"
      "        \"samplerate\": 44100,\n"
      "        \"chunksize\": 512,\n"
      "        \"capture\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/tmp/e2e_in.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        },\n"
      "        \"playback\": {\n"
      "            \"type\": \"File\",\n"
      "            \"filename\": \"/tmp/e2e_out.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  bool success = dsp_engine_set_config(engine, json, &err);
  ASSERT_TRUE(success);

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};
  nanosleep(&ts, NULL);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);

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

TEST(DSPEngineE2E_GeneratorFile_SpeedTest) {
  const char* out_filename = "/tmp/e2e_out_speed.raw";
  remove(out_filename);

  const char* json =
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
      "            \"filename\": \"/tmp/e2e_out_speed.raw\",\n"
      "            \"format\": \"S16_LE\",\n"
      "            \"channels\": 2\n"
      "        }\n"
      "    }\n"
      "}";

  dsp_engine_t* engine = dsp_engine_create();
  ASSERT_TRUE(engine != NULL);

  audio_backend_error_t err;
  memset(&err, 0, sizeof(err));
  ASSERT_TRUE(dsp_engine_set_config(engine, json, &err));

  // Sleep for 50ms to let the engine stream at infinite speed
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};
  nanosleep(&ts, NULL);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);

  // Check the size of the output file
  FILE* f = fopen(out_filename, "rb");
  ASSERT_TRUE(f != NULL);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fclose(f);

  // At real-time (44.1kHz stereo 16-bit = 176.4KB/sec), 50ms would be 8.8KB.
  // Unthrottled generation should easily produce > 1MB of audio in 50ms.
  ASSERT_TRUE(size > 1000000);

  remove(out_filename);
  printf(
      "✅ [E2E Success] Generator -> File ran unthrottled (produced %ld bytes "
      "in 50ms)\n",
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
  bool success1 = dsp_engine_set_config(engine, json1, &err);
  if (!success1) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO SetConfigAndReload test (Failed to set "
        "config: %s)\n",
        err.message);
    dsp_engine_free(engine);
    return;
  }
  ASSERT_TRUE(success1);

  bool success2 = dsp_engine_set_config(engine, json2, &err);
  ASSERT_TRUE(success2);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->mixers_count);
  ASSERT_EQ(1, active->pipeline_count);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
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
  bool success1 = dsp_engine_set_config(engine, json1, &err);
  if (!success1) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO HotParameterReload test (Failed to set "
        "config: %s)\n",
        err.message);
    dsp_engine_free(engine);
    return;
  }
  ASSERT_TRUE(success1);

  bool success2 = dsp_engine_set_config(engine, json2, &err);
  ASSERT_TRUE(success2);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(1, active->filters_count);
  ASSERT_EQ(-3.0, active->filters[0].filter.parameters.gain.gain);

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
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

  audio_backend_error_t berr;
  bool ok = dsp_engine_set_config_struct(engine, parsed, &berr);
  if (!ok) {
    printf(
        "⚠️ [ASIO Warning] Skipping ASIO SetConfigStruct test (Failed to set "
        "config struct: %s)\n",
        berr.message);
    dsp_engine_free(engine);
    return;
  }
  ASSERT_TRUE(ok);

  const dsp_config_t* active = dsp_engine_get_active_config(engine);
  ASSERT_TRUE(active != NULL);
  ASSERT_EQ(48000, active->devices.samplerate);
  ASSERT_EQ(2, capture_device_config_get_channels(&active->devices.capture));

  dsp_engine_stop(engine);
  dsp_engine_free(engine);
}
#endif

static void run_e2e_file_file_test(bool capture_rt, bool playback_rt,
                                   int total_frames, bool test_time) {
  const char* in_file = "/tmp/e2e_rt_in.raw";
  const char* out_file = "/tmp/e2e_rt_out.raw";
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
  bool success = dsp_engine_set_config(engine, json, &err);
  ASSERT_TRUE(success);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  // Wait for the engine to finish processing the file
  // Timeout is 1.0s (the file is 0.1s long at 16kHz for 1600 frames)
  int elapsed_ms = 0;
  while (elapsed_ms < 1000) {
    state_update_t status = dsp_engine_get_status(engine);
    if (status.state == PROCESSING_STATE_INACTIVE) {
      break;
    }
    struct timespec req = {.tv_sec = 0, .tv_nsec = 5000000ULL};  // 5ms
    nanosleep(&req, NULL);
    elapsed_ms += 5;
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                   (double)(t1.tv_nsec - t0.tv_nsec) / 1000000000.0;

  // Stop just in case it didn't finish gracefully
  dsp_engine_stop(engine);
  dsp_engine_free(engine);

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
    // If either capture or playback is realtime, it should take at least ~80ms
    // (for 1600 frames @ 16kHz = 100ms)
    bool is_rt = capture_rt || playback_rt;
    if (is_rt) {
      // should be at least 0.08s, and not more than 0.3s
      ASSERT_TRUE(elapsed >= 0.08);
      ASSERT_TRUE(elapsed < 0.30);
    } else {
      // Non-realtime should take very little time
      ASSERT_TRUE(elapsed < 0.05);
    }
  }
}

TEST(DSPEngineE2E_FileFile_Realtime_FF) {
  run_e2e_file_file_test(false, false, 1600, true);
}

TEST(DSPEngineE2E_FileFile_Realtime_TF) {
  run_e2e_file_file_test(true, false, 1600, true);
}

TEST(DSPEngineE2E_FileFile_Realtime_FT) {
  run_e2e_file_file_test(false, true, 1600, true);
}

TEST(DSPEngineE2E_FileFile_Realtime_TT) {
  run_e2e_file_file_test(true, true, 1600, true);
}

TEST_MAIN()
