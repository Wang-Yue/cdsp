#include "Config/config_parse_filters.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "Config/cJSON.h"
#include "Config/config_parser_internal.h"
#include "Config/configuration.h"
#include "Config/filter_config_types.h"
#include "Config/processor_config_types.h"

int config_parse_filters(const cJSON* filters_obj, dsp_config_t* config,
                         config_error_t* err) {
  if (!cJSON_IsObject(filters_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "filters must be an object");
    return -1;
  }
  int size = 0;
  cJSON* filter_child = NULL;
  cJSON_ArrayForEach(filter_child, filters_obj) { size++; }
  if (size == 0) return 0;

  config->filters =
      (named_filter_config_t*)calloc(size, sizeof(named_filter_config_t));
  if (!config->filters) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->filters_count = size;

  int f = 0;
  cJSON_ArrayForEach(filter_child, filters_obj) {
    named_filter_config_t* nf = &config->filters[f];
    const char* f_name = filter_child->string ? filter_child->string : "";
    strncpy(nf->name, f_name, sizeof(nf->name) - 1);
    nf->name[sizeof(nf->name) - 1] = '\0';

    if (!cJSON_IsObject(filter_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter definition must be an object");
      return -1;
    }

    static const char* const allowed_filter_keys[] = {"type", "description",
                                                      "parameters", NULL};
    if (validate_unknown_fields(filter_child, allowed_filter_keys,
                                "filter definition", err) != 0) {
      return -1;
    }

    filter_config_t* f_conf = &nf->filter;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(filter_child, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
      /* Upstream Filter is an internally tagged enum: serde fails with
       * "missing field `type`" rather than defaulting to a variant. */
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter '%s': missing or non-string 'type'", nf->name);
      return -1;
    }
    f_conf->type = filter_type_from_string(type->valuestring);
    if (f_conf->type == FILTER_TYPE_INVALID) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter '%s': unknown filter type '%s'", nf->name,
                       type->valuestring);
      return -1;
    }

    cJSON* params =
        cJSON_GetObjectItemCaseSensitive(filter_child, "parameters");
    if (!cJSON_IsObject(params)) {
      /* Every upstream Filter variant declares `parameters` without a serde
       * default, so omitting it is "missing field `parameters`". */
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter '%s': missing 'parameters' object", nf->name);
      return -1;
    }
    {
      switch (f_conf->type) {
        case FILTER_TYPE_GAIN: {
          static const char* const allowed[] = {"gain", "scale", "inverted",
                                                "mute", NULL};
          if (validate_unknown_fields(params, allowed, "Gain filter parameters",
                                      err) != 0)
            return -1;
          static const char* const req_gain[] = {"gain", NULL};
          if (require_json_fields(params, req_gain, "Gain filter parameters",
                                  NULL, err) != 0)
            return -1;
          gain_config_t* gp = &f_conf->parameters.gain;
          if (!parse_json_double(params, "gain", &gp->gain)) {
            config_error_set(
                err, CONFIG_ERR_PARSE,
                "field 'gain' in Gain filter parameters must be a number");
            return -1;
          }
          gp->has_gain = true;
          char str_buf[64];
          if (parse_json_str(params, "scale", str_buf, sizeof(str_buf))) {
            if (strcmp(str_buf, "linear") == 0) {
              gp->scale = GAIN_SCALE_LINEAR;
            } else if (strcmp(str_buf, "dB") == 0) {
              gp->scale = GAIN_SCALE_DB;
            } else {
              config_error_set(
                  err, CONFIG_ERR_PARSE,
                  "unknown variant '%s', expected one of 'linear', 'dB'",
                  str_buf);
              return -1;
            }
          } else {
            gp->scale = GAIN_SCALE_DB;
          }
          parse_json_bool(params, "inverted", &gp->inverted);
          parse_json_bool(params, "mute", &gp->mute);
          break;
        }
        case FILTER_TYPE_VOLUME: {
          static const char* const allowed[] = {"ramp_time_ms", "limit",
                                                "fader", NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Volume filter parameters", err) != 0)
            return -1;
          volume_config_t* vp = &f_conf->parameters.volume;
          vp->has_ramp_time_ms =
              parse_json_double(params, "ramp_time_ms", &vp->ramp_time_ms);
          vp->has_limit = parse_json_double(params, "limit", &vp->limit);
          static const config_enum_variant_t volume_faders[] = {
              {"Aux1", FADER_AUX1},
              {"Aux2", FADER_AUX2},
              {"Aux3", FADER_AUX3},
              {"Aux4", FADER_AUX4},
              {NULL, 0}};
          int fader_val = 0;
          if (parse_enum_required(params, "fader", volume_faders,
                                  "Volume filter parameters", &fader_val,
                                  err) != 0)
            return -1;
          vp->fader = (fader_t)fader_val;
          break;
        }
        case FILTER_TYPE_LOUDNESS: {
          static const char* const allowed[] = {
              "reference_level", "high_boost", "low_boost", "attenuate_mid",
              "fader",           "high_freq",  "low_freq",  "high_q",
              "low_q",           NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Loudness filter parameters", err) != 0)
            return -1;
          loudness_config_t* lp = &f_conf->parameters.loudness;
          static const char* const req_loudness[] = {"reference_level", NULL};
          if (require_json_fields(params, req_loudness,
                                  "Loudness filter parameters", NULL, err) != 0)
            return -1;
          lp->has_reference_level = parse_json_double(params, "reference_level",
                                                      &lp->reference_level);
          lp->has_high_boost =
              parse_json_double(params, "high_boost", &lp->high_boost);
          lp->has_low_boost =
              parse_json_double(params, "low_boost", &lp->low_boost);
          parse_json_bool(params, "attenuate_mid", &lp->attenuate_mid);
          lp->has_high_freq =
              parse_json_double(params, "high_freq", &lp->high_freq);
          lp->has_low_freq =
              parse_json_double(params, "low_freq", &lp->low_freq);
          lp->has_high_q = parse_json_double(params, "high_q", &lp->high_q);
          lp->has_low_q = parse_json_double(params, "low_q", &lp->low_q);
          static const config_enum_variant_t loudness_faders[] = {
              {"Main", FADER_MAIN}, {"Aux1", FADER_AUX1}, {"Aux2", FADER_AUX2},
              {"Aux3", FADER_AUX3}, {"Aux4", FADER_AUX4}, {NULL, 0}};
          int lfader_val = FADER_MAIN;
          if (parse_enum_optional(params, "fader", loudness_faders,
                                  "Loudness filter parameters", &lfader_val,
                                  err) != 0)
            return -1;
          lp->fader = (fader_t)lfader_val;
          break;
        }
        case FILTER_TYPE_BIQUAD: {
          static const char* const allowed[] = {
              "type",     "freq",      "gain",
              "q",        "bandwidth", "slope",
              "a1",       "a2",        "b0",
              "b1",       "b2",        "freq_z",
              "freq_p",   "q_p",       "normalize_at_dc",
              "freq_act", "q_act",     "freq_target",
              "q_target", NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Biquad filter parameters", err) != 0)
            return -1;
          biquad_config_t* bp = &f_conf->parameters.biquad;
          // Upstream's BiquadParameters is an internally tagged enum, so the
          // tag is required and an unknown variant is a deserialization error.
          // Falling through the old if/else chain left type at 0 (Free) with
          // all-zero coefficients, silencing the channel without a diagnostic.
          static const config_enum_variant_t biquad_types[] = {
              {"Free", BIQUAD_TYPE_FREE},
              {"Highpass", BIQUAD_TYPE_HIGHPASS},
              {"Lowpass", BIQUAD_TYPE_LOWPASS},
              {"HighpassFO", BIQUAD_TYPE_HIGHPASS_FO},
              {"LowpassFO", BIQUAD_TYPE_LOWPASS_FO},
              {"Highshelf", BIQUAD_TYPE_HIGHSHELF},
              {"Lowshelf", BIQUAD_TYPE_LOWSHELF},
              {"HighshelfFO", BIQUAD_TYPE_HIGHSHELF_FO},
              {"LowshelfFO", BIQUAD_TYPE_LOWSHELF_FO},
              {"Peaking", BIQUAD_TYPE_PEAKING},
              {"Notch", BIQUAD_TYPE_NOTCH},
              {"Bandpass", BIQUAD_TYPE_BANDPASS},
              {"Allpass", BIQUAD_TYPE_ALLPASS},
              {"AllpassFO", BIQUAD_TYPE_ALLPASS_FO},
              {"GeneralNotch", BIQUAD_TYPE_GENERAL_NOTCH},
              {"LinkwitzTransform", BIQUAD_TYPE_LINKWITZ_TRANSFORM},
              {NULL, 0}};
          int biquad_type = 0;
          if (parse_enum_required(params, "type", biquad_types,
                                  "Biquad filter parameters", &biquad_type,
                                  err) != 0)
            return -1;
          bp->type = (biquad_type_t)biquad_type;
          if (bp->type == BIQUAD_TYPE_FREE) {
            static const char* const allowed_free[] = {"type", "a1", "a2", "b0",
                                                       "b1",   "b2", NULL};
            if (validate_unknown_fields(params, allowed_free,
                                        "Biquad Free filter parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_HIGHPASS_FO ||
                     bp->type == BIQUAD_TYPE_LOWPASS_FO ||
                     bp->type == BIQUAD_TYPE_ALLPASS_FO) {
            static const char* const allowed_fo[] = {"type", "freq", NULL};
            if (validate_unknown_fields(params, allowed_fo,
                                        "Biquad FO filter parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_HIGHSHELF_FO ||
                     bp->type == BIQUAD_TYPE_LOWSHELF_FO) {
            static const char* const allowed_shelf_fo[] = {
                "type", "freq", "gain", NULL};
            if (validate_unknown_fields(params, allowed_shelf_fo,
                                        "Biquad Shelf FO filter parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_HIGHPASS ||
                     bp->type == BIQUAD_TYPE_LOWPASS) {
            static const char* const allowed_pass[] = {
                "type", "freq", "q", NULL};
            if (validate_unknown_fields(params, allowed_pass,
                                        "Biquad Highpass/Lowpass parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_GENERAL_NOTCH) {
            static const char* const allowed_gn[] = {
                "type", "freq_p", "freq_z", "q_p", "normalize_at_dc", NULL};
            if (validate_unknown_fields(params, allowed_gn,
                                        "Biquad GeneralNotch parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_LINKWITZ_TRANSFORM) {
            static const char* const allowed_lt[] = {
                "type", "freq_act", "q_act", "freq_target", "q_target", NULL};
            if (validate_unknown_fields(params, allowed_lt,
                                        "Biquad LinkwitzTransform parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_PEAKING) {
            static const char* const allowed_peaking[] = {
                "type", "freq", "gain", "q", "bandwidth", NULL};
            if (validate_unknown_fields(params, allowed_peaking,
                                        "Biquad Peaking parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_HIGHSHELF ||
                     bp->type == BIQUAD_TYPE_LOWSHELF) {
            static const char* const allowed_shelf[] = {
                "type", "freq", "gain", "q", "slope", NULL};
            if (validate_unknown_fields(params, allowed_shelf,
                                        "Biquad Shelf parameters",
                                        err) != 0)
              return -1;
          } else if (bp->type == BIQUAD_TYPE_ALLPASS ||
                     bp->type == BIQUAD_TYPE_BANDPASS ||
                     bp->type == BIQUAD_TYPE_NOTCH) {
            static const char* const allowed_width[] = {
                "type", "freq", "q", "bandwidth", NULL};
            if (validate_unknown_fields(params, allowed_width,
                                        "Biquad filter parameters",
                                        err) != 0)
              return -1;
          }
          {
            // Each upstream variant carries its own required fields; a missing
            // one is "missing field `freq`" and rejects the configuration.
            const char* variant =
                cJSON_GetObjectItemCaseSensitive(params, "type")->valuestring;
            static const char* const req_free[] = {"a1", "a2", "b0",
                                                   "b1", "b2", NULL};
            static const char* const req_freq_q[] = {"freq", "q", NULL};
            static const char* const req_freq[] = {"freq", NULL};
            static const char* const req_freq_gain[] = {"freq", "gain", NULL};
            static const char* const req_notch[] = {"freq_p", "freq_z", "q_p",
                                                    NULL};
            static const char* const req_lt[] = {
                "freq_act", "q_act", "freq_target", "q_target", NULL};
            static const char* const width_q_bw[] = {"q", "bandwidth", NULL};
            static const char* const width_q_slope[] = {"q", "slope", NULL};
            const char* const* required = NULL;
            const char* const* width = NULL;
            switch (bp->type) {
              case BIQUAD_TYPE_FREE:
                required = req_free;
                break;
              case BIQUAD_TYPE_HIGHPASS:
              case BIQUAD_TYPE_LOWPASS:
                required = req_freq_q;
                break;
              case BIQUAD_TYPE_HIGHPASS_FO:
              case BIQUAD_TYPE_LOWPASS_FO:
              case BIQUAD_TYPE_ALLPASS_FO:
                required = req_freq;
                break;
              case BIQUAD_TYPE_HIGHSHELF_FO:
              case BIQUAD_TYPE_LOWSHELF_FO:
                required = req_freq_gain;
                break;
              case BIQUAD_TYPE_PEAKING:
                required = req_freq_gain;
                width = width_q_bw;
                break;
              case BIQUAD_TYPE_HIGHSHELF:
              case BIQUAD_TYPE_LOWSHELF:
                required = req_freq_gain;
                width = width_q_slope;
                break;
              case BIQUAD_TYPE_NOTCH:
              case BIQUAD_TYPE_BANDPASS:
              case BIQUAD_TYPE_ALLPASS:
                required = req_freq;
                width = width_q_bw;
                break;
              case BIQUAD_TYPE_GENERAL_NOTCH:
                required = req_notch;
                break;
              case BIQUAD_TYPE_LINKWITZ_TRANSFORM:
                required = req_lt;
                break;
              default:
                break;
            }
            if (required && require_json_fields(params, required,
                                                "Biquad filter parameters",
                                                variant, err) != 0)
              return -1;
            if (width && require_json_any_field(params, width,
                                                "Biquad filter parameters",
                                                variant, err) != 0)
              return -1;
          }
          parse_json_double(params, "freq", &bp->freq);
          parse_json_double(params, "gain", &bp->gain);

          if (parse_json_double(params, "q", &bp->q)) {
            bp->steepness_type = STEEPNESS_TYPE_Q;
          } else if (parse_json_double(params, "bandwidth", &bp->bandwidth)) {
            bp->steepness_type = STEEPNESS_TYPE_BANDWIDTH;
          } else if (parse_json_double(params, "slope", &bp->slope)) {
            bp->steepness_type = STEEPNESS_TYPE_SLOPE;
          }

          parse_json_double(params, "a1", &bp->a1);
          parse_json_double(params, "a2", &bp->a2);
          parse_json_double(params, "b0", &bp->b0);
          parse_json_double(params, "b1", &bp->b1);
          parse_json_double(params, "b2", &bp->b2);
          parse_json_double(params, "freq_z", &bp->freq_notch);
          parse_json_double(params, "freq_p", &bp->freq_pole);
          parse_json_double(params, "q_p", &bp->q_p);
          parse_json_bool(params, "normalize_at_dc", &bp->normalize_at_dc);
          parse_json_double(params, "freq_act", &bp->freq_act);
          parse_json_double(params, "q_act", &bp->q_act);
          parse_json_double(params, "freq_target", &bp->freq_target);
          parse_json_double(params, "q_target", &bp->q_target);
          break;
        }
        case FILTER_TYPE_DELAY: {
          static const char* const allowed[] = {"delay", "delay_unit",
                                                "subsample", NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Delay filter parameters", err) != 0)
            return -1;
          static const char* const req_delay[] = {"delay", "delay_unit", NULL};
          if (require_json_fields(params, req_delay, "Delay filter parameters",
                                  NULL, err) != 0)
            return -1;
          delay_config_t* dp = &f_conf->parameters.delay;
          if (!parse_json_double(params, "delay", &dp->delay)) {
            config_error_set(
                err, CONFIG_ERR_PARSE,
                "field 'delay' in Delay filter parameters must be a number");
            return -1;
          }
          char unit_buf[64];
          if (parse_json_str(params, "delay_unit", unit_buf,
                             sizeof(unit_buf))) {
            if (strcmp(unit_buf, "us") == 0)
              dp->delay_unit = DELAY_UNIT_US;
            else if (strcmp(unit_buf, "ms") == 0)
              dp->delay_unit = DELAY_UNIT_MS;
            else if (strcmp(unit_buf, "s") == 0)
              dp->delay_unit = DELAY_UNIT_S;
            else if (strcmp(unit_buf, "samples") == 0)
              dp->delay_unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(unit_buf, "mm") == 0)
              dp->delay_unit = DELAY_UNIT_MM;
            else {
              config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                               "Filter '%s': invalid delay_unit '%s'", nf->name,
                               unit_buf);
              return -1;
            }
          } else {
            config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                             "Filter '%s': missing required 'delay_unit'",
                             nf->name);
            return -1;
          }
          parse_json_bool(params, "subsample", &dp->subsample);
          break;
        }
        case FILTER_TYPE_CONV: {
          static const char* const allowed[] = {
              "type",    "values", "filename",         "format",
              "channel", "length", "skip_bytes_lines", "read_bytes_lines",
              NULL};
          if (validate_unknown_fields(params, allowed, "Conv filter parameters",
                                      err) != 0)
            return -1;
          convolution_config_t* cp = &f_conf->parameters.conv;
          /* Upstream ConvParameters is an internally tagged enum, so the tag is
           * mandatory and an unknown tag is a hard deserialization error. */
          static const config_enum_variant_t conv_types[] = {
              {"Values", CONV_TYPE_VALUES},
              {"Wav", CONV_TYPE_WAV},
              {"Raw", CONV_TYPE_RAW},
              {"Dummy", CONV_TYPE_DUMMY},
              {NULL, 0}};
          int conv_type = 0;
          if (parse_enum_required(params, "type", conv_types,
                                  "Conv filter parameters", &conv_type,
                                  err) != 0)
            return -1;
          cp->type = (conv_type_t)conv_type;
          {
            const char* variant =
                cJSON_GetObjectItemCaseSensitive(params, "type")->valuestring;
            static const char* const req_values[] = {"values", NULL};
            static const char* const req_filename[] = {"filename", NULL};
            static const char* const req_length[] = {"length", NULL};
            const char* const* required = NULL;
            switch (cp->type) {
              case CONV_TYPE_VALUES:
                required = req_values;
                break;
              case CONV_TYPE_WAV:
              case CONV_TYPE_RAW:
                required = req_filename;
                break;
              case CONV_TYPE_DUMMY:
                required = req_length;
                break;
              default:
                break;
            }
            if (required &&
                require_json_fields(params, required, "Conv filter parameters",
                                    variant, err) != 0)
              return -1;
          }
          if (cp->type == CONV_TYPE_RAW) {
            static const char* const allowed_raw[] = {
                "type", "filename", "format", "skip_bytes_lines",
                "read_bytes_lines", NULL};
            if (validate_unknown_fields(params, allowed_raw,
                                        "Conv Raw filter parameters",
                                        err) != 0)
              return -1;
          } else if (cp->type == CONV_TYPE_WAV) {
            static const char* const allowed_wav[] = {
                "type", "filename", "channel", NULL};
            if (validate_unknown_fields(params, allowed_wav,
                                        "Conv Wav filter parameters",
                                        err) != 0)
              return -1;
          } else if (cp->type == CONV_TYPE_VALUES) {
            static const char* const allowed_val[] = {"type", "values", NULL};
            if (validate_unknown_fields(params, allowed_val,
                                        "Conv Values filter parameters",
                                        err) != 0)
              return -1;
          } else if (cp->type == CONV_TYPE_DUMMY) {
            static const char* const allowed_dum[] = {"type", "length", NULL};
            if (validate_unknown_fields(params, allowed_dum,
                                        "Conv Dummy filter parameters",
                                        err) != 0)
              return -1;
          }
          cp->values = parse_double_array(
              cJSON_GetObjectItemCaseSensitive(params, "values"),
              &cp->values_count);
          parse_json_str(params, "filename", cp->filename,
                         sizeof(cp->filename));
          parse_json_str(params, "format", cp->format, sizeof(cp->format));
          if (strlen(cp->format) == 0) {
            strncpy(cp->format, "TEXT", sizeof(cp->format) - 1);
          } else if (strcmp(cp->format, "TEXT") != 0 &&
                     file_sample_format_from_string(cp->format) ==
                         BINARY_SAMPLE_FORMAT_INVALID) {
            config_error_set(err, CONFIG_ERR_PARSE,
                             "unknown sample format '%s' for Conv filter",
                             cp->format);
            return -1;
          }
          size_t cval = 0;
          bool cpresent = false;
          if (parse_json_size_t_strict(params, "channel", "Conv filter", &cval,
                                       &cpresent, err) != 0)
            return -1;
          if (cpresent) cp->channel = (int)cval;

          if (parse_json_size_t_strict(params, "length", "Conv filter", &cval,
                                       &cpresent, err) != 0)
            return -1;
          if (cpresent) {
            cp->length = (int)cval;
            if (cp->type == CONV_TYPE_DUMMY && cp->length <= 0) {
              config_error_set(err, CONFIG_ERR_PARSE,
                               "field 'length' in Conv Dummy filter must be "
                               "greater than 0");
              return -1;
            }
          }

          if (parse_json_size_t_strict(params, "skip_bytes_lines",
                                       "Conv filter", &cval, &cpresent,
                                       err) != 0)
            return -1;
          if (cpresent) cp->skip_bytes_lines = (int)cval;

          if (parse_json_size_t_strict(params, "read_bytes_lines",
                                       "Conv filter", &cval, &cpresent,
                                       err) != 0)
            return -1;
          if (cpresent) cp->read_bytes_lines = (int)cval;
          break;
        }
        case FILTER_TYPE_BIQUAD_COMBO: {
          static const char* const allowed[] = {"type",     "freq",  "order",
                                                "gain",     "bands", "freq_min",
                                                "freq_max", "gains", NULL};
          if (validate_unknown_fields(
                  params, allowed, "BiquadCombo filter parameters", err) != 0)
            return -1;
          biquad_combo_config_t* bcp = &f_conf->parameters.biquad_combo;
          static const config_enum_variant_t combo_types[] = {
              {"ButterworthHighpass", BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS},
              {"ButterworthLowpass", BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS},
              {"LinkwitzRileyHighpass",
               BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS},
              {"LinkwitzRileyLowpass",
               BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS},
              {"Tilt", BIQUAD_COMBO_TYPE_TILT},
              {"NPointPeq", BIQUAD_COMBO_TYPE_N_POINT_PEQ},
              {"GraphicEqualizer", BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER},
              {NULL, 0}};
          int combo_type = 0;
          if (parse_enum_required(params, "type", combo_types,
                                  "BiquadCombo filter parameters", &combo_type,
                                  err) != 0)
            return -1;
          bcp->type = (biquad_combo_type_t)combo_type;
          if (bcp->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS ||
              bcp->type == BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS ||
              bcp->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS ||
              bcp->type == BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS) {
            static const char* const allowed_crossover[] = {
                "type", "freq", "order", NULL};
            if (validate_unknown_fields(params, allowed_crossover,
                                        "BiquadCombo filter parameters",
                                        err) != 0)
              return -1;
          } else if (bcp->type == BIQUAD_COMBO_TYPE_TILT) {
            static const char* const allowed_tilt[] = {"type", "gain", NULL};
            if (validate_unknown_fields(params, allowed_tilt,
                                        "BiquadCombo Tilt parameters",
                                        err) != 0)
              return -1;
          } else if (bcp->type == BIQUAD_COMBO_TYPE_N_POINT_PEQ) {
            static const char* const allowed_peq[] = {"type", "bands", NULL};
            if (validate_unknown_fields(params, allowed_peq,
                                        "BiquadCombo NPointPeq parameters",
                                        err) != 0)
              return -1;
          } else if (bcp->type == BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER) {
            static const char* const allowed_geq[] = {
                "type", "gains", "freq_min", "freq_max", NULL};
            if (validate_unknown_fields(params, allowed_geq,
                                        "BiquadCombo GraphicEqualizer parameters",
                                        err) != 0)
              return -1;
          }
          {
            const char* variant =
                cJSON_GetObjectItemCaseSensitive(params, "type")->valuestring;
            static const char* const req_freq_order[] = {"freq", "order", NULL};
            static const char* const req_gain[] = {"gain", NULL};
            static const char* const req_bands[] = {"bands", NULL};
            static const char* const req_gains[] = {"gains", NULL};
            const char* const* required = NULL;
            switch (bcp->type) {
              case BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS:
              case BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS:
              case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS:
              case BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS:
                required = req_freq_order;
                break;
              case BIQUAD_COMBO_TYPE_TILT:
                required = req_gain;
                break;
              case BIQUAD_COMBO_TYPE_N_POINT_PEQ:
                required = req_bands;
                break;
              case BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER:
                required = req_gains;
                break;
              default:
                break;
            }
            if (required && require_json_fields(params, required,
                                                "BiquadCombo filter parameters",
                                                variant, err) != 0)
              return -1;
          }
          bcp->has_freq = parse_json_double(params, "freq", &bcp->freq);
          bcp->has_freq_min =
              parse_json_double(params, "freq_min", &bcp->freq_min);
          bcp->has_freq_max =
              parse_json_double(params, "freq_max", &bcp->freq_max);
          if (bcp->type == BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER) {
            if (!bcp->has_freq_min) {
              bcp->freq_min = 20.0;
              bcp->has_freq_min = true;
            }
            if (!bcp->has_freq_max) {
              bcp->freq_max = 20000.0;
              bcp->has_freq_max = true;
            }
          }
          size_t order_val = 0;
          if (parse_json_size_t_strict(params, "order",
                                       "BiquadCombo filter parameters",
                                       &order_val, &bcp->has_order, err) != 0)
            return -1;
          if (bcp->has_order) bcp->order = (int)order_val;
          bcp->has_gain = parse_json_double(params, "gain", &bcp->gain);

          cJSON* bands_arr = cJSON_GetObjectItemCaseSensitive(params, "bands");
          if (cJSON_IsArray(bands_arr)) {
            int n_bands = cJSON_GetArraySize(bands_arr);
            if (n_bands > 0) {
              bcp->bands =
                  (peq_band_t*)calloc((size_t)n_bands, sizeof(peq_band_t));
              if (bcp->bands) {
                bcp->bands_count = (size_t)n_bands;
                for (int b_idx = 0; b_idx < n_bands; b_idx++) {
                  cJSON* band_obj = cJSON_GetArrayItem(bands_arr, b_idx);
                  // Upstream's PeqBand has no defaults: every band must
                  // carry all three fields.
                  static const char* const req_band[] = {"freq", "q", "gain",
                                                         NULL};
                  if (!cJSON_IsObject(band_obj)) {
                    config_error_set(err, CONFIG_ERR_PARSE,
                                     "band %d in BiquadCombo filter "
                                     "parameters must be an object",
                                     b_idx);
                    return -1;
                  }
                  if (require_json_fields(band_obj, req_band,
                                          "BiquadCombo filter parameters band",
                                          NULL, err) != 0)
                    return -1;
                  parse_json_double(band_obj, "freq", &bcp->bands[b_idx].freq);
                  parse_json_double(band_obj, "q", &bcp->bands[b_idx].q);
                  parse_json_double(band_obj, "gain", &bcp->bands[b_idx].gain);
                }
              }
            }
          }

          cJSON* gains_arr = cJSON_GetObjectItemCaseSensitive(params, "gains");
          bcp->gains = parse_double_array(gains_arr, &bcp->gains_count);
          break;
        }
        case FILTER_TYPE_DIFF_EQ: {
          static const char* const allowed[] = {"a", "b", NULL};
          if (validate_unknown_fields(params, allowed,
                                      "DiffEq filter parameters", err) != 0)
            return -1;
          diffeq_config_t* dep = &f_conf->parameters.diff_eq;
          cJSON* a_arr = cJSON_GetObjectItemCaseSensitive(params, "a");
          dep->a = parse_double_array(a_arr, &dep->a_count);
          cJSON* b_arr = cJSON_GetObjectItemCaseSensitive(params, "b");
          dep->b = parse_double_array(b_arr, &dep->b_count);
          break;
        }
        case FILTER_TYPE_DITHER: {
          static const char* const allowed[] = {"type", "bits", "amplitude",
                                                NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Dither filter parameters", err) != 0)
            return -1;
          dither_config_t* dp = &f_conf->parameters.dither;
          static const config_enum_variant_t dither_types[] = {
              {"None", DITHER_TYPE_NONE},
              {"Flat", DITHER_TYPE_FLAT},
              {"Highpass", DITHER_TYPE_HIGHPASS},
              {"Fweighted441", DITHER_TYPE_FWEIGHTED_441},
              {"FweightedLong441", DITHER_TYPE_FWEIGHTED_LONG_441},
              {"FweightedShort441", DITHER_TYPE_FWEIGHTED_SHORT_441},
              {"Gesemann441", DITHER_TYPE_GESEMANN_441},
              {"Gesemann48", DITHER_TYPE_GESEMANN_48},
              {"Lipshitz441", DITHER_TYPE_LIPSHITZ_441},
              {"LipshitzLong441", DITHER_TYPE_LIPSHITZ_LONG_441},
              {"Shibata441", DITHER_TYPE_SHIBATA_441},
              {"ShibataHigh441", DITHER_TYPE_SHIBATA_HIGH_441},
              {"ShibataLow441", DITHER_TYPE_SHIBATA_LOW_441},
              {"Shibata48", DITHER_TYPE_SHIBATA_48},
              {"ShibataHigh48", DITHER_TYPE_SHIBATA_HIGH_48},
              {"ShibataLow48", DITHER_TYPE_SHIBATA_LOW_48},
              {"Shibata882", DITHER_TYPE_SHIBATA_882},
              {"ShibataLow882", DITHER_TYPE_SHIBATA_LOW_882},
              {"Shibata96", DITHER_TYPE_SHIBATA_96},
              {"ShibataLow96", DITHER_TYPE_SHIBATA_LOW_96},
              {"Shibata192", DITHER_TYPE_SHIBATA_192},
              {"ShibataLow192", DITHER_TYPE_SHIBATA_LOW_192},
              {NULL, 0}};
          int dither_type = 0;
          if (parse_enum_required(params, "type", dither_types,
                                  "Dither filter parameters", &dither_type,
                                  err) != 0)
            return -1;
          dp->type = (dither_type_t)dither_type;
          {
            // Every upstream DitherParameters variant declares `bits`, and the
            // Flat variant also declares `amplitude`; neither has a default.
            const char* variant =
                cJSON_GetObjectItemCaseSensitive(params, "type")->valuestring;
            static const char* const req_bits[] = {"bits", NULL};
            static const char* const req_flat[] = {"bits", "amplitude", NULL};
            const char* const* required =
                dp->type == DITHER_TYPE_FLAT ? req_flat : req_bits;
            if (require_json_fields(params, required,
                                    "Dither filter parameters", variant,
                                    err) != 0)
              return -1;
          }
          if (dp->type != DITHER_TYPE_FLAT) {
            static const char* const allowed_nonflat[] = {"type", "bits", NULL};
            if (validate_unknown_fields(params, allowed_nonflat,
                                        "Dither filter parameters", err) != 0)
              return -1;
          }
          size_t bits_val = 0;
          bool bits_present = false;
          if (parse_json_size_t_strict(params, "bits",
                                       "Dither filter parameters", &bits_val,
                                       &bits_present, err) != 0)
            return -1;
          if (bits_present) dp->bits = (int)bits_val;
          dp->has_amplitude =
              parse_json_double(params, "amplitude", &dp->amplitude);
          break;
        }
        case FILTER_TYPE_CLIPPER: {
          static const char* const allowed[] = {"clip_limit", "soft_clip",
                                                NULL};
          if (validate_unknown_fields(params, allowed,
                                      "Clipper filter parameters", err) != 0)
            return -1;
          clipper_config_t* lp = &f_conf->parameters.clipper;
          parse_json_double(params, "clip_limit", &lp->clip_limit);
          parse_json_bool(params, "soft_clip", &lp->soft_clip);
          break;
        }
        case FILTER_TYPE_LOOKAHEAD_LIMITER: {
          static const char* const allowed[] = {"limit",        "attack",
                                                "release",      "attack_unit",
                                                "release_unit", NULL};
          if (validate_unknown_fields(params, allowed,
                                      "LookaheadLimiter filter parameters",
                                      err) != 0)
            return -1;
          static const char* const req_lim[] = {
              "attack", "release", "attack_unit", "release_unit", NULL};
          if (require_json_fields(params, req_lim,
                                  "LookaheadLimiter filter parameters", NULL,
                                  err) != 0)
            return -1;
          lookahead_limiter_config_t* llp =
              &f_conf->parameters.lookahead_limiter;
          parse_json_double(params, "limit", &llp->limit);
          if (!parse_json_double(params, "attack", &llp->attack)) {
            config_error_set(
                err, CONFIG_ERR_PARSE,
                "field 'attack' in LookaheadLimiter filter parameters must be a number");
            return -1;
          }
          if (!parse_json_double(params, "release", &llp->release)) {
            config_error_set(
                err, CONFIG_ERR_PARSE,
                "field 'release' in LookaheadLimiter filter parameters must be a number");
            return -1;
          }
          char a_unit_buf[64], r_unit_buf[64];
          if (parse_json_str(params, "attack_unit", a_unit_buf,
                             sizeof(a_unit_buf))) {
            if (strcmp(a_unit_buf, "us") == 0)
              llp->attack_unit = TIME_UNIT_US;
            else if (strcmp(a_unit_buf, "ms") == 0)
              llp->attack_unit = TIME_UNIT_MS;
            else if (strcmp(a_unit_buf, "s") == 0)
              llp->attack_unit = TIME_UNIT_S;
            else if (strcmp(a_unit_buf, "samples") == 0)
              llp->attack_unit = TIME_UNIT_SAMPLES;
            else {
              config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                               "Filter '%s': invalid attack_unit '%s'",
                               nf->name, a_unit_buf);
              return -1;
            }
          } else {
            config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                             "Filter '%s': missing required 'attack_unit'",
                             nf->name);
            return -1;
          }

          if (parse_json_str(params, "release_unit", r_unit_buf,
                             sizeof(r_unit_buf))) {
            if (strcmp(r_unit_buf, "us") == 0)
              llp->release_unit = TIME_UNIT_US;
            else if (strcmp(r_unit_buf, "ms") == 0)
              llp->release_unit = TIME_UNIT_MS;
            else if (strcmp(r_unit_buf, "s") == 0)
              llp->release_unit = TIME_UNIT_S;
            else if (strcmp(r_unit_buf, "samples") == 0)
              llp->release_unit = TIME_UNIT_SAMPLES;
            else {
              config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                               "Filter '%s': invalid release_unit '%s'",
                               nf->name, r_unit_buf);
              return -1;
            }
          } else {
            config_error_set(err, CONFIG_ERR_INVALID_FILTER,
                             "Filter '%s': missing required 'release_unit'",
                             nf->name);
            return -1;
          }
          break;
        }
        default:
          break;
      }
    }
    f++;
  }
  return 0;
}

int config_parse_processors(const cJSON* processors_obj, dsp_config_t* config,
                            config_error_t* err) {
  if (!cJSON_IsObject(processors_obj)) {
    config_error_set(err, CONFIG_ERR_PARSE, "processors must be an object");
    return -1;
  }
  int size = 0;
  cJSON* proc_child = NULL;
  cJSON_ArrayForEach(proc_child, processors_obj) { size++; }
  if (size == 0) return 0;

  config->processors =
      (named_processor_config_t*)calloc(size, sizeof(named_processor_config_t));
  if (!config->processors) {
    config_error_set(err, CONFIG_ERR_PARSE, "Memory allocation failure");
    return -1;
  }
  config->processors_count = size;

  int p = 0;
  cJSON_ArrayForEach(proc_child, processors_obj) {
    named_processor_config_t* np = &config->processors[p];
    const char* p_name = proc_child->string ? proc_child->string : "";
    strncpy(np->name, p_name, sizeof(np->name) - 1);
    np->name[sizeof(np->name) - 1] = '\0';

    if (!cJSON_IsObject(proc_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor definition must be an object");
      return -1;
    }

    static const char* const allowed_proc_keys[] = {"type", "description",
                                                    "parameters", NULL};
    if (validate_unknown_fields(proc_child, allowed_proc_keys,
                                "processor definition", err) != 0) {
      return -1;
    }

    processor_config_t* p_conf = &np->processor;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(proc_child, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor '%s': missing or non-string 'type'",
                       np->name);
      return -1;
    }
    p_conf->type = processor_type_from_string(type->valuestring);
    if (p_conf->type == PROCESSOR_TYPE_INVALID) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor '%s': unknown processor type '%s'", np->name,
                       type->valuestring);
      return -1;
    }

    cJSON* params = cJSON_GetObjectItemCaseSensitive(proc_child, "parameters");
    if (!cJSON_IsObject(params)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor '%s': missing 'parameters' object", np->name);
      return -1;
    }
    switch (p_conf->type) {
      case PROCESSOR_TYPE_COMPRESSOR: {
        static const char* const allowed[] = {
            "channels",    "monitor_channels", "process_channels", "attack",
            "attack_unit", "release",          "release_unit",     "threshold",
            "factor",      "makeup_gain",      "soft_clip",        "clip_limit",
            NULL};
        if (validate_unknown_fields(
                params, allowed, "Compressor processor parameters", err) != 0)
          return -1;
        static const char* const req_comp[] = {
            "channels",     "attack",    "attack_unit", "release",
            "release_unit", "threshold", "factor",      NULL};
        if (require_json_fields(params, req_comp,
                                "Compressor processor parameters", NULL,
                                err) != 0)
          return -1;
        compressor_config_t* cp = &p_conf->parameters.compressor;
        if (parse_json_size_t_strict(params, "channels",
                                     "Compressor processor parameters",
                                     &cp->channels, NULL, err) != 0)
          return -1;
        parse_json_double(params, "attack", &cp->attack);
        parse_json_double(params, "release", &cp->release);
        parse_json_double(params, "threshold", &cp->threshold);
        parse_json_double(params, "factor", &cp->factor);
        cp->has_makeup_gain =
            parse_json_double(params, "makeup_gain", &cp->makeup_gain);
        parse_json_bool(params, "soft_clip", &cp->soft_clip);
        cp->has_clip_limit =
            parse_json_double(params, "clip_limit", &cp->clip_limit);

        char a_unit_buf[64], r_unit_buf[64];
        if (parse_json_str(params, "attack_unit", a_unit_buf,
                           sizeof(a_unit_buf))) {
          if (strcmp(a_unit_buf, "us") == 0)
            cp->attack_unit = TIME_UNIT_US;
          else if (strcmp(a_unit_buf, "ms") == 0)
            cp->attack_unit = TIME_UNIT_MS;
          else if (strcmp(a_unit_buf, "s") == 0)
            cp->attack_unit = TIME_UNIT_S;
          else if (strcmp(a_unit_buf, "samples") == 0)
            cp->attack_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid attack_unit '%s'",
                             np->name, a_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'attack_unit'",
                           np->name);
          return -1;
        }

        if (parse_json_str(params, "release_unit", r_unit_buf,
                           sizeof(r_unit_buf))) {
          if (strcmp(r_unit_buf, "us") == 0)
            cp->release_unit = TIME_UNIT_US;
          else if (strcmp(r_unit_buf, "ms") == 0)
            cp->release_unit = TIME_UNIT_MS;
          else if (strcmp(r_unit_buf, "s") == 0)
            cp->release_unit = TIME_UNIT_S;
          else if (strcmp(r_unit_buf, "samples") == 0)
            cp->release_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid release_unit '%s'",
                             np->name, r_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'release_unit'",
                           np->name);
          return -1;
        }

        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "monitor_channels"),
                "monitor_channels", "Compressor processor parameters",
                &cp->monitor_channels, &cp->monitor_channels_count, err) != 0)
          return -1;
        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "process_channels"),
                "process_channels", "Compressor processor parameters",
                &cp->process_channels, &cp->process_channels_count, err) != 0)
          return -1;
        break;
      }
      case PROCESSOR_TYPE_NOISE_GATE: {
        static const char* const allowed[] = {
            "channels",         "monitor_channels",
            "process_channels", "attack",
            "attack_unit",      "release",
            "release_unit",     "threshold",
            "attenuation",      NULL};
        if (validate_unknown_fields(params, allowed,
                                    "NoiseGate processor parameters", err) != 0)
          return -1;
        static const char* const req_gate[] = {
            "channels",     "attack",    "attack_unit", "release",
            "release_unit", "threshold", "attenuation", NULL};
        if (require_json_fields(params, req_gate,
                                "NoiseGate processor parameters", NULL,
                                err) != 0)
          return -1;
        noise_gate_config_t* ng = &p_conf->parameters.noise_gate;
        if (parse_json_size_t_strict(params, "channels",
                                     "NoiseGate processor parameters",
                                     &ng->channels, NULL, err) != 0)
          return -1;
        parse_json_double(params, "attack", &ng->attack);
        parse_json_double(params, "release", &ng->release);
        parse_json_double(params, "threshold", &ng->threshold);
        parse_json_double(params, "attenuation", &ng->attenuation);

        char a_unit_buf[64], r_unit_buf[64];
        if (parse_json_str(params, "attack_unit", a_unit_buf,
                           sizeof(a_unit_buf))) {
          if (strcmp(a_unit_buf, "us") == 0)
            ng->attack_unit = TIME_UNIT_US;
          else if (strcmp(a_unit_buf, "ms") == 0)
            ng->attack_unit = TIME_UNIT_MS;
          else if (strcmp(a_unit_buf, "s") == 0)
            ng->attack_unit = TIME_UNIT_S;
          else if (strcmp(a_unit_buf, "samples") == 0)
            ng->attack_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid attack_unit '%s'",
                             np->name, a_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'attack_unit'",
                           np->name);
          return -1;
        }

        if (parse_json_str(params, "release_unit", r_unit_buf,
                           sizeof(r_unit_buf))) {
          if (strcmp(r_unit_buf, "us") == 0)
            ng->release_unit = TIME_UNIT_US;
          else if (strcmp(r_unit_buf, "ms") == 0)
            ng->release_unit = TIME_UNIT_MS;
          else if (strcmp(r_unit_buf, "s") == 0)
            ng->release_unit = TIME_UNIT_S;
          else if (strcmp(r_unit_buf, "samples") == 0)
            ng->release_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid release_unit '%s'",
                             np->name, r_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'release_unit'",
                           np->name);
          return -1;
        }

        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "monitor_channels"),
                "monitor_channels", "NoiseGate processor parameters",
                &ng->monitor_channels, &ng->monitor_channels_count, err) != 0)
          return -1;
        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "process_channels"),
                "process_channels", "NoiseGate processor parameters",
                &ng->process_channels, &ng->process_channels_count, err) != 0)
          return -1;
        break;
      }
      case PROCESSOR_TYPE_RACE: {
        static const char* const allowed[] = {
            "channels",        "channel_a",  "channel_b",   "delay",
            "subsample_delay", "delay_unit", "attenuation", NULL};
        if (validate_unknown_fields(params, allowed,
                                    "RACE processor parameters", err) != 0)
          return -1;
        static const char* const req_race[] = {
            "channels",   "channel_a",   "channel_b", "delay",
            "delay_unit", "attenuation", NULL};
        if (require_json_fields(params, req_race, "RACE processor parameters",
                                NULL, err) != 0)
          return -1;
        race_config_t* rp = &p_conf->parameters.race;
        parse_json_double(params, "attenuation", &rp->attenuation);
        parse_json_double(params, "delay", &rp->delay);
        if (parse_json_size_t_strict(params, "channels",
                                     "RACE processor parameters", &rp->channels,
                                     NULL, err) != 0 ||
            parse_json_size_t_strict(params, "channel_a",
                                     "RACE processor parameters",
                                     &rp->channel_a, NULL, err) != 0 ||
            parse_json_size_t_strict(params, "channel_b",
                                     "RACE processor parameters",
                                     &rp->channel_b, NULL, err) != 0)
          return -1;
        rp->has_subsample_delay =
            parse_json_bool(params, "subsample_delay", &rp->subsample_delay);

        char unit_buf[64];
        if (parse_json_str(params, "delay_unit", unit_buf, sizeof(unit_buf))) {
          if (strcmp(unit_buf, "us") == 0)
            rp->delay_unit = DELAY_UNIT_US;
          else if (strcmp(unit_buf, "ms") == 0)
            rp->delay_unit = DELAY_UNIT_MS;
          else if (strcmp(unit_buf, "s") == 0)
            rp->delay_unit = DELAY_UNIT_S;
          else if (strcmp(unit_buf, "samples") == 0)
            rp->delay_unit = DELAY_UNIT_SAMPLES;
          else if (strcmp(unit_buf, "mm") == 0)
            rp->delay_unit = DELAY_UNIT_MM;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid delay_unit '%s'",
                             np->name, unit_buf);
            return -1;
          }
          rp->has_delay_unit = true;
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'delay_unit'",
                           np->name);
          return -1;
        }
        break;
      }
      case PROCESSOR_TYPE_LOOKAHEAD_LIMITER: {
        static const char* const allowed[] = {
            "channels", "monitor_channels", "process_channels",
            "limit",    "attack",           "attack_unit",
            "release",  "release_unit",     "delay_processed_only",
            NULL};
        if (validate_unknown_fields(params, allowed,
                                    "LookaheadLimiter processor parameters",
                                    err) != 0)
          return -1;
        static const char* const req_lim[] = {"channels",     "attack",
                                              "attack_unit",  "release",
                                              "release_unit", NULL};
        if (require_json_fields(params, req_lim,
                                "LookaheadLimiter processor parameters", NULL,
                                err) != 0)
          return -1;
        lookahead_limiter_processor_config_t* lp =
            &p_conf->parameters.lookahead_limiter;
        if (parse_json_size_t_strict(params, "channels",
                                     "LookaheadLimiter processor parameters",
                                     &lp->channels, NULL, err) != 0)
          return -1;
        parse_json_double(params, "limit",
                          &lp->limit);  // Default double is fine
        parse_json_double(params, "attack", &lp->attack);
        parse_json_double(params, "release", &lp->release);
        lp->delay_processed_only = false;
        parse_json_bool(params, "delay_processed_only",
                        &lp->delay_processed_only);

        char a_unit_buf[64], r_unit_buf[64];
        if (parse_json_str(params, "attack_unit", a_unit_buf,
                           sizeof(a_unit_buf))) {
          if (strcmp(a_unit_buf, "us") == 0)
            lp->attack_unit = TIME_UNIT_US;
          else if (strcmp(a_unit_buf, "ms") == 0)
            lp->attack_unit = TIME_UNIT_MS;
          else if (strcmp(a_unit_buf, "s") == 0)
            lp->attack_unit = TIME_UNIT_S;
          else if (strcmp(a_unit_buf, "samples") == 0)
            lp->attack_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid attack_unit '%s'",
                             np->name, a_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'attack_unit'",
                           np->name);
          return -1;
        }

        if (parse_json_str(params, "release_unit", r_unit_buf,
                           sizeof(r_unit_buf))) {
          if (strcmp(r_unit_buf, "us") == 0)
            lp->release_unit = TIME_UNIT_US;
          else if (strcmp(r_unit_buf, "ms") == 0)
            lp->release_unit = TIME_UNIT_MS;
          else if (strcmp(r_unit_buf, "s") == 0)
            lp->release_unit = TIME_UNIT_S;
          else if (strcmp(r_unit_buf, "samples") == 0)
            lp->release_unit = TIME_UNIT_SAMPLES;
          else {
            config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                             "Processor '%s': invalid release_unit '%s'",
                             np->name, r_unit_buf);
            return -1;
          }
        } else {
          config_error_set(err, CONFIG_ERR_INVALID_PROCESSOR,
                           "Processor '%s': missing required 'release_unit'",
                           np->name);
          return -1;
        }

        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "monitor_channels"),
                "monitor_channels", "LookaheadLimiter processor parameters",
                &lp->monitor_channels, &lp->monitor_channels_count, err) != 0)
          return -1;
        if (parse_size_t_array_strict(
                cJSON_GetObjectItemCaseSensitive(params, "process_channels"),
                "process_channels", "LookaheadLimiter processor parameters",
                &lp->process_channels, &lp->process_channels_count, err) != 0)
          return -1;
        break;
      }
      default:
        break;
    }
    p++;
  }
  return 0;
}
