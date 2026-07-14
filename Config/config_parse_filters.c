#include "config_parse_filters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Logging/app_logger.h"
#include "cJSON.h"
#include "config_parser_internal.h"
#include "configuration.h"

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
    strncpy(nf->name, filter_child->string, sizeof(nf->name) - 1);

    if (!cJSON_IsObject(filter_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Filter definition must be an object");
      return -1;
    }

    filter_config_t* f_conf = &nf->filter;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(filter_child, "type");
    if (cJSON_IsString(type) && type->valuestring) {
      f_conf->type = filter_type_from_string(type->valuestring);
    }

    cJSON* params =
        cJSON_GetObjectItemCaseSensitive(filter_child, "parameters");
    if (cJSON_IsObject(params)) {
      cJSON* item;
      switch (f_conf->type) {
        case FILTER_TYPE_GAIN: {
          gain_parameters_t* gp = &f_conf->parameters.gain;
          gp->has_gain = parse_json_double(params, "gain", &gp->gain);
          char str_buf[64];
          if (parse_json_str(params, "scale", str_buf, sizeof(str_buf))) {
            gp->scale = (strcasecmp(str_buf, "Linear") == 0) ? GAIN_SCALE_LINEAR : GAIN_SCALE_DB;
          } else {
            gp->scale = GAIN_SCALE_DB;
          }
          parse_json_bool(params, "inverted", &gp->inverted);
          parse_json_bool(params, "mute", &gp->mute);
          break;
        }
        case FILTER_TYPE_VOLUME: {
          volume_parameters_t* vp = &f_conf->parameters.volume;
          vp->has_ramp_time = parse_json_double(params, "ramp_time", &vp->ramp_time);
          vp->has_limit = parse_json_double(params, "limit", &vp->limit);
          item = cJSON_GetObjectItemCaseSensitive(params, "fader");
          if (item) {
            if (cJSON_IsString(item) && item->valuestring) {
              vp->fader = fader_from_string(item->valuestring);
            } else if (cJSON_IsNumber(item)) {
              vp->fader = (fader_t)item->valueint;
            }
          } else {
            vp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_LOUDNESS: {
          loudness_parameters_t* lp = &f_conf->parameters.loudness;
          lp->has_reference_level = parse_json_double(params, "reference_level", &lp->reference_level);
          lp->has_high_boost = parse_json_double(params, "high_boost", &lp->high_boost);
          lp->has_low_boost = parse_json_double(params, "low_boost", &lp->low_boost);
          parse_json_bool(params, "attenuate_mid", &lp->attenuate_mid);
          item = cJSON_GetObjectItemCaseSensitive(params, "fader");
          if (item) {
            if (cJSON_IsString(item) && item->valuestring) {
              lp->fader = fader_from_string(item->valuestring);
            } else if (cJSON_IsNumber(item)) {
              lp->fader = (fader_t)item->valueint;
            }
          } else {
            lp->fader = FADER_MAIN;
          }
          break;
        }
        case FILTER_TYPE_BIQUAD: {
          biquad_parameters_t* bp = &f_conf->parameters.biquad;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "Free") == 0)
              bp->type = BIQUAD_TYPE_FREE;
            else if (strcmp(item->valuestring, "Highpass") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS;
            else if (strcmp(item->valuestring, "Lowpass") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS;
            else if (strcmp(item->valuestring, "HighpassFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHPASS_FO;
            else if (strcmp(item->valuestring, "LowpassFO") == 0)
              bp->type = BIQUAD_TYPE_LOWPASS_FO;
            else if (strcmp(item->valuestring, "Highshelf") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF;
            else if (strcmp(item->valuestring, "Lowshelf") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF;
            else if (strcmp(item->valuestring, "HighshelfFO") == 0)
              bp->type = BIQUAD_TYPE_HIGHSHELF_FO;
            else if (strcmp(item->valuestring, "LowshelfFO") == 0)
              bp->type = BIQUAD_TYPE_LOWSHELF_FO;
            else if (strcmp(item->valuestring, "Peaking") == 0)
              bp->type = BIQUAD_TYPE_PEAKING;
            else if (strcmp(item->valuestring, "Notch") == 0)
              bp->type = BIQUAD_TYPE_NOTCH;
            else if (strcmp(item->valuestring, "Bandpass") == 0)
              bp->type = BIQUAD_TYPE_BANDPASS;
            else if (strcmp(item->valuestring, "Allpass") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS;
            else if (strcmp(item->valuestring, "AllpassFO") == 0)
              bp->type = BIQUAD_TYPE_ALLPASS_FO;
            else if (strcmp(item->valuestring, "GeneralNotch") == 0)
              bp->type = BIQUAD_TYPE_GENERAL_NOTCH;
            else if (strcmp(item->valuestring, "LinkwitzTransform") == 0)
              bp->type = BIQUAD_TYPE_LINKWITZ_TRANSFORM;
          }
          parse_json_double(params, "freq", &bp->freq);
          parse_json_double(params, "gain", &bp->gain);

          if (parse_json_double(params, "q", &bp->q)) {
            bp->steepness_type = STEEPNESS_TYPE_Q;
          }
          if (parse_json_double(params, "bandwidth", &bp->bandwidth)) {
            bp->steepness_type = STEEPNESS_TYPE_BANDWIDTH;
          }
          if (parse_json_double(params, "slope", &bp->slope)) {
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
          delay_parameters_t* dp = &f_conf->parameters.delay;
          parse_json_double(params, "delay", &dp->delay);
          char unit_buf[64];
          if (parse_json_str(params, "unit", unit_buf, sizeof(unit_buf))) {
            if (strcmp(unit_buf, "us") == 0) dp->unit = DELAY_UNIT_US;
            else if (strcmp(unit_buf, "samples") == 0) dp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(unit_buf, "mm") == 0) dp->unit = DELAY_UNIT_MM;
            else dp->unit = DELAY_UNIT_MS;
          } else {
            dp->unit = DELAY_UNIT_MS;
          }
          parse_json_bool(params, "subsample", &dp->subsample);
          break;
        }
        case FILTER_TYPE_CONV: {
          conv_parameters_t* cp = &f_conf->parameters.conv;
          char type_buf[64];
          if (parse_json_str(params, "type", type_buf, sizeof(type_buf))) {
            if (strcmp(type_buf, "Values") == 0) cp->type = CONV_TYPE_VALUES;
            else if (strcmp(type_buf, "Wav") == 0) cp->type = CONV_TYPE_WAV;
            else if (strcmp(type_buf, "Raw") == 0) cp->type = CONV_TYPE_RAW;
            else cp->type = CONV_TYPE_DUMMY;
          }
          cp->values = parse_double_array(cJSON_GetObjectItemCaseSensitive(params, "values"), &cp->values_count);
          parse_json_str(params, "filename", cp->filename, sizeof(cp->filename));
          parse_json_str(params, "format", cp->format, sizeof(cp->format));
          parse_json_int(params, "channel", &cp->channel);
          parse_json_int(params, "length", &cp->length);
          parse_json_int(params, "skip_bytes_lines", &cp->skip_bytes_lines);
          parse_json_int(params, "read_bytes_lines", &cp->read_bytes_lines);
          break;
        }
        case FILTER_TYPE_BIQUAD_COMBO: {
          biquad_combo_parameters_t* bcp = &f_conf->parameters.biquad_combo;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ButterworthHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS;
            else if (strcmp(item->valuestring, "ButterworthLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS;
            else if (strcmp(item->valuestring, "LinkwitzRileyHighpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS;
            else if (strcmp(item->valuestring, "LinkwitzRileyLowpass") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS;
            else if (strcmp(item->valuestring, "Tilt") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_TILT;
            else if (strcmp(item->valuestring, "FivePointPEQ") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_FIVE_POINT_PEQ;
            else if (strcmp(item->valuestring, "GraphicEqualizer") == 0)
              bcp->type = BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER;
          }
          bcp->has_freq = parse_json_double(params, "freq", &bcp->freq);
          bcp->has_freq_min = parse_json_double(params, "freq_min", &bcp->freq_min);
          bcp->has_freq_max = parse_json_double(params, "freq_max", &bcp->freq_max);
          bcp->has_order = parse_json_int(params, "order", &bcp->order);
          bcp->has_gain = parse_json_double(params, "gain", &bcp->gain);
#define PARSE_COMBO_DOUBLE(name, field)                   \
  item = cJSON_GetObjectItemCaseSensitive(params, #name); \
  if (cJSON_IsNumber(item)) {                             \
    bcp->field = item->valuedouble;                       \
    bcp->has_##field = true;                              \
  }
          PARSE_COMBO_DOUBLE(fls, fls)
          PARSE_COMBO_DOUBLE(qls, qls)
          PARSE_COMBO_DOUBLE(gls, gls)
          PARSE_COMBO_DOUBLE(fp1, fp1)
          PARSE_COMBO_DOUBLE(qp1, qp1)
          PARSE_COMBO_DOUBLE(gp1, gp1)
          PARSE_COMBO_DOUBLE(fp2, fp2)
          PARSE_COMBO_DOUBLE(qp2, qp2)
          PARSE_COMBO_DOUBLE(gp2, gp2)
          PARSE_COMBO_DOUBLE(fp3, fp3)
          PARSE_COMBO_DOUBLE(qp3, qp3)
          PARSE_COMBO_DOUBLE(gp3, gp3)
          PARSE_COMBO_DOUBLE(fhs, fhs)
          PARSE_COMBO_DOUBLE(qhs, qhs)
          PARSE_COMBO_DOUBLE(ghs, ghs)
#undef PARSE_COMBO_DOUBLE

          cJSON* gains_arr = cJSON_GetObjectItemCaseSensitive(params, "gains");
          bcp->gains = parse_double_array(gains_arr, &bcp->gains_count);
          break;
        }
        case FILTER_TYPE_DIFF_EQ: {
          diff_eq_parameters_t* dep = &f_conf->parameters.diff_eq;
          cJSON* a_arr = cJSON_GetObjectItemCaseSensitive(params, "a");
          dep->a = parse_double_array(a_arr, &dep->a_count);
          cJSON* b_arr = cJSON_GetObjectItemCaseSensitive(params, "b");
          dep->b = parse_double_array(b_arr, &dep->b_count);
          break;
        }
        case FILTER_TYPE_DITHER: {
          dither_parameters_t* dp = &f_conf->parameters.dither;
          char type_buf[64];
          if (parse_json_str(params, "type", type_buf, sizeof(type_buf))) {
            if (strcmp(type_buf, "None") == 0) dp->type = DITHER_TYPE_NONE;
            else if (strcmp(type_buf, "Flat") == 0) dp->type = DITHER_TYPE_FLAT;
            else if (strcmp(type_buf, "Highpass") == 0) dp->type = DITHER_TYPE_HIGHPASS;
            else if (strcmp(type_buf, "Fweighted441") == 0) dp->type = DITHER_TYPE_FWEIGHTED_441;
            else if (strcmp(type_buf, "FweightedLong441") == 0) dp->type = DITHER_TYPE_FWEIGHTED_LONG_441;
            else if (strcmp(type_buf, "FweightedShort441") == 0) dp->type = DITHER_TYPE_FWEIGHTED_SHORT_441;
            else if (strcmp(type_buf, "Gesemann441") == 0) dp->type = DITHER_TYPE_GESEMANN_441;
            else if (strcmp(type_buf, "Gesemann48") == 0) dp->type = DITHER_TYPE_GESEMANN_48;
            else if (strcmp(type_buf, "Lipshitz441") == 0) dp->type = DITHER_TYPE_LIPSHITZ_441;
            else if (strcmp(type_buf, "LipshitzLong441") == 0) dp->type = DITHER_TYPE_LIPSHITZ_LONG_441;
            else if (strcmp(type_buf, "Shibata441") == 0) dp->type = DITHER_TYPE_SHIBATA_441;
            else if (strcmp(type_buf, "ShibataHigh441") == 0) dp->type = DITHER_TYPE_SHIBATA_HIGH_441;
            else if (strcmp(type_buf, "ShibataLow441") == 0) dp->type = DITHER_TYPE_SHIBATA_LOW_441;
            else if (strcmp(type_buf, "Shibata48") == 0) dp->type = DITHER_TYPE_SHIBATA_48;
            else if (strcmp(type_buf, "ShibataHigh48") == 0) dp->type = DITHER_TYPE_SHIBATA_HIGH_48;
            else if (strcmp(type_buf, "ShibataLow48") == 0) dp->type = DITHER_TYPE_SHIBATA_LOW_48;
            else if (strcmp(type_buf, "Shibata882") == 0) dp->type = DITHER_TYPE_SHIBATA_882;
            else if (strcmp(type_buf, "ShibataLow882") == 0) dp->type = DITHER_TYPE_SHIBATA_LOW_882;
            else if (strcmp(type_buf, "Shibata96") == 0) dp->type = DITHER_TYPE_SHIBATA_96;
            else if (strcmp(type_buf, "ShibataLow96") == 0) dp->type = DITHER_TYPE_SHIBATA_LOW_96;
            else if (strcmp(type_buf, "Shibata192") == 0) dp->type = DITHER_TYPE_SHIBATA_192;
            else if (strcmp(type_buf, "ShibataLow192") == 0) dp->type = DITHER_TYPE_SHIBATA_LOW_192;
          }
          parse_json_int(params, "bits", &dp->bits);
          dp->has_amplitude = parse_json_double(params, "amplitude", &dp->amplitude);
          break;
        }
        case FILTER_TYPE_LIMITER: {
          limiter_parameters_t* lp = &f_conf->parameters.limiter;
          parse_json_double(params, "clip_limit", &lp->clip_limit);
          parse_json_bool(params, "soft_clip", &lp->soft_clip);
          break;
        }
        case FILTER_TYPE_LOOKAHEAD_LIMITER: {
          lookahead_limiter_parameters_t* llp = &f_conf->parameters.lookahead_limiter;
          parse_json_double(params, "limit", &llp->limit);
          parse_json_double(params, "attack", &llp->attack);
          parse_json_double(params, "release", &llp->release);
          char unit_buf[64];
          if (parse_json_str(params, "unit", unit_buf, sizeof(unit_buf))) {
            if (strcmp(unit_buf, "us") == 0) llp->unit = DELAY_UNIT_US;
            else if (strcmp(unit_buf, "samples") == 0) llp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(unit_buf, "mm") == 0) llp->unit = DELAY_UNIT_MM;
            else llp->unit = DELAY_UNIT_MS;
          } else {
            llp->unit = DELAY_UNIT_MS;
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
    strncpy(np->name, proc_child->string, sizeof(np->name) - 1);

    if (!cJSON_IsObject(proc_child)) {
      config_error_set(err, CONFIG_ERR_PARSE,
                       "Processor definition must be an object");
      return -1;
    }

    processor_config_t* p_conf = &np->processor;

    cJSON* type = cJSON_GetObjectItemCaseSensitive(proc_child, "type");
    if (cJSON_IsString(type) && type->valuestring) {
      p_conf->type = processor_type_from_string(type->valuestring);
    }

    cJSON* params = cJSON_GetObjectItemCaseSensitive(proc_child, "parameters");
    if (cJSON_IsObject(params)) {
      cJSON* item;
      switch (p_conf->type) {
        case PROCESSOR_TYPE_COMPRESSOR: {
          compressor_parameters_t* cp = &p_conf->parameters.compressor;
          parse_json_int(params, "channels", &cp->channels);
          parse_json_double(params, "attack", &cp->attack);
          parse_json_double(params, "release", &cp->release);
          parse_json_double(params, "threshold", &cp->threshold);
          parse_json_double(params, "factor", &cp->factor);
          cp->has_makeup_gain = parse_json_double(params, "makeup_gain", &cp->makeup_gain);
          parse_json_bool(params, "soft_clip", &cp->soft_clip);
          cp->has_clip_limit = parse_json_double(params, "clip_limit", &cp->clip_limit);

          cp->monitor_channels = parse_int_array(cJSON_GetObjectItemCaseSensitive(params, "monitor_channels"), &cp->monitor_channels_count);
          cp->process_channels = parse_int_array(cJSON_GetObjectItemCaseSensitive(params, "process_channels"), &cp->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_NOISE_GATE: {
          noise_gate_parameters_t* ng = &p_conf->parameters.noise_gate;
          parse_json_int(params, "channels", &ng->channels);
          parse_json_double(params, "attack", &ng->attack);
          parse_json_double(params, "release", &ng->release);
          parse_json_double(params, "threshold", &ng->threshold);
          parse_json_double(params, "attenuation", &ng->attenuation);

          ng->monitor_channels = parse_int_array(cJSON_GetObjectItemCaseSensitive(params, "monitor_channels"), &ng->monitor_channels_count);
          ng->process_channels = parse_int_array(cJSON_GetObjectItemCaseSensitive(params, "process_channels"), &ng->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_RACE: {
          race_parameters_t* rp = &p_conf->parameters.race;
          parse_json_double(params, "attenuation", &rp->attenuation);
          parse_json_double(params, "delay", &rp->delay);

          char unit_buf[64];
          if (parse_json_str(params, "delay_unit", unit_buf, sizeof(unit_buf))) {
            if (strcmp(unit_buf, "us") == 0) rp->delay_unit = DELAY_UNIT_US;
            else if (strcmp(unit_buf, "samples") == 0) rp->delay_unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(unit_buf, "mm") == 0) rp->delay_unit = DELAY_UNIT_MM;
            else rp->delay_unit = DELAY_UNIT_MS;
            rp->has_delay_unit = true;
          }
          break;
        }
        default:
          break;
      }
    }
    p++;
  }
  return 0;
}
