#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if defined(ENABLE_ASIO) || defined(ENABLE_WASAPI)
#include <objbase.h>
#endif

#include "Logging/app_logger.h"
#include "Public/cdsp_pub_types.h"
#include "Public/config.h"
#include "Public/general.h"
#include "Public/processing.h"
#include "Public/state.h"
#include "Public/volume.h"
#include "Server/websocket_server.h"
#include "Utils/cdsp_time.h"
#include "Utils/double_helpers.h"

static const logger_t g_logger = {"dsp.main"};

static volatile sig_atomic_t keep_running = 1;

/**
 * @brief Signal handler to catch termination signals (SIGINT, SIGTERM).
 *
 * Sets the global keep_running flag to 0, which initiates a graceful shutdown
 * of the main processing loop.
 *
 * @param sig The signal number.
 */
static void sig_handler(int sig) {
  (void)sig;
  keep_running = 0;
}

/**
 * @brief Prints the command-line usage information of the application to
 * standard output.
 */
static void print_usage(void) {
  printf(
      "Usage: dsp-cli [CONFIGFILE] [OPTIONS]\n"
      "  CONFIGFILE        Path to JSON/YAML configuration file.\n\n"
      "Options:\n"
      "  -h, --help        Print this help message.\n"
      "  -c, --check       Check config file and exit.\n"
      "  -s, --statefile   Use the given file to persist volume/mute state.\n"
      "  -w, --wait        Wait for config from websocket (starts inactive).\n"
      "  --no_config       Ignore config file in statefile and start without.\n"
      "  -p, --port        Port for the WebSocket control server.\n"
      "  -a, --address     IP address to bind WebSocket server to (defaults to "
      "127.0.0.1).\n"
      "  -l, --loglevel    Log level (trace, debug, info, warn, error). "
      "Defaults to info.\n"
      "  -o, --logfile     Write logs to the given file path.\n"
      "  -g, --gain        Initial gain in dB for main volume control.\n"
      "  --gain1           Initial gain in dB for Aux1 fader.\n"
      "  --gain2           Initial gain in dB for Aux2 fader.\n"
      "  --gain3           Initial gain in dB for Aux3 fader.\n"
      "  --gain4           Initial gain in dB for Aux4 fader.\n"
      "  -m, --mute        Start with main volume control muted.\n"
      "  --mute1           Start with Aux1 fader muted.\n"
      "  --mute2           Start with Aux2 fader muted.\n"
      "  --mute3           Start with Aux3 fader muted.\n"
      "  --mute4           Start with Aux4 fader muted.\n"
      "  -r, --samplerate  Override samplerate in config.\n"
      "  -n, --channels    Override number of channels of capture device in "
      "config.\n"
      "  -f, --format      Override sample format of capture device in "
      "config.\n"
      "  -e, --extra_samples Override number of extra samples in config.\n\n"
      "Supported device types:\n"
      "  Capture: "
#if defined(ENABLE_COREAUDIO)
      "CoreAudio, "
#endif
#if defined(ENABLE_ALSA)
      "ALSA, "
#endif
#if defined(ENABLE_PIPEWIRE)
      "PipeWire, "
#endif
#if defined(ENABLE_WASAPI)
      "WASAPI, "
#endif
#if defined(ENABLE_ASIO)
      "ASIO, "
#endif
      "File, Stdin, Generator\n"
      "  Playback: "
#if defined(ENABLE_COREAUDIO)
      "CoreAudio, "
#endif
#if defined(ENABLE_ALSA)
      "ALSA, "
#endif
#if defined(ENABLE_PIPEWIRE)
      "PipeWire, "
#endif
#if defined(ENABLE_WASAPI)
      "WASAPI, "
#endif
#if defined(ENABLE_ASIO)
      "ASIO, "
#endif
      "File, Stdout\n");
}

int main(int argc, char** argv) {
#if defined(ENABLE_ASIO) || defined(ENABLE_WASAPI)
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
#endif
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  const char* config_path = NULL;
  const char* state_file_path = NULL;
  bool check_only = false;
  uint16_t port = 0;
  bool has_port = false;
  const char* bind_address = "127.0.0.1";
  bool wait_config = false;
  bool no_config = false;
  const char* log_level_str = "info";

  double initial_gains[CDSP_FADER_COUNT];
  bool has_initial_gains[CDSP_FADER_COUNT];
  bool initial_mutes[CDSP_FADER_COUNT];
  bool has_initial_mutes[CDSP_FADER_COUNT];
  for (int i = 0; i < CDSP_FADER_COUNT; i++) {
    initial_gains[i] = 0.0;
    has_initial_gains[i] = false;
    initial_mutes[i] = false;
    has_initial_mutes[i] = false;
  }

  int samplerate_override = -1;
  int channels_override = -1;
  const char* format_override = NULL;
  int extra_samples_override = -1;

  for (int i = 1; i < argc; i++) {
    const char* arg = argv[i];
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      print_usage();
      return 0;
    } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0) {
      check_only = true;
    } else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--wait") == 0) {
      wait_config = true;
    } else if (strcmp(arg, "--no_config") == 0) {
      no_config = true;
    } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--statefile") == 0) {
      if (i + 1 < argc) {
        state_file_path = argv[++i];
      } else {
        printf("Error: Missing value for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
      if (i + 1 < argc) {
        port = (uint16_t)atoi(argv[++i]);
        has_port = true;
      } else {
        printf("Error: Invalid port for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-a") == 0 || strcmp(arg, "--address") == 0) {
      if (i + 1 < argc) {
        bind_address = argv[++i];
      } else {
        printf("Error: Missing value for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loglevel") == 0) {
      if (i + 1 < argc) {
        log_level_str = argv[++i];
      } else {
        printf("Error: Missing value for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--logfile") == 0) {
      if (i + 1 < argc) {
        printf(
            "Note: Native file logging is not supported. Please redirect "
            "stdout/stderr instead: > %s 2>&1\n",
            argv[++i]);
      } else {
        printf("Error: Missing value for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-g") == 0 || strcmp(arg, "--gain") == 0) {
      if (i + 1 < argc) {
#if CDSP_FADER_COUNT > 0
        initial_gains[0] = atof(argv[++i]);
        has_initial_gains[0] = true;
#else
        i++;
#endif
      } else {
        printf("Error: Invalid gain value\n");
        return 1;
      }
    } else if (strcmp(arg, "--gain1") == 0) {
      if (i + 1 < argc) {
#if CDSP_FADER_COUNT > 1
        initial_gains[1] = atof(argv[++i]);
        has_initial_gains[1] = true;
#else
        i++;
#endif
      } else {
        printf("Error: Invalid gain1 value\n");
        return 1;
      }
    } else if (strcmp(arg, "--gain2") == 0) {
      if (i + 1 < argc) {
#if CDSP_FADER_COUNT > 2
        initial_gains[2] = atof(argv[++i]);
        has_initial_gains[2] = true;
#else
        i++;
#endif
      } else {
        printf("Error: Invalid gain2 value\n");
        return 1;
      }
    } else if (strcmp(arg, "--gain3") == 0) {
      if (i + 1 < argc) {
#if CDSP_FADER_COUNT > 3
        initial_gains[3] = atof(argv[++i]);
        has_initial_gains[3] = true;
#else
        i++;
#endif
      } else {
        printf("Error: Invalid gain3 value\n");
        return 1;
      }
    } else if (strcmp(arg, "--gain4") == 0) {
      if (i + 1 < argc) {
#if CDSP_FADER_COUNT > 4
        initial_gains[4] = atof(argv[++i]);
        has_initial_gains[4] = true;
#else
        i++;
#endif
      } else {
        printf("Error: Invalid gain4 value\n");
        return 1;
      }
    } else if (strcmp(arg, "-m") == 0 || strcmp(arg, "--mute") == 0) {
#if CDSP_FADER_COUNT > 0
      initial_mutes[0] = true;
      has_initial_mutes[0] = true;
#endif
    } else if (strcmp(arg, "--mute1") == 0) {
#if CDSP_FADER_COUNT > 1
      initial_mutes[1] = true;
      has_initial_mutes[1] = true;
#endif
    } else if (strcmp(arg, "--mute2") == 0) {
#if CDSP_FADER_COUNT > 2
      initial_mutes[2] = true;
      has_initial_mutes[2] = true;
#endif
    } else if (strcmp(arg, "--mute3") == 0) {
#if CDSP_FADER_COUNT > 3
      initial_mutes[3] = true;
      has_initial_mutes[3] = true;
#endif
    } else if (strcmp(arg, "--mute4") == 0) {
#if CDSP_FADER_COUNT > 4
      initial_mutes[4] = true;
      has_initial_mutes[4] = true;
#endif
    } else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--samplerate") == 0) {
      if (i + 1 < argc) {
        samplerate_override = atoi(argv[++i]);
      } else {
        printf("Error: Invalid samplerate value\n");
        return 1;
      }
    } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--channels") == 0) {
      if (i + 1 < argc) {
        channels_override = atoi(argv[++i]);
      } else {
        printf("Error: Invalid channels value\n");
        return 1;
      }
    } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--format") == 0) {
      if (i + 1 < argc) {
        format_override = argv[++i];
      } else {
        printf("Error: Missing format value for %s\n", arg);
        return 1;
      }
    } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--extra_samples") == 0) {
      if (i + 1 < argc) {
        extra_samples_override = atoi(argv[++i]);
      } else {
        printf("Error: Missing extra_samples value for %s\n", arg);
        return 1;
      }
    } else {
      if (arg[0] != '-') {
        config_path = arg;
      } else {
        printf("Unknown option: %s\n", arg);
        print_usage();
        return 1;
      }
    }
  }

  cdsp_set_log_level(log_level_str);

  logger_info(&g_logger, "Starting dsp-cli application");

  if (check_only) {
    if (!config_path) {
      logger_error(&g_logger, "Missing config file for --check");
      printf("Error: Missing config file to check.\n");
      return 1;
    }
    char* result = NULL;
    bool is_error = false;
    if (cdsp_validate_config_file(config_path, &result, &is_error) &&
        !is_error) {
      logger_info(&g_logger, "Configuration check succeeded for %s",
                  config_path);
      printf("Configuration is valid.\n");
    } else {
      logger_error(&g_logger, "Configuration check failed: %s",
                   result ? result : "Invalid config");
      printf("Configuration check failed: %s\n",
             result ? result : "Invalid config");
      if (result) free(result);
      return 1;
    }
    if (result) free(result);
    return 0;
  }

  // Load state file if present
  char* allocated_config_path = NULL;
  cdsp_state_t* loaded_state = cdsp_state_create();
  bool has_loaded_state = false;
  if (state_file_path && loaded_state) {
    if (cdsp_state_load(state_file_path, loaded_state)) {
      has_loaded_state = true;
      logger_info(&g_logger, "Loaded state file from %s", state_file_path);
      if (!config_path && !no_config &&
          cdsp_state_has_config_path(loaded_state)) {
        allocated_config_path =
            strdup(cdsp_state_get_config_path(loaded_state));
        config_path = allocated_config_path;
      }
    }
  }

  if (has_loaded_state && loaded_state) {
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      if (!has_initial_gains[i]) {
        initial_gains[i] = cdsp_state_get_volume(loaded_state, i);
        has_initial_gains[i] = true;
      }
      if (!has_initial_mutes[i]) {
        initial_mutes[i] = cdsp_state_get_mute(loaded_state, i);
        has_initial_mutes[i] = true;
      }
    }
  }

  if (loaded_state) {
    cdsp_state_free(loaded_state);
  }

  if (!wait_config && !no_config && !config_path) {
    logger_error(&g_logger, "Missing required configuration file");
    printf("Error: Missing required configuration file.\n");
    print_usage();
    if (allocated_config_path) free(allocated_config_path);
    return 1;
  }

  dsp_engine_t* engine = cdsp_engine_create();
  if (!engine) {
    logger_error(&g_logger, "Failed to allocate dsp_engine_t: out of memory");
    printf("Error starting engine: Failed to allocate engine\n");
    if (allocated_config_path) free(allocated_config_path);
    return 1;
  }

  if (config_path && !wait_config && !no_config) {
    cdsp_backend_error_t berr = {0};
    if (cdsp_engine_set_config_file(engine, config_path, samplerate_override,
                                    channels_override, format_override,
                                    extra_samples_override, &berr)) {
      logger_info(&g_logger, "DSP engine configured and started");
      printf("Engine started successfully.\n");
    } else {
      logger_error(&g_logger, "Failed to configure engine: %s", berr.message);
      printf("Error starting engine: %s\n", berr.message);
      cdsp_engine_free(engine);
      if (allocated_config_path) free(allocated_config_path);
      return 1;
    }
  } else {
    logger_info(
        &g_logger,
        "Starting engine in inactive state (waiting for websocket config)");
    printf(
        "Starting engine in inactive state (waiting for websocket "
        "configuration)...\n");
  }

  for (int i = 0; i < CDSP_FADER_COUNT; i++) {
    if (has_initial_gains[i]) {
      cdsp_set_fader_volume(engine, (uint32_t)i, (float)initial_gains[i], true);
    }
    if (initial_mutes[i]) {
      cdsp_set_fader_mute(engine, (uint32_t)i, true);
    }
  }

  if (state_file_path) {
    cdsp_set_state_file_path(engine, state_file_path);
  }
  if (config_path) {
    cdsp_set_config_file_path(engine, config_path);
  }

  websocket_server_t* server = NULL;
  if (has_port) {
    server = websocket_server_create(port, bind_address);
    websocket_server_set_engine(server, engine);
    if (websocket_server_start(server)) {
      printf("WebSocket server running on %s:%u\n", bind_address, port);
    } else {
      logger_error(&g_logger, "Failed to start WebSocket server on %s:%u",
                   bind_address, port);
      printf("Error starting WebSocket server\n");
    }
  }

  printf("Press Ctrl+C to stop.\n");
  bool started = (config_path != NULL);
  while (keep_running) {
    cdsp_sleep_ms(100);
    cdsp_engine_poll(engine);
    if (started) {
      if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_INACTIVE) {
        logger_info(&g_logger, "Engine reached inactive state, exiting loop");
        printf("Engine finished processing. Exiting.\n");
        break;
      }
    }
  }

  if (server) websocket_server_free(server);
  cdsp_engine_free(engine);
  if (allocated_config_path) free(allocated_config_path);
  logger_info(&g_logger, "Application exit clean");
  printf("Engine stopped.\n");
#if defined(ENABLE_ASIO) || defined(ENABLE_WASAPI)
  CoUninitialize();
#endif
  return 0;
}
