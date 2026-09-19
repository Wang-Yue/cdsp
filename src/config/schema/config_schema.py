"""
CamillaDSP / CDSP Complete Configuration Schema.
Pure Python 3 standard library.
100% pure declarative schema definitions without embedded C strings.
"""

import sys
sys.dont_write_bytecode = True

from tools.codegen.schema_model import (
    FieldType, PrimitiveType, StringType, EnumType, ArrayType,
    StructType, TaggedUnionType, NamedMapType, VariantRule, Field,
    TYPE_BOOL, TYPE_INT, TYPE_INT64, TYPE_UINT32, TYPE_SIZE_T, TYPE_DOUBLE
)

# =============================================================================
# ENUMS
# =============================================================================

ENUM_TIME_UNIT = EnumType(
    name="time_unit",
    c_type="time_unit_t",
    variants=[
        ("TIME_UNIT_US", "us"),
        ("TIME_UNIT_MS", "ms"),
        ("TIME_UNIT_S", "s"),
        ("TIME_UNIT_SAMPLES", "samples"),
    ],
    default="TIME_UNIT_MS",
    invalid_val="TIME_UNIT_INVALID"
)

ENUM_DELAY_UNIT = EnumType(
    name="delay_unit",
    c_type="delay_unit_t",
    variants=[
        ("DELAY_UNIT_MS", "ms"),
        ("DELAY_UNIT_US", "us"),
        ("DELAY_UNIT_S", "s"),
        ("DELAY_UNIT_SAMPLES", "samples"),
        ("DELAY_UNIT_MM", "mm"),
    ],
    default="DELAY_UNIT_MS",
    invalid_val="DELAY_UNIT_INVALID"
)

ENUM_GAIN_SCALE = EnumType(
    name="gain_scale",
    c_type="gain_scale_t",
    variants=[
        ("GAIN_SCALE_DB", "dB"),
        ("GAIN_SCALE_LINEAR", "linear"),
    ],
    default="GAIN_SCALE_DB"
)

ENUM_FADER = EnumType(
    name="fader",
    c_type="fader_t",
    variants=[
        ("FADER_MAIN", "Main"),
        ("FADER_AUX1", "Aux1"),
        ("FADER_AUX2", "Aux2"),
        ("FADER_AUX3", "Aux3"),
        ("FADER_AUX4", "Aux4"),
    ],
    default="Main",
    invalid_val="FADER_NONE"
)

ENUM_VOLUME_FADER = EnumType(
    name="volume_fader",
    c_type="fader_t",
    variants=[
        ("FADER_AUX1", "Aux1"),
        ("FADER_AUX2", "Aux2"),
        ("FADER_AUX3", "Aux3"),
        ("FADER_AUX4", "Aux4"),
    ],
    invalid_val="FADER_NONE",
    is_external=True
)

ENUM_FILTER_TYPE = EnumType(
    name="filter_type",
    c_type="filter_type_t",
    variants=[
        ("FILTER_TYPE_GAIN", "Gain"),
        ("FILTER_TYPE_VOLUME", "Volume"),
        ("FILTER_TYPE_LOUDNESS", "Loudness"),
        ("FILTER_TYPE_BIQUAD", "Biquad"),
        ("FILTER_TYPE_CONV", "Conv"),
        ("FILTER_TYPE_DELAY", "Delay"),
        ("FILTER_TYPE_BIQUAD_COMBO", "BiquadCombo"),
        ("FILTER_TYPE_DIFF_EQ", "DiffEq"),
        ("FILTER_TYPE_DITHER", "Dither"),
        ("FILTER_TYPE_CLIPPER", "Clipper"),
        ("FILTER_TYPE_LOOKAHEAD_LIMITER", "LookaheadLimiter"),
    ],
    invalid_val="FILTER_TYPE_INVALID"
)

ENUM_BIQUAD_TYPE = EnumType(
    name="biquad_type",
    c_type="biquad_type_t",
    variants=[
        ("BIQUAD_TYPE_FREE", "Free"),
        ("BIQUAD_TYPE_HIGHPASS", "Highpass"),
        ("BIQUAD_TYPE_LOWPASS", "Lowpass"),
        ("BIQUAD_TYPE_HIGHPASS_FO", "HighpassFO"),
        ("BIQUAD_TYPE_LOWPASS_FO", "LowpassFO"),
        ("BIQUAD_TYPE_HIGHSHELF", "Highshelf"),
        ("BIQUAD_TYPE_LOWSHELF", "Lowshelf"),
        ("BIQUAD_TYPE_HIGHSHELF_FO", "HighshelfFO"),
        ("BIQUAD_TYPE_LOWSHELF_FO", "LowshelfFO"),
        ("BIQUAD_TYPE_PEAKING", "Peaking"),
        ("BIQUAD_TYPE_NOTCH", "Notch"),
        ("BIQUAD_TYPE_BANDPASS", "Bandpass"),
        ("BIQUAD_TYPE_ALLPASS", "Allpass"),
        ("BIQUAD_TYPE_ALLPASS_FO", "AllpassFO"),
        ("BIQUAD_TYPE_GENERAL_NOTCH", "GeneralNotch"),
        ("BIQUAD_TYPE_LINKWITZ_TRANSFORM", "LinkwitzTransform"),
    ]
)

ENUM_STEEPNESS_TYPE = EnumType(
    name="steepness_type",
    c_type="steepness_type_t",
    variants=[
        ("STEEPNESS_TYPE_Q", "q"),
        ("STEEPNESS_TYPE_BANDWIDTH", "bandwidth"),
        ("STEEPNESS_TYPE_SLOPE", "slope"),
    ],
    default="STEEPNESS_TYPE_Q"
)

ENUM_CONV_TYPE = EnumType(
    name="conv_type",
    c_type="conv_type_t",
    variants=[
        ("CONV_TYPE_RAW", "Raw"),
        ("CONV_TYPE_WAV", "Wav"),
        ("CONV_TYPE_VALUES", "Values"),
        ("CONV_TYPE_DUMMY", "Dummy"),
    ]
)

ENUM_BIQUAD_COMBO_TYPE = EnumType(
    name="biquad_combo_type",
    c_type="biquad_combo_type_t",
    variants=[
        ("BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS", "ButterworthHighpass"),
        ("BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS", "ButterworthLowpass"),
        ("BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS", "LinkwitzRileyHighpass"),
        ("BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS", "LinkwitzRileyLowpass"),
        ("BIQUAD_COMBO_TYPE_TILT", "Tilt"),
        ("BIQUAD_COMBO_TYPE_N_POINT_PEQ", "NPointPeq"),
        ("BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER", "GraphicEqualizer"),
    ]
)

ENUM_DITHER_TYPE = EnumType(
    name="dither_type",
    c_type="dither_type_t",
    variants=[
        ("DITHER_TYPE_NONE", "None"),
        ("DITHER_TYPE_FLAT", "Flat"),
        ("DITHER_TYPE_HIGHPASS", "Highpass"),
        ("DITHER_TYPE_FWEIGHTED_441", "Fweighted441"),
        ("DITHER_TYPE_FWEIGHTED_LONG_441", "FweightedLong441"),
        ("DITHER_TYPE_FWEIGHTED_SHORT_441", "FweightedShort441"),
        ("DITHER_TYPE_GESEMANN_441", "Gesemann441"),
        ("DITHER_TYPE_GESEMANN_48", "Gesemann48"),
        ("DITHER_TYPE_LIPSHITZ_441", "Lipshitz441"),
        ("DITHER_TYPE_LIPSHITZ_LONG_441", "LipshitzLong441"),
        ("DITHER_TYPE_SHIBATA_441", "Shibata441"),
        ("DITHER_TYPE_SHIBATA_HIGH_441", "ShibataHigh441"),
        ("DITHER_TYPE_SHIBATA_LOW_441", "ShibataLow441"),
        ("DITHER_TYPE_SHIBATA_48", "Shibata48"),
        ("DITHER_TYPE_SHIBATA_HIGH_48", "ShibataHigh48"),
        ("DITHER_TYPE_SHIBATA_LOW_48", "ShibataLow48"),
        ("DITHER_TYPE_SHIBATA_882", "Shibata882"),
        ("DITHER_TYPE_SHIBATA_LOW_882", "ShibataLow882"),
        ("DITHER_TYPE_SHIBATA_96", "Shibata96"),
        ("DITHER_TYPE_SHIBATA_LOW_96", "ShibataLow96"),
        ("DITHER_TYPE_SHIBATA_192", "Shibata192"),
        ("DITHER_TYPE_SHIBATA_LOW_192", "ShibataLow192"),
    ],
    default="None"
)

ENUM_RESAMPLER_TYPE = EnumType(
    name="resampler_type",
    c_type="resampler_type_t",
    variants=[
        ("RESAMPLER_TYPE_SYNCHRONOUS", "Synchronous"),
        ("RESAMPLER_TYPE_ASYNC_SINC", "AsyncSinc"),
        ("RESAMPLER_TYPE_ASYNC_POLY", "AsyncPoly"),
        ("RESAMPLER_TYPE_SLIP", "Slip"),
    ],
    default="Synchronous"
)

ENUM_RESAMPLER_PROFILE = EnumType(
    name="resampler_profile",
    c_type="resampler_profile_t",
    variants=[
        ("RESAMPLER_PROFILE_VERY_FAST", "VeryFast"),
        ("RESAMPLER_PROFILE_FAST", "Fast"),
        ("RESAMPLER_PROFILE_BALANCED", "Balanced"),
        ("RESAMPLER_PROFILE_ACCURATE", "Accurate"),
    ],
    default="Balanced"
)

ENUM_FIXED_ASYNC = EnumType(
    name="fixed_async",
    c_type="fixed_async_t",
    variants=[
        ("FIXED_ASYNC_INPUT", "input"),
        ("FIXED_ASYNC_OUTPUT", "output"),
    ],
    default="input"
)

ENUM_PROCESSOR_TYPE = EnumType(
    name="processor_type",
    c_type="processor_type_t",
    variants=[
        ("PROCESSOR_TYPE_COMPRESSOR", "Compressor"),
        ("PROCESSOR_TYPE_NOISE_GATE", "NoiseGate"),
        ("PROCESSOR_TYPE_RACE", "RACE"),
        ("PROCESSOR_TYPE_LOOKAHEAD_LIMITER", "LookaheadLimiter"),
    ],
    invalid_val="PROCESSOR_TYPE_INVALID"
)

ENUM_PIPELINE_STEP_TYPE = EnumType(
    name="pipeline_step_type",
    c_type="pipeline_step_type_t",
    variants=[
        ("PIPELINE_STEP_TYPE_FILTER", "Filter"),
        ("PIPELINE_STEP_TYPE_MIXER", "Mixer"),
        ("PIPELINE_STEP_TYPE_PROCESSOR", "Processor"),
    ]
)

# Audio Backend & Devices Enums
ENUM_AUDIO_BACKEND_TYPE = EnumType(
    name="audio_backend_type",
    c_type="audio_backend_type_t",
    variants=[
        ("AUDIO_BACKEND_TYPE_CORE_AUDIO", "CoreAudio", [], "ENABLE_COREAUDIO"),
        ("AUDIO_BACKEND_TYPE_ALSA", "Alsa", [], "ENABLE_ALSA"),
        ("AUDIO_BACKEND_TYPE_PIPEWIRE", "PipeWire", [], "ENABLE_PIPEWIRE"),
        ("AUDIO_BACKEND_TYPE_WASAPI", "Wasapi", [], "ENABLE_WASAPI"),
        ("AUDIO_BACKEND_TYPE_ASIO", "Asio", [], "ENABLE_ASIO"),
        ("AUDIO_BACKEND_TYPE_FILE", "File", ["RawFile", "WavFile"]),
        ("AUDIO_BACKEND_TYPE_STDIN_OUT", "Stdin", ["Stdout"]),
        ("AUDIO_BACKEND_TYPE_GENERATOR", "SignalGenerator"),
    ],
    invalid_val="AUDIO_BACKEND_TYPE_INVALID"
)

ENUM_SIGNAL_TYPE = EnumType(
    name="signal_type",
    c_type="signal_type_t",
    variants=[
        ("SIGNAL_TYPE_SINE", "Sine"),
        ("SIGNAL_TYPE_SQUARE", "Square"),
        ("SIGNAL_TYPE_WHITE_NOISE", "WhiteNoise"),
    ],
    invalid_val="SIGNAL_TYPE_INVALID"
)

ENUM_SDM_FILTER = EnumType(
    name="sdm_filter",
    c_type="sdm_filter_t",
    variants=[
        ("SDM_FILTER_CLANS4", "clans-4"),
        ("SDM_FILTER_SDM4", "sdm-4"),
        ("SDM_FILTER_CLANS5", "clans-5"),
        ("SDM_FILTER_SDM5", "sdm-5"),
        ("SDM_FILTER_CLANS6", "clans-6"),
        ("SDM_FILTER_SDM6", "sdm-6"),
        ("SDM_FILTER_CLANS7", "clans-7"),
        ("SDM_FILTER_SDM7", "sdm-7"),
        ("SDM_FILTER_CLANS8", "clans-8"),
        ("SDM_FILTER_SDM8", "sdm-8"),
    ],
    invalid_val="SDM_FILTER_INVALID",
    default="sdm-6"
)

ENUM_COREAUDIO_SAMPLE_FORMAT = EnumType(
    name="coreaudio_sample_format",
    c_type="coreaudio_sample_format_t",
    variants=[
        ("COREAUDIO_SAMPLE_FORMAT_S16", "S16"),
        ("COREAUDIO_SAMPLE_FORMAT_S24", "S24"),
        ("COREAUDIO_SAMPLE_FORMAT_S32", "S32"),
        ("COREAUDIO_SAMPLE_FORMAT_F32", "F32"),
    ],
    invalid_val="COREAUDIO_SAMPLE_FORMAT_INVALID",
    guard="ENABLE_COREAUDIO"
)

ENUM_ALSA_SAMPLE_FORMAT = EnumType(
    name="alsa_sample_format",
    c_type="alsa_sample_format_t",
    variants=[
        ("ALSA_SAMPLE_FORMAT_S16_LE", "S16_LE"),
        ("ALSA_SAMPLE_FORMAT_S24_3_LE", "S24_3_LE"),
        ("ALSA_SAMPLE_FORMAT_S24_4_LE", "S24_4_LE"),
        ("ALSA_SAMPLE_FORMAT_S32_LE", "S32_LE"),
        ("ALSA_SAMPLE_FORMAT_F32_LE", "F32_LE"),
        ("ALSA_SAMPLE_FORMAT_F64_LE", "F64_LE"),
        ("ALSA_SAMPLE_FORMAT_DSD_U8", "DSD_U8"),
        ("ALSA_SAMPLE_FORMAT_DSD_U16_LE", "DSD_U16_LE"),
        ("ALSA_SAMPLE_FORMAT_DSD_U16_BE", "DSD_U16_BE"),
        ("ALSA_SAMPLE_FORMAT_DSD_U32_LE", "DSD_U32_LE"),
        ("ALSA_SAMPLE_FORMAT_DSD_U32_BE", "DSD_U32_BE"),
    ],
    invalid_val="ALSA_SAMPLE_FORMAT_INVALID",
    guard="ENABLE_ALSA"
)

ENUM_WASAPI_SAMPLE_FORMAT = EnumType(
    name="wasapi_sample_format",
    c_type="wasapi_sample_format_t",
    variants=[
        ("WASAPI_SAMPLE_FORMAT_S16", "S16"),
        ("WASAPI_SAMPLE_FORMAT_S24", "S24"),
        ("WASAPI_SAMPLE_FORMAT_S32", "S32"),
        ("WASAPI_SAMPLE_FORMAT_F32", "F32"),
    ],
    invalid_val="WASAPI_SAMPLE_FORMAT_INVALID",
    guard="ENABLE_WASAPI"
)

ENUM_ASIO_SAMPLE_FORMAT = EnumType(
    name="asio_sample_format",
    c_type="asio_sample_format_t",
    variants=[
        ("ASIO_SAMPLE_FORMAT_S16_LE", "S16_LE"),
        ("ASIO_SAMPLE_FORMAT_S24_3_LE", "S24_3_LE"),
        ("ASIO_SAMPLE_FORMAT_S24_4_LE", "S24_4_LE"),
        ("ASIO_SAMPLE_FORMAT_S32_LE", "S32_LE"),
        ("ASIO_SAMPLE_FORMAT_F32_LE", "F32_LE"),
        ("ASIO_SAMPLE_FORMAT_F64_LE", "F64_LE"),
        ("ASIO_SAMPLE_FORMAT_DSD_INT8", "DSD_INT8"),
    ],
    invalid_val="ASIO_SAMPLE_FORMAT_INVALID",
    guard="ENABLE_ASIO"
)

ENUM_BINARY_SAMPLE_FORMAT = EnumType(
    name="binary_sample_format",
    c_type="binary_sample_format_t",
    variants=[
        ("BINARY_SAMPLE_FORMAT_S16_LE", "S16_LE"),
        ("BINARY_SAMPLE_FORMAT_S24_3_LE", "S24_3_LE"),
        ("BINARY_SAMPLE_FORMAT_S24_4_RJ_LE", "S24_4_RJ_LE"),
        ("BINARY_SAMPLE_FORMAT_S24_4_LJ_LE", "S24_4_LJ_LE"),
        ("BINARY_SAMPLE_FORMAT_S32_LE", "S32_LE"),
        ("BINARY_SAMPLE_FORMAT_F32_LE", "F32_LE"),
        ("BINARY_SAMPLE_FORMAT_F64_LE", "F64_LE"),
        ("BINARY_SAMPLE_FORMAT_DSD_U8", "DSD_U8"),
        ("BINARY_SAMPLE_FORMAT_DSD_U16_LE", "DSD_U16_LE"),
        ("BINARY_SAMPLE_FORMAT_DSD_U16_BE", "DSD_U16_BE"),
        ("BINARY_SAMPLE_FORMAT_DSD_U32_LE", "DSD_U32_LE"),
        ("BINARY_SAMPLE_FORMAT_DSD_U32_BE", "DSD_U32_BE"),
        ("BINARY_SAMPLE_FORMAT_DSD_U32_REVERSED", "DSD_U32_REVERSED"),
    ],
    invalid_val="BINARY_SAMPLE_FORMAT_INVALID"
)

# =============================================================================
# STRUCTS
# =============================================================================

# Resampler
STRUCT_RESAMPLER = StructType(
    name="resampler_config",
    c_type="resampler_config_t",
    fields=[
        Field("type", ENUM_RESAMPLER_TYPE, required=True),
        Field("profile", StringType(32), has_flag=True),
        Field("interpolation", StringType(32), has_flag=True),
        Field("sinc_len", TYPE_INT, has_flag=True),
        Field("oversampling_factor", TYPE_INT, has_flag=True),
        Field("window", StringType(32), has_flag=True),
        Field("f_cutoff", TYPE_DOUBLE, has_flag=True),
    ],
    variant_tag_field="type",
    variant_rules=[
        VariantRule(
            tag_value=["RESAMPLER_TYPE_SYNCHRONOUS", "RESAMPLER_TYPE_SLIP"],
            allowed_keys=["type"]
        ),
        VariantRule(
            tag_value="RESAMPLER_TYPE_ASYNC_POLY",
            allowed_keys=["type", "interpolation"],
            required_keys=["interpolation"],
            fields=["interpolation"]
        ),
        VariantRule(
            tag_value="RESAMPLER_TYPE_ASYNC_SINC",
            allowed_keys=["type", "profile", "sinc_len", "oversampling_factor", "interpolation", "window", "f_cutoff"],
            profile_field="profile",
            profile_enum=ENUM_RESAMPLER_PROFILE,
            profile_fallback_required=["sinc_len", "interpolation", "window", "oversampling_factor"],
            fields=["sinc_len", "oversampling_factor", "interpolation", "window", "f_cutoff"]
        )
    ]
)

# Mixer Structs
STRUCT_MIXER_SOURCE = StructType(
    name="mixer_source",
    c_type="mixer_source_t",
    fields=[
        Field("channel", TYPE_SIZE_T, required=True),
        Field("gain", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
        Field("inverted", TYPE_BOOL, default=False),
        Field("mute", TYPE_BOOL, default=False),
        Field("scale", ENUM_GAIN_SCALE, default="GAIN_SCALE_DB"),
    ]
)

STRUCT_MIXER_MAPPING = StructType(
    name="mixer_mapping",
    c_type="mixer_mapping_t",
    fields=[
        Field("dest", TYPE_SIZE_T, required=True),
        Field("sources", ArrayType(STRUCT_MIXER_SOURCE), required=True),
        Field("mute", TYPE_BOOL, default=False),
    ]
)

STRUCT_MIXER = StructType(
    name="mixer_config",
    c_type="mixer_config_t",
    fields=[
        Field("description", StringType(256)),
        Field("channels_in", TYPE_SIZE_T, json_object="channels", json_key="in", required=True),
        Field("channels_out", TYPE_SIZE_T, json_object="channels", json_key="out", required=True),
        Field("mapping", ArrayType(STRUCT_MIXER_MAPPING), required=True),
        Field("labels", ArrayType(StringType(128)), has_flag=True, allow_null_items=True),
    ],
    nested_objects={
        "channels": ["in", "out"]
    }
)

FILTER_EXTRA_KEYS = ["type", "description"]
PROCESSOR_EXTRA_KEYS = ["type", "description"]
CAPTURE_EXTRA_KEYS = ["type", "labels", "bypass_dop", "dop_cutoff_hz", "description"]
PLAYBACK_EXTRA_KEYS = ["type", "labels", "output_dop", "dsd_encoder_filter", "description"]

# Processor Structs
STRUCT_COMPRESSOR = StructType(
    name="compressor_config",
    c_type="compressor_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T),
        Field("monitor_channels", ArrayType(TYPE_SIZE_T)),
        Field("process_channels", ArrayType(TYPE_SIZE_T)),
        Field("attack", TYPE_DOUBLE, required=True),
        Field("attack_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("release", TYPE_DOUBLE, required=True),
        Field("release_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("threshold", TYPE_DOUBLE, required=True),
        Field("factor", TYPE_DOUBLE, required=True),
        Field("makeup_gain", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
        Field("soft_clip", TYPE_BOOL, default=False),
        Field("clip_limit", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
    ],
    allowed_extra_keys=PROCESSOR_EXTRA_KEYS
)

STRUCT_NOISE_GATE = StructType(
    name="noise_gate_config",
    c_type="noise_gate_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T),
        Field("monitor_channels", ArrayType(TYPE_SIZE_T)),
        Field("process_channels", ArrayType(TYPE_SIZE_T)),
        Field("attack", TYPE_DOUBLE, required=True),
        Field("attack_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("release", TYPE_DOUBLE, required=True),
        Field("release_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("threshold", TYPE_DOUBLE, required=True),
        Field("attenuation", TYPE_DOUBLE, required=True),
    ],
    allowed_extra_keys=PROCESSOR_EXTRA_KEYS
)

STRUCT_RACE = StructType(
    name="race_config",
    c_type="race_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T),
        Field("channel_a", TYPE_SIZE_T),
        Field("channel_b", TYPE_SIZE_T),
        Field("delay", TYPE_DOUBLE, required=True),
        Field("subsample_delay", TYPE_BOOL, has_flag=True, default=False),
        Field("delay_unit", ENUM_DELAY_UNIT, has_flag=True, default="DELAY_UNIT_MS"),
        Field("attenuation", TYPE_DOUBLE, required=True),
    ],
    allowed_extra_keys=PROCESSOR_EXTRA_KEYS
)

STRUCT_LOOKAHEAD_LIMITER_PROC = StructType(
    name="lookahead_limiter_processor_config",
    c_type="lookahead_limiter_processor_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T),
        Field("monitor_channels", ArrayType(TYPE_SIZE_T)),
        Field("process_channels", ArrayType(TYPE_SIZE_T)),
        Field("limit", TYPE_DOUBLE, required=True),
        Field("attack", TYPE_DOUBLE, required=True),
        Field("attack_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("release", TYPE_DOUBLE, required=True),
        Field("release_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("delay_processed_only", TYPE_BOOL, default=False),
    ],
    allowed_extra_keys=PROCESSOR_EXTRA_KEYS
)

UNION_PROCESSOR = TaggedUnionType(
    name="processor_config",
    c_type="processor_config_t",
    tag_enum=ENUM_PROCESSOR_TYPE,
    tag_field="type",
    union_field="parameters",
    requires_parameters_object=True,
    container_allowed_keys=["type", "parameters", "description"],
    missing_params_msg="missing 'parameters' object in processor '%s'",
    variants={
        "PROCESSOR_TYPE_COMPRESSOR": ("compressor", STRUCT_COMPRESSOR),
        "PROCESSOR_TYPE_NOISE_GATE": ("noise_gate", STRUCT_NOISE_GATE),
        "PROCESSOR_TYPE_RACE": ("race", STRUCT_RACE),
        "PROCESSOR_TYPE_LOOKAHEAD_LIMITER": ("lookahead_limiter", STRUCT_LOOKAHEAD_LIMITER_PROC),
    }
)

# Filter Structs
STRUCT_GAIN = StructType(
    name="gain_config",
    c_type="gain_config_t",
    fields=[
        Field("gain", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
        Field("scale", ENUM_GAIN_SCALE, default="GAIN_SCALE_DB"),
        Field("inverted", TYPE_BOOL, default=False),
        Field("mute", TYPE_BOOL, default=False),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_VOLUME = StructType(
    name="volume_config",
    c_type="volume_config_t",
    fields=[
        Field("fader", ENUM_VOLUME_FADER, required=True),
        Field("ramp_time_ms", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
        Field("limit", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_LOUDNESS = StructType(
    name="loudness_config",
    c_type="loudness_config_t",
    fields=[
        Field("reference_level", TYPE_DOUBLE, has_flag=True),
        Field("high_boost", TYPE_DOUBLE, has_flag=True),
        Field("low_boost", TYPE_DOUBLE, has_flag=True),
        Field("attenuate_mid", TYPE_BOOL, default=False),
        Field("high_freq", TYPE_DOUBLE, has_flag=True),
        Field("low_freq", TYPE_DOUBLE, has_flag=True),
        Field("high_q", TYPE_DOUBLE, has_flag=True),
        Field("low_q", TYPE_DOUBLE, has_flag=True),
        Field("fader", ENUM_FADER, default="FADER_MAIN"),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_BIQUAD = StructType(
    name="biquad_config",
    c_type="biquad_config_t",
    fields=[
        Field("type", ENUM_BIQUAD_TYPE, required=True),
        Field("freq", TYPE_DOUBLE, has_flag=True),
        Field("gain", TYPE_DOUBLE, has_flag=True),
        Field("q", TYPE_DOUBLE, has_flag=True),
        Field("bandwidth", TYPE_DOUBLE, has_flag=True),
        Field("slope", TYPE_DOUBLE, has_flag=True),
        Field("a1", TYPE_DOUBLE, has_flag=True),
        Field("a2", TYPE_DOUBLE, has_flag=True),
        Field("b0", TYPE_DOUBLE, has_flag=True),
        Field("b1", TYPE_DOUBLE, has_flag=True),
        Field("b2", TYPE_DOUBLE, has_flag=True),
        Field("freq_notch", TYPE_DOUBLE, has_flag=True),
        Field("freq_pole", TYPE_DOUBLE, has_flag=True),
        Field("q_p", TYPE_DOUBLE, has_flag=True),
        Field("normalize_at_dc", TYPE_BOOL, default=False),
        Field("freq_act", TYPE_DOUBLE, has_flag=True),
        Field("q_act", TYPE_DOUBLE, has_flag=True),
        Field("freq_target", TYPE_DOUBLE, has_flag=True),
        Field("q_target", TYPE_DOUBLE, has_flag=True),
        Field("steepness_type", ENUM_STEEPNESS_TYPE, default="STEEPNESS_TYPE_Q"),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS,
    variant_tag_field="type",
    variant_rules=[
        VariantRule(
            tag_value="BIQUAD_TYPE_FREE",
            required_keys=["a1", "a2", "b0", "b1", "b2"],
            fields=["a1", "a2", "b0", "b1", "b2"]
        ),
        VariantRule(
            tag_value=[
                "BIQUAD_TYPE_HIGHPASS_FO", "BIQUAD_TYPE_LOWPASS_FO",
                "BIQUAD_TYPE_HIGHSHELF_FO", "BIQUAD_TYPE_LOWSHELF_FO",
                "BIQUAD_TYPE_ALLPASS_FO"
            ],
            required_keys=["freq"],
            fields=["freq", "gain"]
        ),
        VariantRule(
            tag_value=["BIQUAD_TYPE_HIGHPASS", "BIQUAD_TYPE_LOWPASS"],
            required_keys=["freq", "q"],
            fields=["freq", "q"],
            sets_field=("steepness_type", "STEEPNESS_TYPE_Q")
        ),
        VariantRule(
            tag_value="BIQUAD_TYPE_PEAKING",
            required_keys=["freq", "gain"],
            fields=["freq", "gain"],
            one_of=[["q", "bandwidth"]],
            one_of_sets={
                "q": ("steepness_type", "STEEPNESS_TYPE_Q"),
                "bandwidth": ("steepness_type", "STEEPNESS_TYPE_BANDWIDTH")
            }
        ),
        VariantRule(
            tag_value=["BIQUAD_TYPE_NOTCH", "BIQUAD_TYPE_BANDPASS", "BIQUAD_TYPE_ALLPASS"],
            required_keys=["freq"],
            fields=["freq"],
            one_of=[["q", "bandwidth"]],
            one_of_sets={
                "q": ("steepness_type", "STEEPNESS_TYPE_Q"),
                "bandwidth": ("steepness_type", "STEEPNESS_TYPE_BANDWIDTH")
            }
        ),
        VariantRule(
            tag_value=["BIQUAD_TYPE_HIGHSHELF", "BIQUAD_TYPE_LOWSHELF"],
            required_keys=["freq", "gain"],
            fields=["freq", "gain"],
            any_of_optional=[
                ("q", ("steepness_type", "STEEPNESS_TYPE_Q")),
                ("slope", ("steepness_type", "STEEPNESS_TYPE_SLOPE"))
            ],
            default_assignments={
                "q": "1.0 / sqrt(2.0)",
                "has_q": "true",
                "steepness_type": "STEEPNESS_TYPE_Q"
            }
        ),
        VariantRule(
            tag_value="BIQUAD_TYPE_GENERAL_NOTCH",
            required_keys=["freq_notch", "freq_pole"],
            fields=["freq_notch", "freq_pole", "q_p", "q", "normalize_at_dc"]
        ),
        VariantRule(
            tag_value="BIQUAD_TYPE_LINKWITZ_TRANSFORM",
            required_keys=["freq_act", "q_act", "freq_target", "q_target"],
            fields=["freq_act", "q_act", "freq_target", "q_target"]
        ),
    ]
)

STRUCT_CONVOLUTION = StructType(
    name="conv_config",
    c_type="conv_config_t",
    fields=[
        Field("type", ENUM_CONV_TYPE, required=True),
        Field("filename", StringType(512), has_flag=True),
        Field("format", StringType(32), has_flag=True),
        Field("channel", TYPE_INT, has_flag=True),
        Field("length", TYPE_INT, has_flag=True),
        Field("skip_bytes_lines", TYPE_SIZE_T, has_flag=True),
        Field("read_bytes_lines", TYPE_SIZE_T, has_flag=True),
        Field("values", ArrayType(TYPE_DOUBLE), has_flag=True),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS,
    variant_tag_field="type",
    variant_rules=[
        VariantRule(
            tag_value="CONV_TYPE_WAV",
            required_keys=["filename"],
            fields=["filename", "channel", "length", "skip_bytes_lines", "read_bytes_lines"]
        ),
        VariantRule(
            tag_value="CONV_TYPE_RAW",
            required_keys=["filename", "format"],
            fields=["filename", "format", "channel", "length", "skip_bytes_lines", "read_bytes_lines"]
        ),
        VariantRule(
            tag_value="CONV_TYPE_VALUES",
            required_keys=["values"],
            fields=["values"]
        ),
        VariantRule(
            tag_value="CONV_TYPE_DUMMY",
            fields=["length"]
        ),
    ]
)

STRUCT_DELAY = StructType(
    name="delay_config",
    c_type="delay_config_t",
    fields=[
        Field("delay", TYPE_DOUBLE, required=True),
        Field("delay_unit", ENUM_DELAY_UNIT, default="DELAY_UNIT_MS"),
        Field("subsample", TYPE_BOOL, default=False),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_PEQ_BAND = StructType(
    name="peq_band",
    c_type="peq_band_t",
    fields=[
        Field("freq", TYPE_DOUBLE, required=True),
        Field("q", TYPE_DOUBLE, required=True),
        Field("gain", TYPE_DOUBLE, required=True),
    ]
)

STRUCT_BIQUAD_COMBO = StructType(
    name="biquad_combo_config",
    c_type="biquad_combo_config_t",
    fields=[
        Field("type", ENUM_BIQUAD_COMBO_TYPE, required=True),
        Field("freq", TYPE_DOUBLE, has_flag=True),
        Field("order", TYPE_INT, has_flag=True),
        Field("gain", TYPE_DOUBLE, has_flag=True),
        Field("high_gain", TYPE_DOUBLE, has_flag=True),
        Field("bands", ArrayType(STRUCT_PEQ_BAND)),
        Field("freq_min", TYPE_DOUBLE, has_flag=True),
        Field("freq_max", TYPE_DOUBLE, has_flag=True),
        Field("gains", ArrayType(TYPE_DOUBLE)),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS,
    variant_tag_field="type",
    variant_rules=[
        VariantRule(
            tag_value=[
                "BIQUAD_COMBO_TYPE_BUTTERWORTH_HIGHPASS",
                "BIQUAD_COMBO_TYPE_BUTTERWORTH_LOWPASS",
                "BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_HIGHPASS",
                "BIQUAD_COMBO_TYPE_LINKWITZ_RILEY_LOWPASS"
            ],
            required_keys=["freq", "order"],
            fields=["freq", "order"]
        ),
        VariantRule(
            tag_value="BIQUAD_COMBO_TYPE_TILT",
            fields=["gain", "high_gain", "freq"]
        ),
        VariantRule(
            tag_value="BIQUAD_COMBO_TYPE_N_POINT_PEQ",
            fields=["bands"]
        ),
        VariantRule(
            tag_value="BIQUAD_COMBO_TYPE_GRAPHIC_EQUALIZER",
            fields=["freq_min", "freq_max", "gains"]
        ),
    ]
)

STRUCT_DIFFEQ = StructType(
    name="diff_eq_config",
    c_type="diff_eq_config_t",
    fields=[
        Field("a", ArrayType(TYPE_DOUBLE), required=True),
        Field("b", ArrayType(TYPE_DOUBLE), required=True),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_DITHER = StructType(
    name="dither_config",
    c_type="dither_config_t",
    fields=[
        Field("type", ENUM_DITHER_TYPE, default="DITHER_TYPE_NONE"),
        Field("bits", TYPE_INT, required=True),
        Field("amplitude", TYPE_DOUBLE, has_flag=True, getter_default=0.0),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_CLIPPER = StructType(
    name="clipper_config",
    c_type="clipper_config_t",
    fields=[
        Field("clip_limit", TYPE_DOUBLE, required=True),
        Field("soft_clip", TYPE_BOOL, default=False),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

STRUCT_LOOKAHEAD_LIMITER_FILTER = StructType(
    name="lookahead_limiter_filter_config",
    c_type="lookahead_limiter_filter_config_t",
    fields=[
        Field("limit", TYPE_DOUBLE, required=True),
        Field("attack", TYPE_DOUBLE, required=True),
        Field("attack_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
        Field("release", TYPE_DOUBLE, required=True),
        Field("release_unit", ENUM_TIME_UNIT, default="TIME_UNIT_MS"),
    ],
    allowed_extra_keys=FILTER_EXTRA_KEYS
)

UNION_FILTER = TaggedUnionType(
    name="filter_config",
    c_type="filter_config_t",
    tag_enum=ENUM_FILTER_TYPE,
    tag_field="type",
    union_field="parameters",
    requires_parameters_object=True,
    container_allowed_keys=["type", "parameters", "description"],
    missing_params_msg="missing 'parameters' in filter '%s'",
    variants={
        "FILTER_TYPE_GAIN": ("gain", STRUCT_GAIN),
        "FILTER_TYPE_VOLUME": ("volume", STRUCT_VOLUME),
        "FILTER_TYPE_LOUDNESS": ("loudness", STRUCT_LOUDNESS),
        "FILTER_TYPE_BIQUAD": ("biquad", STRUCT_BIQUAD),
        "FILTER_TYPE_CONV": ("conv", STRUCT_CONVOLUTION),
        "FILTER_TYPE_DELAY": ("delay", STRUCT_DELAY),
        "FILTER_TYPE_BIQUAD_COMBO": ("biquad_combo", STRUCT_BIQUAD_COMBO),
        "FILTER_TYPE_DIFF_EQ": ("diff_eq", STRUCT_DIFFEQ),
        "FILTER_TYPE_DITHER": ("dither", STRUCT_DITHER),
        "FILTER_TYPE_CLIPPER": ("clipper", STRUCT_CLIPPER),
        "FILTER_TYPE_LOOKAHEAD_LIMITER": ("lookahead_limiter", STRUCT_LOOKAHEAD_LIMITER_FILTER),
    }
)

# Pipeline Step Struct
STRUCT_PIPELINE_STEP = StructType(
    name="pipeline_step_config",
    c_type="pipeline_step_config_t",
    fields=[
        Field("type", ENUM_PIPELINE_STEP_TYPE, required=True),
        Field("description", StringType(256)),
        Field("channel", TYPE_SIZE_T, has_flag=True),
        Field("channels", ArrayType(TYPE_SIZE_T), has_flag=True),
        Field("name", StringType(128), has_flag=True),
        Field("names", ArrayType(StringType(128)), has_flag=True),
        Field("bypassed", TYPE_BOOL, default=False),
    ],
    variant_tag_field="type",
    variant_rules=[
        VariantRule(
            tag_value="PIPELINE_STEP_TYPE_FILTER",
            allowed_keys=["type", "names", "channels", "description", "bypassed"],
            required_keys=["names"],
            fields=["names", "channels", "description", "bypassed"]
        ),
        VariantRule(
            tag_value="PIPELINE_STEP_TYPE_MIXER",
            allowed_keys=["type", "name", "description", "bypassed"],
            required_keys=["name"],
            fields=["name", "description", "bypassed"]
        ),
        VariantRule(
            tag_value="PIPELINE_STEP_TYPE_PROCESSOR",
            allowed_keys=["type", "name", "description", "bypassed"],
            required_keys=["name"],
            fields=["name", "description", "bypassed"]
        ),
    ]
)

# Backend Device Structs
STRUCT_GENERATOR_SIGNAL = StructType(
    name="generator_signal",
    c_type="generator_signal_t",
    fields=[
        Field("type", ENUM_SIGNAL_TYPE, required=True),
        Field("freq", TYPE_DOUBLE, has_flag=True, getter_default=1000.0),
        Field("level", TYPE_DOUBLE, required=True),
    ]
)

STRUCT_COREAUDIO_CAPTURE = StructType(
    name="coreaudio_capture_config",
    c_type="coreaudio_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_COREAUDIO_SAMPLE_FORMAT, has_flag=True),
        Field("loopback", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS,
    guard="ENABLE_COREAUDIO"
)

STRUCT_COREAUDIO_PLAYBACK = StructType(
    name="coreaudio_playback_config",
    c_type="coreaudio_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_COREAUDIO_SAMPLE_FORMAT, has_flag=True),
        Field("exclusive", TYPE_BOOL, has_flag=True, default=False),
        Field("target_level", TYPE_INT, has_flag=True),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS,
    guard="ENABLE_COREAUDIO"
)

STRUCT_ALSA_CAPTURE = StructType(
    name="alsa_capture_config",
    c_type="alsa_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), required=True),
        Field("format", ENUM_ALSA_SAMPLE_FORMAT, has_flag=True),
        Field("stop_on_inactive", TYPE_BOOL, has_flag=True, default=False),
        Field("link_volume_control", StringType(256), has_flag=True),
        Field("link_mute_control", StringType(256), has_flag=True),
        Field("threaded", TYPE_BOOL, has_flag=True, default=True),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS,
    guard="ENABLE_ALSA"
)

STRUCT_ALSA_PLAYBACK = StructType(
    name="alsa_playback_config",
    c_type="alsa_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), required=True),
        Field("format", ENUM_ALSA_SAMPLE_FORMAT, has_flag=True),
        Field("target_level", TYPE_INT, has_flag=True),
        Field("threaded", TYPE_BOOL, has_flag=True, default=True),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS,
    guard="ENABLE_ALSA"
)

STRUCT_PIPEWIRE_CAPTURE = StructType(
    name="pipewire_capture_config",
    c_type="pipewire_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("node_name", StringType(256), has_flag=True),
        Field("node_description", StringType(256), has_flag=True),
        Field("node_group_name", StringType(256), has_flag=True),
        Field("autoconnect_to", StringType(256), has_flag=True),
        Field("loopback", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS,
    guard="ENABLE_PIPEWIRE"
)

STRUCT_PIPEWIRE_PLAYBACK = StructType(
    name="pipewire_playback_config",
    c_type="pipewire_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("node_name", StringType(256), has_flag=True),
        Field("node_description", StringType(256), has_flag=True),
        Field("node_group_name", StringType(256), has_flag=True),
        Field("autoconnect_to", StringType(256), has_flag=True),
        Field("target_level", TYPE_INT, has_flag=True),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS,
    guard="ENABLE_PIPEWIRE"
)

STRUCT_STDIN_CAPTURE = StructType(
    name="stdin_capture_config",
    c_type="stdin_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("format", ENUM_BINARY_SAMPLE_FORMAT, required=True),
        Field("extra_samples", TYPE_INT, has_flag=True),
        Field("skip_bytes", TYPE_SIZE_T, has_flag=True),
        Field("read_bytes", TYPE_SIZE_T, has_flag=True),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS
)

STRUCT_STDOUT_PLAYBACK = StructType(
    name="stdout_playback_config",
    c_type="stdout_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("format", ENUM_BINARY_SAMPLE_FORMAT, required=True),
        Field("wav_header", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS
)

STRUCT_WASAPI_CAPTURE = StructType(
    name="wasapi_capture_config",
    c_type="wasapi_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_WASAPI_SAMPLE_FORMAT, has_flag=True),
        Field("exclusive", TYPE_BOOL, has_flag=True, default=False),
        Field("loopback", TYPE_BOOL, has_flag=True, default=False),
        Field("polling", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS,
    guard="ENABLE_WASAPI"
)

STRUCT_WASAPI_PLAYBACK = StructType(
    name="wasapi_playback_config",
    c_type="wasapi_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_WASAPI_SAMPLE_FORMAT, has_flag=True),
        Field("exclusive", TYPE_BOOL, has_flag=True, default=False),
        Field("polling", TYPE_BOOL, has_flag=True, default=False),
        Field("target_level", TYPE_INT, has_flag=True),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS,
    guard="ENABLE_WASAPI"
)

STRUCT_ASIO_CAPTURE = StructType(
    name="asio_capture_config",
    c_type="asio_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_ASIO_SAMPLE_FORMAT, has_flag=True),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS,
    guard="ENABLE_ASIO"
)

STRUCT_ASIO_PLAYBACK = StructType(
    name="asio_playback_config",
    c_type="asio_playback_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("device", StringType(256), has_flag=True),
        Field("format", ENUM_ASIO_SAMPLE_FORMAT, has_flag=True),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS,
    guard="ENABLE_ASIO"
)

STRUCT_WAV_FILE_CAPTURE = StructType(
    name="wav_file_capture_config",
    c_type="wav_file_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T),
        Field("filename", StringType(512), required=True, has_flag=True),
        Field("extra_samples", TYPE_INT, has_flag=True),
        Field("realtime", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS
)

STRUCT_RAW_FILE_CAPTURE = StructType(
    name="raw_file_capture_config",
    c_type="raw_file_capture_config_t",
    fields=[
        Field("filename", StringType(512), required=True, has_flag=True),
        Field("format", ENUM_BINARY_SAMPLE_FORMAT, required=True, has_flag=True),
        Field("channels", TYPE_SIZE_T, required=True),
        Field("skip_bytes", TYPE_SIZE_T, has_flag=True),
        Field("read_bytes", TYPE_SIZE_T, has_flag=True),
        Field("extra_samples", TYPE_INT, has_flag=True),
        Field("realtime", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS
)

STRUCT_RAW_FILE_PLAYBACK = StructType(
    name="raw_file_playback_config",
    c_type="raw_file_playback_config_t",
    fields=[
        Field("filename", StringType(512), required=True, has_flag=True),
        Field("format", ENUM_BINARY_SAMPLE_FORMAT, required=True, has_flag=True),
        Field("channels", TYPE_SIZE_T, required=True),
        Field("wav_header", TYPE_BOOL, has_flag=True, default=False),
        Field("use_rf64", TYPE_BOOL, has_flag=True, default=False),
        Field("realtime", TYPE_BOOL, has_flag=True, default=False),
    ],
    allowed_extra_keys=PLAYBACK_EXTRA_KEYS
)

STRUCT_GENERATOR_CAPTURE = StructType(
    name="generator_capture_config",
    c_type="generator_capture_config_t",
    fields=[
        Field("channels", TYPE_SIZE_T, required=True),
        Field("signal", STRUCT_GENERATOR_SIGNAL, required=True),
    ],
    allowed_extra_keys=CAPTURE_EXTRA_KEYS
)

UNION_CAPTURE = TaggedUnionType(
    name="capture_device_config",
    c_type="capture_device_config_t",
    tag_enum=ENUM_AUDIO_BACKEND_TYPE,
    tag_field="type",
    union_field="cfg",
    extra_fields=[
        Field("labels", ArrayType(StringType(128)), has_flag=True, allow_null_items=True),
        Field("is_wav", TYPE_BOOL, has_flag=True),
        Field("bypass_dop", TYPE_BOOL, has_flag=True, default=True),
        Field("dop_cutoff_hz", TYPE_DOUBLE, has_flag=True, default=20000.0),
    ],
    variants={
        "AUDIO_BACKEND_TYPE_CORE_AUDIO": ("coreaudio", STRUCT_COREAUDIO_CAPTURE),
        "AUDIO_BACKEND_TYPE_ALSA": ("alsa", STRUCT_ALSA_CAPTURE),
        "AUDIO_BACKEND_TYPE_PIPEWIRE": ("pipewire", STRUCT_PIPEWIRE_CAPTURE),
        "AUDIO_BACKEND_TYPE_FILE": ("raw_file", STRUCT_RAW_FILE_CAPTURE),
        "AUDIO_BACKEND_TYPE_STDIN_OUT": ("stdin_in", STRUCT_STDIN_CAPTURE),
        "AUDIO_BACKEND_TYPE_GENERATOR": ("generator", STRUCT_GENERATOR_CAPTURE),
        "AUDIO_BACKEND_TYPE_WASAPI": ("wasapi", STRUCT_WASAPI_CAPTURE),
        "AUDIO_BACKEND_TYPE_ASIO": ("asio", STRUCT_ASIO_CAPTURE),
    },
    extra_union_members=[("wav_file", STRUCT_WAV_FILE_CAPTURE)],
    is_flattened=True,
    rejected_variants=["File", "Stdout"],
    variant_type_aliases={
        "RawFile": ("AUDIO_BACKEND_TYPE_FILE", "raw_file", {"is_wav": False, "has_is_wav": True}),
        "WavFile": ("AUDIO_BACKEND_TYPE_FILE", "wav_file", {"is_wav": True, "has_is_wav": True}),
    }
)

UNION_PLAYBACK = TaggedUnionType(
    name="playback_device_config",
    c_type="playback_device_config_t",
    tag_enum=ENUM_AUDIO_BACKEND_TYPE,
    tag_field="type",
    union_field="cfg",
    extra_fields=[
        Field("labels", ArrayType(StringType(128)), has_flag=True, allow_null_items=True),
        Field("is_wav", TYPE_BOOL, has_flag=True),
        Field("output_dop", TYPE_BOOL, has_flag=True, default=False),
        Field("dsd_encoder_filter", ENUM_SDM_FILTER, has_flag=True, default="SDM_FILTER_SDM6"),
    ],
    variants={
        "AUDIO_BACKEND_TYPE_CORE_AUDIO": ("coreaudio", STRUCT_COREAUDIO_PLAYBACK),
        "AUDIO_BACKEND_TYPE_ALSA": ("alsa", STRUCT_ALSA_PLAYBACK),
        "AUDIO_BACKEND_TYPE_PIPEWIRE": ("pipewire", STRUCT_PIPEWIRE_PLAYBACK),
        "AUDIO_BACKEND_TYPE_FILE": ("raw_file", STRUCT_RAW_FILE_PLAYBACK),
        "AUDIO_BACKEND_TYPE_STDIN_OUT": ("stdout_out", STRUCT_STDOUT_PLAYBACK),
        "AUDIO_BACKEND_TYPE_WASAPI": ("wasapi", STRUCT_WASAPI_PLAYBACK),
        "AUDIO_BACKEND_TYPE_ASIO": ("asio", STRUCT_ASIO_PLAYBACK),
    },
    is_flattened=True,
    rejected_variants=["Stdin", "WavFile", "RawFile"],
    variant_type_aliases={
        "File": ("AUDIO_BACKEND_TYPE_FILE", "raw_file", {}),
    }
)

STRUCT_DEVICES = StructType(
    name="devices_config",
    c_type="devices_config_t",
    fields=[
        Field("samplerate", TYPE_SIZE_T, required=True),
        Field("chunksize", TYPE_SIZE_T, required=True),
        Field("enable_rate_adjust", TYPE_BOOL, has_flag=True, default=False),
        Field("target_level", TYPE_INT, has_flag=True),
        Field("adjust_interval_s", TYPE_DOUBLE, has_flag=True),
        Field("resampler", STRUCT_RESAMPLER, has_flag=True),
        Field("capture", UNION_CAPTURE, required=True),
        Field("playback", UNION_PLAYBACK, required=True),
        Field("capture_samplerate", TYPE_SIZE_T, has_flag=True),
        Field("silence_threshold", TYPE_DOUBLE, has_flag=True),
        Field("silence_timeout_s", TYPE_DOUBLE, has_flag=True),
        Field("volume_ramp_time_ms", TYPE_DOUBLE, has_flag=True),
        Field("volume_limit", TYPE_DOUBLE, has_flag=True),
        Field("queuelimit", TYPE_INT, has_flag=True),
        Field("stop_on_rate_change", TYPE_BOOL, has_flag=True, default=False),
        Field("rate_measure_interval_s", TYPE_DOUBLE, has_flag=True),
        Field("multithreaded", TYPE_BOOL, has_flag=True, default=False),
        Field("worker_threads", TYPE_INT, has_flag=True),
    ]
)

STRUCT_DSP_CONFIG = StructType(
    name="dsp_config",
    c_type="dsp_config_t",
    fields=[
        Field("title", StringType(128)),
        Field("description", StringType(256)),
        Field("devices", STRUCT_DEVICES, required=True),
        Field("filters", NamedMapType(
            name="filters",
            item_c_type="named_filter_config_t",
            value_type=UNION_FILTER,
            value_field="filter",
            has_description=True
        )),
        Field("mixers", NamedMapType(
            name="mixers",
            item_c_type="named_mixer_config_t",
            value_type=STRUCT_MIXER,
            value_field="mixer",
            has_description=False
        )),
        Field("processors", NamedMapType(
            name="processors",
            item_c_type="named_processor_config_t",
            value_type=UNION_PROCESSOR,
            value_field="processor",
            has_description=True
        )),
        Field("pipeline", ArrayType(STRUCT_PIPELINE_STEP)),
    ]
)


# All schemas to generate
ALL_SCHEMAS = [
    # Enums
    ENUM_TIME_UNIT,
    ENUM_DELAY_UNIT,
    ENUM_GAIN_SCALE,
    ENUM_FADER,
    ENUM_VOLUME_FADER,
    ENUM_FILTER_TYPE,
    ENUM_BIQUAD_TYPE,
    ENUM_STEEPNESS_TYPE,
    ENUM_CONV_TYPE,
    ENUM_BIQUAD_COMBO_TYPE,
    ENUM_DITHER_TYPE,
    ENUM_RESAMPLER_TYPE,
    ENUM_RESAMPLER_PROFILE,
    ENUM_FIXED_ASYNC,
    ENUM_PROCESSOR_TYPE,
    ENUM_PIPELINE_STEP_TYPE,
    ENUM_AUDIO_BACKEND_TYPE,
    ENUM_SIGNAL_TYPE,
    ENUM_SDM_FILTER,
    ENUM_COREAUDIO_SAMPLE_FORMAT,
    ENUM_ALSA_SAMPLE_FORMAT,
    ENUM_WASAPI_SAMPLE_FORMAT,
    ENUM_ASIO_SAMPLE_FORMAT,
    ENUM_BINARY_SAMPLE_FORMAT,

    # Resampler
    STRUCT_RESAMPLER,

    # Mixer
    STRUCT_MIXER_SOURCE,
    STRUCT_MIXER_MAPPING,
    STRUCT_MIXER,

    # Processors
    STRUCT_COMPRESSOR,
    STRUCT_NOISE_GATE,
    STRUCT_RACE,
    STRUCT_LOOKAHEAD_LIMITER_PROC,
    UNION_PROCESSOR,

    # Filter Structs
    STRUCT_GAIN,
    STRUCT_VOLUME,
    STRUCT_LOUDNESS,
    STRUCT_BIQUAD,
    STRUCT_CONVOLUTION,
    STRUCT_DELAY,
    STRUCT_PEQ_BAND,
    STRUCT_BIQUAD_COMBO,
    STRUCT_DIFFEQ,
    STRUCT_DITHER,
    STRUCT_CLIPPER,
    STRUCT_LOOKAHEAD_LIMITER_FILTER,
    UNION_FILTER,

    # Pipeline
    STRUCT_PIPELINE_STEP,

    # Backend Devices
    STRUCT_GENERATOR_SIGNAL,
    STRUCT_COREAUDIO_CAPTURE,
    STRUCT_COREAUDIO_PLAYBACK,
    STRUCT_ALSA_CAPTURE,
    STRUCT_ALSA_PLAYBACK,
    STRUCT_PIPEWIRE_CAPTURE,
    STRUCT_PIPEWIRE_PLAYBACK,
    STRUCT_STDIN_CAPTURE,
    STRUCT_STDOUT_PLAYBACK,
    STRUCT_WASAPI_CAPTURE,
    STRUCT_WASAPI_PLAYBACK,
    STRUCT_ASIO_CAPTURE,
    STRUCT_ASIO_PLAYBACK,
    STRUCT_WAV_FILE_CAPTURE,
    STRUCT_RAW_FILE_CAPTURE,
    STRUCT_RAW_FILE_PLAYBACK,
    STRUCT_GENERATOR_CAPTURE,

    # Device & DSP Config
    UNION_CAPTURE,
    UNION_PLAYBACK,
    STRUCT_DEVICES,
    STRUCT_DSP_CONFIG,
]
