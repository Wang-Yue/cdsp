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
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) {
            gp->gain = item->valuedouble;
            gp->has_gain = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "scale");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcasecmp(item->valuestring, "Linear") == 0)
              gp->scale = GAIN_SCALE_LINEAR;
            else
              gp->scale = GAIN_SCALE_DB;
          } else {
            gp->scale = GAIN_SCALE_DB;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "inverted");
          if (cJSON_IsBool(item)) {
            gp->inverted = cJSON_IsTrue(item);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "mute");
          if (cJSON_IsBool(item)) {
            gp->mute = cJSON_IsTrue(item);
          }
          break;
        }
        case FILTER_TYPE_VOLUME: {
          volume_parameters_t* vp = &f_conf->parameters.volume;
          item = cJSON_GetObjectItemCaseSensitive(params, "ramp_time");
          if (cJSON_IsNumber(item)) {
            vp->ramp_time = item->valuedouble;
            vp->has_ramp_time = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "limit");
          if (cJSON_IsNumber(item)) {
            vp->limit = item->valuedouble;
            vp->has_limit = true;
          }
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
          item = cJSON_GetObjectItemCaseSensitive(params, "reference_level");
          if (cJSON_IsNumber(item)) {
            lp->reference_level = item->valuedouble;
            lp->has_reference_level = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "high_boost");
          if (cJSON_IsNumber(item)) {
            lp->high_boost = item->valuedouble;
            lp->has_high_boost = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "low_boost");
          if (cJSON_IsNumber(item)) {
            lp->low_boost = item->valuedouble;
            lp->has_low_boost = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "attenuate_mid");
          if (cJSON_IsBool(item)) {
            lp->attenuate_mid = cJSON_IsTrue(item);
          }
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
          item = cJSON_GetObjectItemCaseSensitive(params, "freq");
          if (cJSON_IsNumber(item)) bp->freq = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) bp->gain = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "q");
          if (cJSON_IsNumber(item)) {
            bp->q = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_Q;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "bandwidth");
          if (cJSON_IsNumber(item)) {
            bp->bandwidth = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_BANDWIDTH;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "slope");
          if (cJSON_IsNumber(item)) {
            bp->slope = item->valuedouble;
            bp->steepness_type = STEEPNESS_TYPE_SLOPE;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "a1");
          if (cJSON_IsNumber(item)) bp->a1 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "a2");
          if (cJSON_IsNumber(item)) bp->a2 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b0");
          if (cJSON_IsNumber(item)) bp->b0 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b1");
          if (cJSON_IsNumber(item)) bp->b1 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "b2");
          if (cJSON_IsNumber(item)) bp->b2 = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_z");
          if (cJSON_IsNumber(item)) bp->freq_notch = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_p");
          if (cJSON_IsNumber(item)) bp->freq_pole = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_p");
          if (cJSON_IsNumber(item)) bp->q_p = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "normalize_at_dc");
          if (cJSON_IsBool(item)) bp->normalize_at_dc = cJSON_IsTrue(item);
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_act");
          if (cJSON_IsNumber(item)) bp->freq_act = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_act");
          if (cJSON_IsNumber(item)) bp->q_act = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_target");
          if (cJSON_IsNumber(item)) bp->freq_target = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "q_target");
          if (cJSON_IsNumber(item)) bp->q_target = item->valuedouble;
          break;
        }
        case FILTER_TYPE_DELAY: {
          delay_parameters_t* dp = &f_conf->parameters.delay;
          item = cJSON_GetObjectItemCaseSensitive(params, "delay");
          if (cJSON_IsNumber(item)) dp->delay = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              dp->unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              dp->unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              dp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              dp->unit = DELAY_UNIT_MM;
          } else {
            dp->unit = DELAY_UNIT_MS;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "subsample");
          if (cJSON_IsBool(item)) dp->subsample = cJSON_IsTrue(item);
          break;
        }
        case FILTER_TYPE_CONV: {
          conv_parameters_t* cp = &f_conf->parameters.conv;
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "Values") == 0)
              cp->type = CONV_TYPE_VALUES;
            else if (strcmp(item->valuestring, "Wav") == 0)
              cp->type = CONV_TYPE_WAV;
            else if (strcmp(item->valuestring, "Raw") == 0)
              cp->type = CONV_TYPE_RAW;
            else
              cp->type = CONV_TYPE_DUMMY;
          }
          cJSON* val_arr = cJSON_GetObjectItemCaseSensitive(params, "values");
          cp->values = parse_double_array(val_arr, &cp->values_count);
          item = cJSON_GetObjectItemCaseSensitive(params, "filename");
          if (cJSON_IsString(item) && item->valuestring) {
            strncpy(cp->filename, item->valuestring, sizeof(cp->filename) - 1);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "format");
          if (cJSON_IsString(item) && item->valuestring) {
            strncpy(cp->format, item->valuestring, sizeof(cp->format) - 1);
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "channel");
          if (cJSON_IsNumber(item)) cp->channel = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "length");
          if (cJSON_IsNumber(item)) cp->length = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "skip_bytes_lines");
          if (cJSON_IsNumber(item)) cp->skip_bytes_lines = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "read_bytes_lines");
          if (cJSON_IsNumber(item)) cp->read_bytes_lines = item->valueint;
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
          item = cJSON_GetObjectItemCaseSensitive(params, "freq");
          if (cJSON_IsNumber(item)) {
            bcp->freq = item->valuedouble;
            bcp->has_freq = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_min");
          if (cJSON_IsNumber(item)) {
            bcp->freq_min = item->valuedouble;
            bcp->has_freq_min = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "freq_max");
          if (cJSON_IsNumber(item)) {
            bcp->freq_max = item->valuedouble;
            bcp->has_freq_max = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "order");
          if (cJSON_IsNumber(item)) {
            bcp->order = item->valueint;
            bcp->has_order = true;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "gain");
          if (cJSON_IsNumber(item)) {
            bcp->gain = item->valuedouble;
            bcp->has_gain = true;
          }
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
          item = cJSON_GetObjectItemCaseSensitive(params, "type");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "None") == 0)
              dp->type = DITHER_TYPE_NONE;
            else if (strcmp(item->valuestring, "Flat") == 0)
              dp->type = DITHER_TYPE_FLAT;
            else if (strcmp(item->valuestring, "Highpass") == 0)
              dp->type = DITHER_TYPE_HIGHPASS;
            else if (strcmp(item->valuestring, "Fweighted441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_441;
            else if (strcmp(item->valuestring, "FweightedLong441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_LONG_441;
            else if (strcmp(item->valuestring, "FweightedShort441") == 0)
              dp->type = DITHER_TYPE_FWEIGHTED_SHORT_441;
            else if (strcmp(item->valuestring, "Gesemann441") == 0)
              dp->type = DITHER_TYPE_GESEMANN_441;
            else if (strcmp(item->valuestring, "Gesemann48") == 0)
              dp->type = DITHER_TYPE_GESEMANN_48;
            else if (strcmp(item->valuestring, "Lipshitz441") == 0)
              dp->type = DITHER_TYPE_LIPSHITZ_441;
            else if (strcmp(item->valuestring, "LipshitzLong441") == 0)
              dp->type = DITHER_TYPE_LIPSHITZ_LONG_441;
            else if (strcmp(item->valuestring, "Shibata441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_441;
            else if (strcmp(item->valuestring, "ShibataHigh441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_HIGH_441;
            else if (strcmp(item->valuestring, "ShibataLow441") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_441;
            else if (strcmp(item->valuestring, "Shibata48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_48;
            else if (strcmp(item->valuestring, "ShibataHigh48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_HIGH_48;
            else if (strcmp(item->valuestring, "ShibataLow48") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_48;
            else if (strcmp(item->valuestring, "Shibata882") == 0)
              dp->type = DITHER_TYPE_SHIBATA_882;
            else if (strcmp(item->valuestring, "ShibataLow882") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_882;
            else if (strcmp(item->valuestring, "Shibata96") == 0)
              dp->type = DITHER_TYPE_SHIBATA_96;
            else if (strcmp(item->valuestring, "ShibataLow96") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_96;
            else if (strcmp(item->valuestring, "Shibata192") == 0)
              dp->type = DITHER_TYPE_SHIBATA_192;
            else if (strcmp(item->valuestring, "ShibataLow192") == 0)
              dp->type = DITHER_TYPE_SHIBATA_LOW_192;
          }
          item = cJSON_GetObjectItemCaseSensitive(params, "bits");
          if (cJSON_IsNumber(item)) dp->bits = item->valueint;
          item = cJSON_GetObjectItemCaseSensitive(params, "amplitude");
          if (cJSON_IsNumber(item)) {
            dp->amplitude = item->valuedouble;
            dp->has_amplitude = true;
          }
          break;
        }
        case FILTER_TYPE_LIMITER: {
          limiter_parameters_t* lp = &f_conf->parameters.limiter;
          item = cJSON_GetObjectItemCaseSensitive(params, "clip_limit");
          if (cJSON_IsNumber(item)) lp->clip_limit = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "soft_clip");
          if (cJSON_IsBool(item)) lp->soft_clip = cJSON_IsTrue(item);
          break;
        }
        case FILTER_TYPE_LOOKAHEAD_LIMITER: {
          lookahead_limiter_parameters_t* llp =
              &f_conf->parameters.lookahead_limiter;
          item = cJSON_GetObjectItemCaseSensitive(params, "limit");
          if (cJSON_IsNumber(item)) llp->limit = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) llp->attack = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) llp->release = item->valuedouble;
          item = cJSON_GetObjectItemCaseSensitive(params, "unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              llp->unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              llp->unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              llp->unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              llp->unit = DELAY_UNIT_MM;
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

          item = cJSON_GetObjectItemCaseSensitive(params, "channels");
          if (cJSON_IsNumber(item)) cp->channels = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) cp->attack = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) cp->release = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "threshold");
          if (cJSON_IsNumber(item)) cp->threshold = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "factor");
          if (cJSON_IsNumber(item)) cp->factor = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "makeup_gain");
          if (cJSON_IsNumber(item)) {
            cp->makeup_gain = item->valuedouble;
            cp->has_makeup_gain = true;
          }

          item = cJSON_GetObjectItemCaseSensitive(params, "soft_clip");
          if (cJSON_IsBool(item)) cp->soft_clip = cJSON_IsTrue(item);

          item = cJSON_GetObjectItemCaseSensitive(params, "clip_limit");
          if (cJSON_IsNumber(item)) {
            cp->clip_limit = item->valuedouble;
            cp->has_clip_limit = true;
          }

          cJSON* mon_arr =
              cJSON_GetObjectItemCaseSensitive(params, "monitor_channels");
          cp->monitor_channels =
              parse_int_array(mon_arr, &cp->monitor_channels_count);

          cJSON* proc_arr =
              cJSON_GetObjectItemCaseSensitive(params, "process_channels");
          cp->process_channels =
              parse_int_array(proc_arr, &cp->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_NOISE_GATE: {
          noise_gate_parameters_t* ng = &p_conf->parameters.noise_gate;

          item = cJSON_GetObjectItemCaseSensitive(params, "channels");
          if (cJSON_IsNumber(item)) ng->channels = item->valueint;

          item = cJSON_GetObjectItemCaseSensitive(params, "attack");
          if (cJSON_IsNumber(item)) ng->attack = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "release");
          if (cJSON_IsNumber(item)) ng->release = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "threshold");
          if (cJSON_IsNumber(item)) ng->threshold = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "attenuation");
          if (cJSON_IsNumber(item)) ng->attenuation = item->valuedouble;

          cJSON* mon_arr =
              cJSON_GetObjectItemCaseSensitive(params, "monitor_channels");
          ng->monitor_channels =
              parse_int_array(mon_arr, &ng->monitor_channels_count);

          cJSON* proc_arr =
              cJSON_GetObjectItemCaseSensitive(params, "process_channels");
          ng->process_channels =
              parse_int_array(proc_arr, &ng->process_channels_count);
          break;
        }
        case PROCESSOR_TYPE_RACE: {
          race_parameters_t* rp = &p_conf->parameters.race;

          item = cJSON_GetObjectItemCaseSensitive(params, "attenuation");
          if (cJSON_IsNumber(item)) rp->attenuation = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "delay");
          if (cJSON_IsNumber(item)) rp->delay = item->valuedouble;

          item = cJSON_GetObjectItemCaseSensitive(params, "delay_unit");
          if (cJSON_IsString(item) && item->valuestring) {
            if (strcmp(item->valuestring, "ms") == 0)
              rp->delay_unit = DELAY_UNIT_MS;
            else if (strcmp(item->valuestring, "us") == 0)
              rp->delay_unit = DELAY_UNIT_US;
            else if (strcmp(item->valuestring, "samples") == 0)
              rp->delay_unit = DELAY_UNIT_SAMPLES;
            else if (strcmp(item->valuestring, "mm") == 0)
              rp->delay_unit = DELAY_UNIT_MM;
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
