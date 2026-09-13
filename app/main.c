#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(ENABLE_ASIO) || defined(ENABLE_WASAPI)
#include <objbase.h>
#endif

#include "Logging/app_logger.h"
#include "cdsp/cdsp_pub_types.h"
#include "cdsp/config.h"
#include "cdsp/fader.h"
#include "cdsp/general.h"
#include "cdsp/processing.h"
#include "cdsp/state.h"
#ifdef ENABLE_WEBSOCKET
#include "Server/websocket_server.h"
#endif
#include "Utils/cdsp_time.h"

#define CDSP_EXIT_OK 0
#define CDSP_EXIT_BAD_CONFIG 101
#define CDSP_EXIT_PROCESSING_ERROR 102
#define CDSP_EXIT_FORCED 103

#if defined(__APPLE__)
#define CDSP_OS_NAME "macos"
#elif defined(__linux__)
#define CDSP_OS_NAME "linux"
#elif defined(_WIN32)
#define CDSP_OS_NAME "windows"
#else
#define CDSP_OS_NAME "unknown"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define CDSP_ARCH_NAME "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define CDSP_ARCH_NAME "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#define CDSP_ARCH_NAME "arm"
#elif defined(__i386__) || defined(_M_IX86)
#define CDSP_ARCH_NAME "x86"
#else
#define CDSP_ARCH_NAME "unknown"
#endif

static const logger_t g_logger = {"dsp.main"};

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t g_term_signal_count = 0;
static volatile sig_atomic_t g_reload_requested = 0;
static volatile sig_atomic_t g_stop_requested = 0;

static void sig_term_handler(int sig) {
  (void)sig;
  if (++g_term_signal_count >= 2) {
    _exit(CDSP_EXIT_FORCED);
  }
  keep_running = 0;
}

#ifdef SIGHUP
static void sig_hup_handler(int sig) {
  (void)sig;
  g_reload_requested = 1;
}
#endif

#ifdef SIGUSR1
static void sig_usr1_handler(int sig) {
  (void)sig;
  g_stop_requested = 1;
}
#endif

static bool parse_gain_value(const char* str, double* out_val) {
  if (!str || *str == '\0') return false;
  char* endptr = NULL;
  double val = strtod(str, &endptr);
  if (endptr == str || *endptr != '\0') return false;
  if (val < -120.0 || val > 20.0) return false;
  *out_val = val;
  return true;
}

static bool parse_positive_int(const char* str, int* out_val) {
  if (!str || *str == '\0') return false;
  char* endptr = NULL;
  long val = strtol(str, &endptr, 10);
  if (endptr == str || *endptr != '\0' || val < 1) return false;
  *out_val = (int)val;
  return true;
}

static bool is_valid_format(const char* str) {
  if (!str) return false;
  return (strcmp(str, "S16_LE") == 0 || strcmp(str, "S24_3_LE") == 0 ||
          strcmp(str, "S24_4_LJ_LE") == 0 || strcmp(str, "S24_4_RJ_LE") == 0 ||
          strcmp(str, "S32_LE") == 0 || strcmp(str, "F32_LE") == 0 ||
          strcmp(str, "F64_LE") == 0);
}

static bool is_valid_ip(const char* ip) {
  if (!ip || *ip == '\0') return false;
  struct in_addr addr4;
  if (inet_pton(AF_INET, ip, &addr4) == 1) return true;
  struct in6_addr addr6;
  if (inet_pton(AF_INET6, ip, &addr6) == 1) return true;
  return false;
}

/**
 * @brief Prints the command-line usage information of the application to
 * standard output.
 */
static void print_usage(void) {
  printf(
      "Usage: cdsp [CONFIGFILE] [OPTIONS]\n"
      "  CONFIGFILE        Path to JSON/YAML configuration file.\n\n"
      "Options:\n"
      "  -h, --help        Print this help message.\n"
      "  -c, --check       Check config file and exit.\n"
      "  -s, --statefile   Use the given file to persist volume/mute state.\n"
      "  -v                Increase message verbosity.\n"
#ifdef ENABLE_WEBSOCKET
      "  -w, --wait        Wait for config from websocket (starts inactive).\n"
#endif
      "  --no_config       Ignore config file in statefile and start without.\n"
#ifdef ENABLE_WEBSOCKET
      "  -p, --port        Port for the WebSocket control server.\n"
      "  -a, --address     IP address to bind WebSocket server to (defaults to "
      "127.0.0.1).\n"
      "  --cert            Path to .pfx/.p12 certificate file.\n"
      "  --pass            Password for certificate file.\n"
#endif
      "  -l, --loglevel    Log level (trace, debug, info, warn, error, off). "
      "Defaults to info.\n"
      "  -o, --logfile     Write logs to the given file path.\n"
      "  --log_rotate_size Rotate log file when size exceeds bytes (min "
      "1000).\n"
      "  --log_keep_nbr    Number of previous log files to keep.\n"
      "  --custom_log_spec Custom logger specification.\n"
      "  -g, --gain        Initial gain in dB for main volume control "
      "(-120..+20).\n"
      "  --gain1           Initial gain in dB for Aux1 fader (-120..+20).\n"
      "  --gain2           Initial gain in dB for Aux2 fader (-120..+20).\n"
      "  --gain3           Initial gain in dB for Aux3 fader (-120..+20).\n"
      "  --gain4           Initial gain in dB for Aux4 fader (-120..+20).\n"
      "  -m, --mute        Start with main volume control muted.\n"
      "  --mute1           Start with Aux1 fader muted.\n"
      "  --mute2           Start with Aux2 fader muted.\n"
      "  --mute3           Start with Aux3 fader muted.\n"
      "  --mute4           Start with Aux4 fader muted.\n"
      "  -r, --samplerate  Override samplerate in config.\n"
      "  -n, --channels    Override number of channels of capture device in "
      "config.\n"
      "  -f, --format      Override sample format of capture device in "
      "config (S16_LE, S24_3_LE, S24_4_LJ_LE, S24_4_RJ_LE, S32_LE, F32_LE, "
      "F64_LE).\n"
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
  signal(SIGINT, sig_term_handler);
  signal(SIGTERM, sig_term_handler);
#ifdef SIGQUIT
  signal(SIGQUIT, sig_term_handler);
#endif
#ifdef SIGHUP
  signal(SIGHUP, sig_hup_handler);
#endif
#ifdef SIGUSR1
  signal(SIGUSR1, sig_usr1_handler);
#endif

  const char* config_path = NULL;
  const char* state_file_path = NULL;
  bool check_only = false;
#ifdef ENABLE_WEBSOCKET
  uint16_t port = 0;
  bool has_port = false;
  const char* bind_address = "127.0.0.1";
  bool has_address = false;
  bool wait_config = false;
  const char* cert_path = NULL;
  const char* cert_pass = NULL;
#else
  const bool wait_config = false;
  const bool has_port = false;
  const bool has_address = false;
#endif
  bool no_config = false;
  const char* log_level_str = "info";
  bool has_loglevel = false;
  int verbosity_count = 0;
  bool has_logfile = false;
  bool has_rotate_size = false;
  size_t log_rotate_size = 0;
  bool has_keep_nbr = false;
  size_t log_keep_nbr = 0;

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
      return CDSP_EXIT_OK;
    } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0) {
      check_only = true;
    } else if (strcmp(arg, "-v") == 0) {
      verbosity_count++;
    } else if (strncmp(arg, "-v", 2) == 0 && strlen(arg) > 2) {
      bool all_v = true;
      for (size_t k = 1; k < strlen(arg); k++) {
        if (arg[k] == 'v')
          verbosity_count++;
        else {
          all_v = false;
          break;
        }
      }
      if (!all_v) {
        fprintf(stderr, "Unknown option: %s\n", arg);
        print_usage();
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--wait") == 0) {
#ifdef ENABLE_WEBSOCKET
      wait_config = true;
#else
      fprintf(stderr, "Error: WebSocket support is not compiled in.\n");
      return CDSP_EXIT_BAD_CONFIG;
#endif
    } else if (strcmp(arg, "--no_config") == 0) {
      no_config = true;
    } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--statefile") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        state_file_path = argv[++i];
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0) {
#ifdef ENABLE_WEBSOCKET
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        char* endptr = NULL;
        long p = strtol(argv[++i], &endptr, 10);
        if (endptr == argv[i] || *endptr != '\0' || p < 0 || p >= 65535) {
          fprintf(stderr,
                  "Error: Invalid port '%s': Must be between 0 and 65534\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        port = (uint16_t)p;
        has_port = true;
      } else {
        fprintf(stderr, "Error: Missing port for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
#else
      fprintf(stderr, "Error: WebSocket support is not compiled in.\n");
      return CDSP_EXIT_BAD_CONFIG;
#endif
    } else if (strcmp(arg, "-a") == 0 || strcmp(arg, "--address") == 0) {
#ifdef ENABLE_WEBSOCKET
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        bind_address = argv[++i];
        if (!is_valid_ip(bind_address)) {
          fprintf(stderr,
                  "Error: Invalid address '%s': Must be a valid IP address\n",
                  bind_address);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_address = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
#else
      fprintf(stderr, "Error: WebSocket support is not compiled in.\n");
      return CDSP_EXIT_BAD_CONFIG;
#endif
#ifdef ENABLE_WEBSOCKET
    } else if (strcmp(arg, "--cert") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        cert_path = argv[++i];
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--pass") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        cert_pass = argv[++i];
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
#endif
    } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loglevel") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        log_level_str = argv[++i];
        if (strcmp(log_level_str, "trace") != 0 &&
            strcmp(log_level_str, "debug") != 0 &&
            strcmp(log_level_str, "info") != 0 &&
            strcmp(log_level_str, "warn") != 0 &&
            strcmp(log_level_str, "error") != 0 &&
            strcmp(log_level_str, "off") != 0) {
          fprintf(stderr,
                  "Error: Invalid log level '%s'. Must be one of: trace, "
                  "debug, info, warn, error, off\n",
                  log_level_str);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_loglevel = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--custom_log_spec") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        log_level_str = argv[++i];
        has_loglevel = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--logfile") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        const char* logfile_path = argv[++i];
        app_logger_set_logfile(logfile_path);
        has_logfile = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--log_rotate_size") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        char* endptr = NULL;
        long sz = strtol(argv[++i], &endptr, 10);
        if (endptr == argv[i] || *endptr != '\0' || sz < 1000) {
          fprintf(stderr,
                  "Error: Invalid log_rotate_size '%s': Must be an integer >= "
                  "1000\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        log_rotate_size = (size_t)sz;
        has_rotate_size = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--log_keep_nbr") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        char* endptr = NULL;
        long nbr = strtol(argv[++i], &endptr, 10);
        if (endptr == argv[i] || *endptr != '\0' || nbr < 0) {
          fprintf(stderr, "Error: Invalid log_keep_nbr '%s'\n", argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        log_keep_nbr = (size_t)nbr;
        has_keep_nbr = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-g") == 0 || strcmp(arg, "--gain") == 0) {
      if (i + 1 < argc) {
        if (!parse_gain_value(argv[++i], &initial_gains[0])) {
          fprintf(stderr,
                  "Error: Invalid gain value '%s': Must be a number between "
                  "-120 and +20\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_initial_gains[0] = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--gain1") == 0) {
      if (i + 1 < argc) {
        if (!parse_gain_value(argv[++i], &initial_gains[1])) {
          fprintf(stderr,
                  "Error: Invalid gain1 value '%s': Must be a number between "
                  "-120 and +20\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_initial_gains[1] = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--gain2") == 0) {
      if (i + 1 < argc) {
        if (!parse_gain_value(argv[++i], &initial_gains[2])) {
          fprintf(stderr,
                  "Error: Invalid gain2 value '%s': Must be a number between "
                  "-120 and +20\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_initial_gains[2] = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--gain3") == 0) {
      if (i + 1 < argc) {
        if (!parse_gain_value(argv[++i], &initial_gains[3])) {
          fprintf(stderr,
                  "Error: Invalid gain3 value '%s': Must be a number between "
                  "-120 and +20\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_initial_gains[3] = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "--gain4") == 0) {
      if (i + 1 < argc) {
        if (!parse_gain_value(argv[++i], &initial_gains[4])) {
          fprintf(stderr,
                  "Error: Invalid gain4 value '%s': Must be a number between "
                  "-120 and +20\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
        has_initial_gains[4] = true;
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
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
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        if (!parse_positive_int(argv[++i], &samplerate_override)) {
          fprintf(stderr,
                  "Error: Invalid samplerate value '%s': Must be a positive "
                  "integer\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--channels") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        if (!parse_positive_int(argv[++i], &channels_override)) {
          fprintf(stderr,
                  "Error: Invalid channels value '%s': Must be a positive "
                  "integer\n",
                  argv[i]);
          return CDSP_EXIT_BAD_CONFIG;
        }
      } else {
        fprintf(stderr, "Error: Missing value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--format") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        format_override = argv[++i];
        if (!is_valid_format(format_override)) {
          fprintf(
              stderr,
              "Error: Invalid format '%s'. Must be one of: S16_LE, S24_3_LE, "
              "S24_4_LJ_LE, S24_4_RJ_LE, S32_LE, F32_LE, F64_LE\n",
              format_override);
          return CDSP_EXIT_BAD_CONFIG;
        }
      } else {
        fprintf(stderr, "Error: Missing format value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else if (strcmp(arg, "-e") == 0 || strcmp(arg, "--extra_samples") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        if (!parse_positive_int(argv[++i], &extra_samples_override)) {
          fprintf(stderr,
                  "Error: Missing or invalid extra_samples value for %s (must "
                  "be >= 1)\n",
                  arg);
          return CDSP_EXIT_BAD_CONFIG;
        }
      } else {
        fprintf(stderr, "Error: Missing extra_samples value for %s\n", arg);
        return CDSP_EXIT_BAD_CONFIG;
      }
    } else {
      if (arg[0] != '-') {
        if (config_path) {
          fprintf(stderr, "Error: Unexpected argument: %s\n", arg);
          return CDSP_EXIT_BAD_CONFIG;
        }
        config_path = arg;
      } else {
        fprintf(stderr, "Unknown option: %s\n", arg);
        print_usage();
        return CDSP_EXIT_BAD_CONFIG;
      }
    }
  }

  // Conflict validation: -v conflicts with -l
  if (verbosity_count > 0 && has_loglevel) {
    fprintf(stderr,
            "Error: The argument '-v' cannot be used with '--loglevel'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }

  // Argument dependency validations
#ifdef ENABLE_WEBSOCKET
  if (wait_config && !has_port) {
    fprintf(stderr, "Error: The argument '--wait' requires '--port <PORT>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
  if (has_address && !has_port) {
    fprintf(stderr,
            "Error: The argument '--address' requires '--port <PORT>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
  if (cert_path && !has_port) {
    fprintf(stderr, "Error: The argument '--cert' requires '--port <PORT>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
  if (cert_pass && !has_port) {
    fprintf(stderr, "Error: The argument '--pass' requires '--port <PORT>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
#endif
  if (no_config) {
    if (!wait_config) {
      fprintf(stderr, "Error: The argument '--no_config' requires '--wait'\n");
      return CDSP_EXIT_BAD_CONFIG;
    }
    if (!state_file_path) {
      fprintf(stderr,
              "Error: The argument '--no_config' requires '--statefile "
              "<STATEFILE>'\n");
      return CDSP_EXIT_BAD_CONFIG;
    }
    if (config_path) {
      fprintf(stderr,
              "Error: The argument '--no_config' cannot be used with a config "
              "file\n");
      return CDSP_EXIT_BAD_CONFIG;
    }
  }
  if (check_only && !config_path) {
    fprintf(stderr, "Error: The argument '--check' requires a config file\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
  if (has_rotate_size && !has_logfile) {
    fprintf(stderr,
            "Error: The argument '--log_rotate_size' requires '--logfile "
            "<LOGFILE>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }
  if (has_keep_nbr && !has_rotate_size) {
    fprintf(stderr,
            "Error: The argument '--log_keep_nbr' requires '--log_rotate_size "
            "<ROTATE_SIZE>'\n");
    return CDSP_EXIT_BAD_CONFIG;
  }

  if (has_rotate_size) {
    app_logger_set_logfile_rotation(log_rotate_size, log_keep_nbr);
  }

  // Resolve log level
  if (!has_loglevel) {
    if (verbosity_count == 1) {
      log_level_str = "debug";
    } else if (verbosity_count >= 2) {
      log_level_str = "trace";
    } else {
      log_level_str = "info";
    }
  }
  cdsp_set_log_level(log_level_str);

  // Register process-global persistent overrides
  cdsp_set_cli_overrides(samplerate_override, channels_override,
                         format_override, extra_samples_override);

  if (check_only) {
    char* result = NULL;
    cdsp_config_error_type_t is_error = CDSP_CONFIG_ERR_NONE;
    if (cdsp_validate_config_file_with_overrides(
            config_path, samplerate_override, channels_override,
            format_override, extra_samples_override, &result, &is_error) &&
        is_error == CDSP_CONFIG_ERR_NONE) {
      printf("Config is valid\n");
      if (result) free(result);
      app_logger_flush_and_stop(app_logger_get_shared());
      return CDSP_EXIT_OK;
    } else {
      printf("Config is not valid\n");
      printf("%s\n", result ? result : "Invalid config");
      if (result) free(result);
      app_logger_flush_and_stop(app_logger_get_shared());
      return CDSP_EXIT_BAD_CONFIG;
    }
  }

  logger_info(&g_logger, "CamillaDSP version %s (cdsp)", cdsp_get_version());
  logger_info(&g_logger, "Running on %s, %s", CDSP_OS_NAME, CDSP_ARCH_NAME);

  // Load state file if present
  char* allocated_config_path = NULL;
  cdsp_state_t* loaded_state = cdsp_state_create();
  bool has_loaded_state = false;
  if (state_file_path && loaded_state) {
    if (cdsp_state_load(state_file_path, loaded_state)) {
      has_loaded_state = true;
      logger_debug(&g_logger, "Loaded state: from %s", state_file_path);
    }
  }

  if (has_loaded_state && loaded_state) {
    logger_debug(&g_logger, "Using statefile for initial volume");
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      if (!has_initial_gains[i]) {
        initial_gains[i] = cdsp_state_get_volume(loaded_state, i);
      }
    }
    logger_debug(&g_logger, "Using statefile for initial mute");
    for (int i = 0; i < CDSP_FADER_COUNT; i++) {
      if (!has_initial_mutes[i]) {
        initial_mutes[i] = cdsp_state_get_mute(loaded_state, i);
      }
    }
  } else {
    logger_debug(&g_logger, "Using default initial volume");
    logger_debug(&g_logger, "Using default initial mute");
  }

  for (int i = 0; i < CDSP_FADER_COUNT; i++) {
    if (has_initial_gains[i]) {
      if (i == 0)
        logger_debug(&g_logger,
                     "Using command line argument for initial main volume");
      else
        logger_debug(&g_logger,
                     "Using command line argument for initial Aux%d volume", i);
    }
    if (has_initial_mutes[i]) {
      if (i == 0)
        logger_debug(&g_logger,
                     "Using command line argument for initial main mute");
      else
        logger_debug(&g_logger,
                     "Using command line argument for initial Aux%d mute", i);
    }
  }

  if (!config_path && has_loaded_state && loaded_state) {
    if (no_config) {
      logger_debug(
          &g_logger,
          "Ignoring config from statefile as per command line argument");
    } else if (cdsp_state_has_config_path(loaded_state)) {
      logger_debug(&g_logger, "Using config from statefile");
      allocated_config_path = strdup(cdsp_state_get_config_path(loaded_state));
      config_path = allocated_config_path;
    }
  }

  // Save state to statefile if needed (matching bin.rs:586-597)
  if (state_file_path) {
    bool state_changed = !has_loaded_state;
    if (has_loaded_state) {
      const char* loaded_cfg = cdsp_state_get_config_path(loaded_state);
      if ((config_path &&
           (!loaded_cfg || strcmp(config_path, loaded_cfg) != 0)) ||
          (!config_path && loaded_cfg)) {
        state_changed = true;
      }
      for (int i = 0; i < CDSP_FADER_COUNT && !state_changed; i++) {
        if (initial_gains[i] != cdsp_state_get_volume(loaded_state, i) ||
            initial_mutes[i] != cdsp_state_get_mute(loaded_state, i)) {
          state_changed = true;
        }
      }
    }
    if (state_changed) {
      cdsp_state_t* state_to_save = cdsp_state_create();
      if (state_to_save) {
        if (config_path) cdsp_state_set_config_path(state_to_save, config_path);
        for (int i = 0; i < CDSP_FADER_COUNT; i++) {
          cdsp_state_set_volume(state_to_save, i, initial_gains[i]);
          cdsp_state_set_mute(state_to_save, i, initial_mutes[i]);
        }
        cdsp_state_save(state_file_path, state_to_save);
        cdsp_state_free(state_to_save);
      }
    } else {
      logger_debug(&g_logger, "No change to state from %s, not overwriting.",
                   state_file_path);
    }
  }

  if (loaded_state) {
    cdsp_state_free(loaded_state);
  }

  // Upstream: configfile is required unless wait or statefile is present
  if (!config_path) {
    if (!wait_config) {
      if (!state_file_path) {
        logger_error(&g_logger, "Missing required configuration file");
        fprintf(stderr, "Error: Missing required configuration file.\n");
        print_usage();
        app_logger_flush_and_stop(app_logger_get_shared());
        return CDSP_EXIT_BAD_CONFIG;
      }
      // With statefile but no config and no wait, exit OK cleanly
      logger_debug(&g_logger,
                   "Wait mode is disabled, there are no queued commands, and "
                   "no new config. Exiting.");
      app_logger_flush_and_stop(app_logger_get_shared());
      return CDSP_EXIT_OK;
    }
  }

  dsp_engine_t* engine = cdsp_engine_create();
  if (!engine) {
    logger_error(&g_logger, "Failed to allocate dsp_engine_t: out of memory");
    fprintf(stderr, "Error starting engine: Failed to allocate engine\n");
    if (allocated_config_path) free(allocated_config_path);
    app_logger_flush_and_stop(app_logger_get_shared());
    return CDSP_EXIT_PROCESSING_ERROR;
  }

  for (int i = 0; i < CDSP_FADER_COUNT; i++) {
    cdsp_set_fader_volume(engine, (uint32_t)i, (float)initial_gains[i], true);
    if (initial_mutes[i]) {
      cdsp_set_fader_mute(engine, (uint32_t)i, true);
    }
  }

  if (state_file_path) {
    cdsp_set_state_file_path(engine, state_file_path);
  }

  // Load configuration if path is available (wait mode still loads initial
  // config)
  if (config_path && !no_config) {
    cdsp_backend_error_t berr = {0};
    if (cdsp_engine_set_config_file(engine, config_path, samplerate_override,
                                    channels_override, format_override,
                                    extra_samples_override, &berr)) {
      logger_info(&g_logger, "DSP engine configured and started");
      fprintf(stderr, "Engine started successfully.\n");
    } else {
      logger_error(&g_logger, "Failed to configure engine: %s", berr.message);
      fprintf(stderr, "Error starting engine: %s\n", berr.message);
      cdsp_engine_free(engine);
      if (allocated_config_path) free(allocated_config_path);
      app_logger_flush_and_stop(app_logger_get_shared());
      return CDSP_EXIT_BAD_CONFIG;
    }
  } else {
    logger_info(
        &g_logger,
        "Starting engine in inactive state (waiting for websocket config)");
    fprintf(stderr,
            "Starting engine in inactive state (waiting for websocket "
            "configuration)...\n");
  }

#ifdef ENABLE_WEBSOCKET
  websocket_server_t* server = NULL;
  if (has_port) {
    server = websocket_server_create(port, bind_address);
    websocket_server_set_engine(server, engine);
    if (websocket_server_start(server)) {
      fprintf(stderr, "WebSocket server running on %s:%u\n", bind_address,
              port);
    } else {
      logger_error(&g_logger, "Failed to start WebSocket server on %s:%u",
                   bind_address, port);
      fprintf(stderr, "Error starting WebSocket server\n");
    }
  }
#endif

  fprintf(stderr, "Press Ctrl+C to stop.\n");
  bool is_batch_mode = (config_path != NULL && !wait_config);
  while (keep_running) {
    cdsp_sleep_ms(100);
    cdsp_engine_poll(engine);
#ifdef SIGHUP
    if (g_reload_requested) {
      g_reload_requested = 0;
      logger_info(&g_logger, "Reloading configuration via SIGHUP");
      cdsp_backend_error_t berr = {0};
      if (!cdsp_reload_config(engine, &berr)) {
        logger_error(&g_logger, "Reload failed: %s", berr.message);
      }
    }
#endif
#ifdef SIGUSR1
    if (g_stop_requested) {
      g_stop_requested = 0;
      logger_info(&g_logger, "Stopping processing via SIGUSR1");
      cdsp_stop(engine);
    }
#endif
    if (is_batch_mode) {
      if (cdsp_get_state(engine) == CDSP_PROCESSING_STATE_INACTIVE) {
        logger_info(&g_logger, "Engine reached inactive state, exiting loop");
        fprintf(stderr, "Engine finished processing. Exiting.\n");
        break;
      }
    }
  }

#ifdef ENABLE_WEBSOCKET
  if (server) websocket_server_free(server);
#endif
  cdsp_engine_free(engine);
  if (allocated_config_path) free(allocated_config_path);
  logger_info(&g_logger, "Application exit clean");
  fprintf(stderr, "Engine stopped.\n");
  app_logger_flush_and_stop(app_logger_get_shared());
#if defined(ENABLE_ASIO) || defined(ENABLE_WASAPI)
  CoUninitialize();
#endif
  return CDSP_EXIT_OK;
}
