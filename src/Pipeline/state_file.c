#include "Pipeline/state_file.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Logging/app_logger.h"
#include "Utils/cdsp_path.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#else
#include <unistd.h>
#endif

static const logger_t g_logger = {"dsp.pipeline.state"};

struct dsp_state_s {
  char config_path[1024];
  bool has_config_path;
  bool mute[5];
  double volume[5];
};

dsp_state_t* dsp_state_create(void) {
  dsp_state_t* state = (dsp_state_t*)calloc(1, sizeof(dsp_state_t));
  return state;
}

void dsp_state_free(dsp_state_t* state) { free(state); }

/**
 * @brief Helper function to trim trailing whitespace and newline characters
 * from a string.
 *
 * Modifies the input string in-place by replacing trailing whitespace, carriage
 * returns, and line feeds with null terminators.
 *
 * @param str The string to trim.
 */
static void trim_trailing(char* str) {
  size_t len = strlen(str);
  while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' ||
                     isspace((unsigned char)str[len - 1]))) {
    str[len - 1] = '\0';
    len--;
  }
}

/**
 * @brief Strip trailing YAML comments (' # ...'), respecting double and single
 * quotes so hashes within quoted strings are preserved.
 */
static void strip_trailing_comment(char* str) {
  bool in_double_quote = false;
  bool in_single_quote = false;
  for (char* p = str; *p != '\0'; p++) {
    if (*p == '"' && !in_single_quote) {
      size_t backslashes = 0;
      char* b = p - 1;
      while (b >= str && *b == '\\') {
        backslashes++;
        b--;
      }
      if (backslashes % 2 == 0) {
        in_double_quote = !in_double_quote;
      }
    } else if (*p == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
    } else if (*p == '#' && !in_double_quote && !in_single_quote) {
      if (p == str || isspace((unsigned char)*(p - 1))) {
        *p = '\0';
        break;
      }
    }
  }
  trim_trailing(str);
}

/**
 * @brief Parse a YAML boolean scalar.
 *
 * @param val   The scalar text, already stripped of surrounding whitespace.
 * @param out   Receives the parsed value on success.
 * @return true if @p val is a recognised boolean, false otherwise.
 */
static bool parse_bool_scalar(const char* val, bool* out) {
  if (strcmp(val, "true") == 0 || strcmp(val, "True") == 0 ||
      strcmp(val, "TRUE") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(val, "false") == 0 || strcmp(val, "False") == 0 ||
      strcmp(val, "FALSE") == 0) {
    *out = false;
    return true;
  }
  return false;
}

/**
 * @brief Parse a YAML floating point scalar, rejecting trailing garbage.
 * Supports YAML .inf, -.inf, .nan representations.
 *
 * @param val   The scalar text, already stripped of surrounding whitespace.
 * @param out   Receives the parsed value on success.
 * @return true if @p val is entirely consumed as a number, false otherwise.
 */
static bool parse_double_scalar(const char* val, double* out) {
  if (strcmp(val, ".inf") == 0 || strcmp(val, "+.inf") == 0 ||
      strcmp(val, ".Inf") == 0 || strcmp(val, "+.Inf") == 0 ||
      strcmp(val, ".INF") == 0 || strcmp(val, "+.INF") == 0) {
    *out = INFINITY;
    return true;
  }
  if (strcmp(val, "-.inf") == 0 || strcmp(val, "-.Inf") == 0 ||
      strcmp(val, "-.INF") == 0) {
    *out = -INFINITY;
    return true;
  }
  if (strcmp(val, ".nan") == 0 || strcmp(val, ".NaN") == 0 ||
      strcmp(val, ".NAN") == 0) {
    *out = NAN;
    return true;
  }
  char* end = NULL;
  double parsed = strtod(val, &end);
  if (end == val || !end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

/**
 * @brief Test whether a trimmed line begins a YAML block sequence entry.
 */
static bool is_sequence_item(const char* trimmed) {
  return trimmed[0] == '-' && (trimmed[1] == ' ' || trimmed[1] == '\t');
}

bool dsp_state_load(const char* filename, dsp_state_t* out_state) {
  if (!filename || !out_state) return false;
  FILE* fp = cdsp_fopen(filename, "r");
  if (!fp) {
    logger_warn(&g_logger, "State file could not be opened: %s", filename);
    return false;
  }

  memset(out_state, 0, sizeof(dsp_state_t));

  char line[1024];
  // Parser state machine mode:
  // 0: Root level key-value pairs
  // 1: Processing the elements of the 'mute' list
  // 2: Processing the elements of the 'volume' list
  int mode = 0;
  int mute_idx = 0;
  int vol_idx = 0;
  bool seen_config_path = false;
  bool seen_mute = false;
  bool seen_volume = false;
  bool valid = true;

  while (fgets(line, sizeof(line), fp)) {
    trim_trailing(line);
    strip_trailing_comment(line);

    // Indentation is deliberately ignored. A YAML block sequence nested under
    // a mapping key may be written either at the key's own indentation (which
    // is what upstream's libyaml-based writer emits) or indented under it;
    // both forms are the same document. A sequence therefore ends at the next
    // mapping key, not at a particular column.
    char* trimmed = line;
    while (*trimmed == ' ' || *trimmed == '\t') {
      trimmed++;
    }

    // skip empty or comment or doc-start lines
    if (trimmed[0] == '\0' || trimmed[0] == '#' || strcmp(trimmed, "---") == 0) {
      continue;
    }

    if (mode != 0 && is_sequence_item(trimmed)) {
      char* val = trimmed + 2;
      while (*val == ' ' || *val == '\t') val++;
      if (mode == 1) {  // mute list
        bool parsed = false;
        if (mute_idx >= 5 || !parse_bool_scalar(val, &parsed)) {
          valid = false;
          break;
        }
        out_state->mute[mute_idx++] = parsed;
      } else {  // volume list
        double parsed = 0.0;
        if (vol_idx >= 5 || !parse_double_scalar(val, &parsed)) {
          valid = false;
          break;
        }
        out_state->volume[vol_idx++] = parsed;
      }
      continue;
    }

    // Anything that is not a sequence entry is a root-level mapping key, which
    // closes any sequence currently being read.
    mode = 0;

    if (strncmp(trimmed, "config_path:", 12) == 0) {
      if (seen_config_path) {
        valid = false;
        break;
      }
      seen_config_path = true;
      char* val = trimmed + 12;
      while (*val == ' ' || *val == '\t') val++;
      if (strcmp(val, "null") != 0 && strcmp(val, "~") != 0 && val[0] != '\0') {
        // strip quotes if any and unescape
        if (val[0] == '"' || val[0] == '\'') {
          char quote_char = val[0];
          size_t vlen = strlen(val);
          if (vlen >= 2 && val[vlen - 1] == quote_char) {
            char* dst = out_state->config_path;
            size_t max_dst = sizeof(out_state->config_path) - 1;
            size_t d = 0;
            for (size_t s = 1; s + 1 < vlen && d < max_dst; s++) {
              if (quote_char == '"' && val[s] == '\\' && s + 2 < vlen) {
                s++;
                if (val[s] == '"') dst[d++] = '"';
                else if (val[s] == '\\') dst[d++] = '\\';
                else if (val[s] == 'n') dst[d++] = '\n';
                else if (val[s] == 't') dst[d++] = '\t';
                else { dst[d++] = '\\'; dst[d++] = val[s]; }
              } else {
                dst[d++] = val[s];
              }
            }
            dst[d] = '\0';
          } else {
            strncpy(out_state->config_path, val + 1,
                    sizeof(out_state->config_path) - 1);
            out_state->config_path[sizeof(out_state->config_path) - 1] = '\0';
          }
        } else {
          strncpy(out_state->config_path, val,
                  sizeof(out_state->config_path) - 1);
          out_state->config_path[sizeof(out_state->config_path) - 1] = '\0';
        }
        out_state->has_config_path = true;
      }
    } else if (strncmp(trimmed, "mute:", 5) == 0) {
      if (seen_mute) {
        valid = false;
        break;
      }
      seen_mute = true;
      char* val = trimmed + 5;
      while (*val == ' ' || *val == '\t') val++;
      if (*val == '[') {
        // Flow sequence style: mute: [false, false, false, false, false]
        val++;  // skip '['
        char* close_bracket = strrchr(val, ']');
        if (!close_bracket) {
          valid = false;
          break;
        }
        *close_bracket = '\0';
        mute_idx = 0;
        char* token = strtok(val, ",");
        while (token) {
          while (*token == ' ' || *token == '\t') token++;
          trim_trailing(token);
          bool parsed = false;
          if (mute_idx >= 5 || !parse_bool_scalar(token, &parsed)) {
            valid = false;
            break;
          }
          out_state->mute[mute_idx++] = parsed;
          token = strtok(NULL, ",");
        }
        if (!valid || mute_idx != 5) {
          valid = false;
          break;
        }
        mode = 0;
      } else {
        mode = 1;
        mute_idx = 0;
      }
    } else if (strncmp(trimmed, "volume:", 7) == 0) {
      if (seen_volume) {
        valid = false;
        break;
      }
      seen_volume = true;
      char* val = trimmed + 7;
      while (*val == ' ' || *val == '\t') val++;
      if (*val == '[') {
        // Flow sequence style: volume: [0.0, 0.0, 0.0, 0.0, 0.0]
        val++;  // skip '['
        char* close_bracket = strrchr(val, ']');
        if (!close_bracket) {
          valid = false;
          break;
        }
        *close_bracket = '\0';
        vol_idx = 0;
        char* token = strtok(val, ",");
        while (token) {
          while (*token == ' ' || *token == '\t') token++;
          trim_trailing(token);
          double parsed = 0.0;
          if (vol_idx >= 5 || !parse_double_scalar(token, &parsed)) {
            valid = false;
            break;
          }
          out_state->volume[vol_idx++] = parsed;
          token = strtok(NULL, ",");
        }
        if (!valid || vol_idx != 5) {
          valid = false;
          break;
        }
        mode = 0;
      } else {
        mode = 2;
        vol_idx = 0;
      }
    } else {
      // Upstream deserializes with `deny_unknown_fields`, so an unrecognised
      // key invalidates the whole file.
      valid = false;
      break;
    }
  }

  fclose(fp);

  // Upstream's `load_state` returns None on any deserialization error and the
  // caller then uses nothing from the file. Partially parsed state must not be
  // reported as success: doing so used to adopt `config_path` while resetting
  // every fader to 0 dB and unmuted.
  if (!valid || !seen_config_path || !seen_mute || !seen_volume ||
      mute_idx != 5 || vol_idx != 5) {
    logger_warn(&g_logger, "Invalid statefile, ignoring: %s", filename);
    memset(out_state, 0, sizeof(dsp_state_t));
    return false;
  }

  return true;
}

bool dsp_state_save(const char* filename, const dsp_state_t* state) {
  if (!filename || !state) return false;

  // Save to a temporary file first, then rename to the target filename.
  // This ensures an atomic write, preventing corruption of the state file
  // if the process is interrupted or crashes during write.
  char tmp_name[1024];
  int written = snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", filename);
  if (written < 0 || (size_t)written >= sizeof(tmp_name)) {
    logger_error(&g_logger, "State file path overflow for %s", filename);
    return false;
  }

  FILE* fp = cdsp_fopen(tmp_name, "w");
  if (!fp) {
    logger_error(&g_logger, "Failed to open state temporary file: %s",
                 tmp_name);
    return false;
  }

  fprintf(fp, "---\n");
  if (state->has_config_path) {
    fprintf(fp, "config_path: \"");
    for (const char* p = state->config_path; *p != '\0'; p++) {
      if (*p == '\\' || *p == '"') {
        fputc('\\', fp);
      }
      fputc(*p, fp);
    }
    fprintf(fp, "\"\n");
  } else {
    fprintf(fp, "config_path: null\n");
  }

  // Upstream's libyaml-based writer emits a block sequence at the same
  // indentation as its mapping key, so match that byte-for-byte. The parser
  // accepts either form.
  fprintf(fp, "mute:\n");
  for (int i = 0; i < 5; i++) {
    fprintf(fp, "- %s\n", state->mute[i] ? "true" : "false");
  }

  fprintf(fp, "volume:\n");
  for (int i = 0; i < 5; i++) {
    fprintf(fp, "- %.9g\n", state->volume[i]);
  }

  fflush(fp);
#ifdef _WIN32
  _commit(fileno(fp));
#elif defined(__APPLE__)
  fcntl(fileno(fp), F_FULLFSYNC);
#else
  fsync(fileno(fp));
#endif
  fclose(fp);

#ifdef _WIN32
  if (!MoveFileExA(tmp_name, filename,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    logger_error(&g_logger, "Failed to rename state temporary file %s to %s",
                 tmp_name, filename);
    remove(tmp_name);
    return false;
  }
#else
  if (rename(tmp_name, filename) != 0) {
    logger_error(&g_logger, "Failed to rename state temporary file %s to %s",
                 tmp_name, filename);
    remove(tmp_name);
    return false;
  }
#endif

  logger_info(&g_logger, "State saved to %s", filename);
  return true;
}

const char* dsp_state_get_config_path(const dsp_state_t* state) {
  return state ? state->config_path : NULL;
}

void dsp_state_set_config_path(dsp_state_t* state, const char* path) {
  if (!state) return;
  if (path) {
    strncpy(state->config_path, path, sizeof(state->config_path) - 1);
    state->config_path[sizeof(state->config_path) - 1] = '\0';
    state->has_config_path = true;
  } else {
    state->config_path[0] = '\0';
    state->has_config_path = false;
  }
}

bool dsp_state_has_config_path(const dsp_state_t* state) {
  return state ? state->has_config_path : false;
}

void dsp_state_set_has_config_path(dsp_state_t* state, bool has_path) {
  if (state) state->has_config_path = has_path;
}

bool dsp_state_get_mute(const dsp_state_t* state, int index) {
  if (state && index >= 0 && index < 5) {
    return state->mute[index];
  }
  return false;
}

void dsp_state_set_mute(dsp_state_t* state, int index, bool mute) {
  if (state && index >= 0 && index < 5) {
    state->mute[index] = mute;
  }
}

double dsp_state_get_volume(const dsp_state_t* state, int index) {
  if (state && index >= 0 && index < 5) {
    return state->volume[index];
  }
  return 0.0;
}

void dsp_state_set_volume(dsp_state_t* state, int index, double volume) {
  if (state && index >= 0 && index < 5) {
    state->volume[index] = volume;
  }
}
