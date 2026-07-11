#ifndef DSP_CONFIG_TYPES_H
#define DSP_CONFIG_TYPES_H

#include "config/BiquadCoefficients.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class Fader { Main = 0, Aux1 = 1, Aux2 = 2, Aux3 = 3, Aux4 = 4 };

std::string faderToString(Fader fader);
Fader stringToFader(const std::string& str);

enum class ProcessingState { Inactive, Starting, Running, Paused, Stalled };

std::string processingStateToString(ProcessingState state);
ProcessingState uint8ToProcessingState(uint8_t rawByte);

enum class StopReasonType {
    None,
    Done,
    CaptureError,
    PlaybackError,
    CaptureFormatChange,
    PlaybackFormatChange,
    UnknownError
};

struct ProcessingStopReason {
    StopReasonType type = StopReasonType::None;
    std::string message;
    int formatChangeRate = 0;
};

struct StateUpdate {
    ProcessingState state = ProcessingState::Inactive;
    ProcessingStopReason stopReason;
};

struct AudioDevice {
    std::string name;
};

struct VuLevels {
    std::vector<float> playback_rms;
    std::vector<float> playback_peak;
    std::vector<float> capture_rms;
    std::vector<float> capture_peak;
};

struct SpectrumData {
    std::vector<float> frequencies;
    std::vector<float> magnitudes;
};

struct AudioSamplesData {
    std::vector<std::vector<float>> channels;
    std::vector<float> left() const { return !channels.empty() ? channels[0] : std::vector<float>(); }
    std::vector<float> right() const { return channels.size() > 1 ? channels[1] : left(); }
};

struct SamplerateCapability {
    int samplerate = 0;
    std::vector<std::string> formats;
};

struct ChannelCapability {
    int channels = 0;
    std::vector<SamplerateCapability> samplerates;
};

struct DeviceCapabilitySet {
    std::vector<ChannelCapability> capabilities;
};

struct AudioDeviceDescriptor {
    std::string name;
    std::vector<DeviceCapabilitySet> capability_sets;
};

enum class AudioBackendType { CoreAudio, WASAPI, ASIO, ALSA, PulseAudio, RawFile, WavFile, SignalGenerator };

inline bool isHardwareBackend(AudioBackendType type) {
    return type == AudioBackendType::CoreAudio || type == AudioBackendType::WASAPI || type == AudioBackendType::ASIO ||
           type == AudioBackendType::ALSA || type == AudioBackendType::PulseAudio;
}

std::string audioBackendTypeToString(AudioBackendType type);
AudioBackendType stringToAudioBackendType(const std::string& str);

enum class SDMFilter { Clans4, SDM4, Clans5, SDM5, Clans6, SDM6, Clans7, SDM7, Clans8, SDM8 };
std::string sdmFilterToString(SDMFilter f);
SDMFilter stringToSDMFilter(const std::string& str);

enum class DelayUnit { ms, us, samples, mm };
std::string delayUnitToString(DelayUnit unit);
DelayUnit stringToDelayUnit(const std::string& str);
double delayUnitToSamples(DelayUnit unit, double delay, double sampleRate);

enum class GainScale { dB, linear };
std::string gainScaleToString(GainScale s);
GainScale stringToGainScale(const std::string& str);

enum class ResamplerType { Synchronous, Apple, AsyncSinc, AsyncPoly };
std::string resamplerTypeToString(ResamplerType t);
ResamplerType stringToResamplerType(const std::string& str);

enum class ResamplerProfile { VeryFast, Fast, Balanced, Accurate };
std::string resamplerProfileToString(ResamplerProfile p);
ResamplerProfile stringToResamplerProfile(const std::string& str);

enum class ResamplerInterpolation { Linear, Cubic, Quintic, Septic };
enum class SincInterpolation { Nearest, Linear, Quadratic, Cubic };
enum class AppleResamplerQuality { Min, Low, Medium, High, Max };
enum class AppleResamplerComplexity { Linear, Normal, Mastering, MinimumPhase };

enum class ConfigErrorType { ParseError, ValidationError, InvalidFilter, InvalidMixer, InvalidPipeline };

struct ConfigError {
    ConfigErrorType type = ConfigErrorType::ValidationError;
    std::string message;
};

struct GeneratorConfig {
    std::string type = "Sine";
    std::optional<double> freq = 1000.0;
    double level = -6.0;
    QJsonObject toJson() const;
    static GeneratorConfig fromJson(const QJsonObject& json);
};

struct CoreAudioCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static CoreAudioCaptureConfig fromJson(const QJsonObject& json);
};

struct CoreAudioPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dopEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static CoreAudioPlaybackConfig fromJson(const QJsonObject& json);
};

struct WASAPICaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> loopback;
    std::optional<bool> polling;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static WASAPICaptureConfig fromJson(const QJsonObject& json);
};

struct WASAPIPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> polling;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dopEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static WASAPIPlaybackConfig fromJson(const QJsonObject& json);
};

struct ASIOCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ASIOCaptureConfig fromJson(const QJsonObject& json);
};

struct ASIOPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dopEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ASIOPlaybackConfig fromJson(const QJsonObject& json);
};

struct ALSACaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> stopOnInactive;
    std::optional<std::string> linkVolumeControl;
    std::optional<std::string> linkMuteControl;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ALSACaptureConfig fromJson(const QJsonObject& json);
};

struct ALSAPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> stopOnInactive;
    std::optional<std::string> linkVolumeControl;
    std::optional<std::string> linkMuteControl;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ALSAPlaybackConfig fromJson(const QJsonObject& json);
};

struct PulseAudioCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> stopOnInactive;
    std::optional<std::string> linkVolumeControl;
    std::optional<std::string> linkMuteControl;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static PulseAudioCaptureConfig fromJson(const QJsonObject& json);
};

struct PulseAudioPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> stopOnInactive;
    std::optional<std::string> linkVolumeControl;
    std::optional<std::string> linkMuteControl;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static PulseAudioPlaybackConfig fromJson(const QJsonObject& json);
};

struct WavFileCaptureConfig {
    std::string filename;
    std::optional<int> extraSamples;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static WavFileCaptureConfig fromJson(const QJsonObject& json);
};

struct RawFileCaptureConfig {
    int channels = 2;
    std::string filename;
    std::string format = "S16_LE";
    std::optional<int> skipBytes;
    std::optional<int> readBytes;
    std::optional<int> extraSamples;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static RawFileCaptureConfig fromJson(const QJsonObject& json);
};

struct RawFilePlaybackConfig {
    int channels = 2;
    std::string filename;
    std::string format = "S16_LE";
    std::optional<bool> wavHeader;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static RawFilePlaybackConfig fromJson(const QJsonObject& json);
};

struct GeneratorCaptureConfig {
    int channels = 2;
    GeneratorConfig signal;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static GeneratorCaptureConfig fromJson(const QJsonObject& json);
};

struct CaptureDeviceConfig {
    AudioBackendType backend = AudioBackendType::CoreAudio;
    CoreAudioCaptureConfig coreAudio;
    WASAPICaptureConfig wasapi;
    ASIOCaptureConfig asio;
    ALSACaptureConfig alsa;
    PulseAudioCaptureConfig pulseAudio;
    WavFileCaptureConfig wavFile;
    RawFileCaptureConfig rawFile;
    GeneratorCaptureConfig generator;

    QJsonObject toJson() const;
    static CaptureDeviceConfig fromJson(const QJsonObject& json);
};

struct PlaybackDeviceConfig {
    AudioBackendType backend = AudioBackendType::CoreAudio;
    CoreAudioPlaybackConfig coreAudio;
    WASAPIPlaybackConfig wasapi;
    ASIOPlaybackConfig asio;
    ALSAPlaybackConfig alsa;
    PulseAudioPlaybackConfig pulseAudio;
    RawFilePlaybackConfig rawFile;

    QJsonObject toJson() const;
    static PlaybackDeviceConfig fromJson(const QJsonObject& json);
};

struct ResamplerConfig {
    ResamplerType type = ResamplerType::Synchronous;
    std::optional<std::string> profile;
    std::optional<std::string> interpolation;
    std::optional<AppleResamplerQuality> appleQuality;
    std::optional<AppleResamplerComplexity> appleComplexity;
    std::optional<int> sincLen;
    std::optional<int> oversamplingFactor;
    std::optional<std::string> window;
    std::optional<double> fCutoff;

    QJsonObject toJson() const;
    static ResamplerConfig fromJson(const QJsonObject& json);
};

struct DevicesConfig {
    int samplerate = 48000;
    int chunksize = 1024;
    std::optional<bool> enableRateAdjust;
    std::optional<int> targetLevel;
    std::optional<double> adjustPeriod;
    std::optional<ResamplerConfig> resampler;
    CaptureDeviceConfig capture;
    PlaybackDeviceConfig playback;
    std::optional<int> captureSamplerate;
    std::optional<double> silenceThreshold;
    std::optional<double> silenceTimeout;
    std::optional<double> volumeRampTime;
    std::optional<double> volumeLimit;
    std::optional<int> queuelimit;
    std::optional<bool> stopOnRateChange;
    std::optional<double> rateMeasureInterval;
    std::optional<bool> multithreaded;
    std::optional<int> workerThreads;

    QJsonObject toJson() const;
    static DevicesConfig fromJson(const QJsonObject& json);
};

enum class FilterType {
    Gain,
    Volume,
    Loudness,
    Biquad,
    Conv,
    Delay,
    BiquadCombo,
    DiffEq,
    Dither,
    Limiter,
    LookaheadLimiter
};
std::string filterTypeToString(FilterType t);
FilterType stringToFilterType(const std::string& str);

struct GainParameters {
    std::optional<double> gain;
    std::optional<GainScale> scale;
    std::optional<bool> inverted;
    std::optional<bool> mute;
    QJsonObject toJson() const;
    static GainParameters fromJson(const QJsonObject& json);
};

struct LoudnessParameters {
    std::optional<double> referenceLevel;
    std::optional<double> highBoost;
    std::optional<double> lowBoost;
    std::optional<bool> attenuateMid;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
    static LoudnessParameters fromJson(const QJsonObject& json);
};

enum class ConvType { Values, Wav, Raw, Dummy };

struct ConvParameters {
    ConvType type = ConvType::Raw;
    std::vector<double> values;
    std::string filename;
    std::string format;
    std::optional<int> channel;
    std::optional<int> length;
    std::optional<int> skipBytesLines;
    std::optional<int> readBytesLines;
    QJsonObject toJson() const;
    static ConvParameters fromJson(const QJsonObject& json);
};

struct DelayParameters {
    double delay = 0.0;
    std::optional<DelayUnit> unit;
    std::optional<bool> subsample;
    QJsonObject toJson() const;
    static DelayParameters fromJson(const QJsonObject& json);
};

enum class BiquadComboType {
    ButterworthHighpass,
    ButterworthLowpass,
    LinkwitzRileyHighpass,
    LinkwitzRileyLowpass,
    Tilt,
    FivePointPeq,
    GraphicEqualizer
};
std::string biquadComboTypeToString(BiquadComboType t);
BiquadComboType stringToBiquadComboType(const std::string& str);

struct BiquadComboParameters {
    BiquadComboType type = BiquadComboType::ButterworthLowpass;
    std::optional<double> freq;
    std::optional<int> order;
    std::optional<double> gain;
    std::optional<double> fls, qls, gls;
    std::optional<double> fp1, qp1, gp1;
    std::optional<double> fp2, qp2, gp2;
    std::optional<double> fp3, qp3, gp3;
    std::optional<double> fhs, qhs, ghs;
    std::optional<double> freqMin, freqMax;
    std::vector<double> gains;
    QJsonObject toJson() const;
    static BiquadComboParameters fromJson(const QJsonObject& json);
};

struct DiffEqParameters {
    std::vector<double> a;
    std::vector<double> b;
    QJsonObject toJson() const;
    static DiffEqParameters fromJson(const QJsonObject& json);
};

enum class DitherType {
    None,
    Flat,
    Highpass,
    Fweighted441,
    FweightedLong441,
    FweightedShort441,
    Gesemann441,
    Gesemann48,
    Lipshitz441,
    LipshitzLong441,
    Shibata441,
    ShibataHigh441,
    ShibataLow441,
    Shibata48,
    ShibataHigh48,
    ShibataLow48,
    Shibata882,
    ShibataLow882,
    Shibata96,
    ShibataLow96,
    Shibata192,
    ShibataLow192
};
std::string ditherTypeToString(DitherType t);
DitherType stringToDitherType(const std::string& str);

struct DitherParameters {
    DitherType type = DitherType::Flat;
    int bits = 16;
    std::optional<double> amplitude;
    QJsonObject toJson() const;
    static DitherParameters fromJson(const QJsonObject& json);
};

struct LimiterParameters {
    double clipLimit = 0.0;
    std::optional<bool> softClip;
    QJsonObject toJson() const;
    static LimiterParameters fromJson(const QJsonObject& json);
};

struct LookaheadLimiterParameters {
    double limit = 0.0;
    double attack = 5.0;
    double release = 100.0;
    std::optional<DelayUnit> unit;
    QJsonObject toJson() const;
    static LookaheadLimiterParameters fromJson(const QJsonObject& json);
};

struct VolumeParameters {
    std::optional<double> rampTime;
    std::optional<double> limit;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
    static VolumeParameters fromJson(const QJsonObject& json);
};

struct FilterConfig {
    FilterType type = FilterType::Gain;
    GainParameters gainParams;
    VolumeParameters volumeParams;
    LoudnessParameters loudnessParams;
    BiquadParameters biquadParams;
    ConvParameters convParams;
    DelayParameters delayParams;
    BiquadComboParameters comboParams;
    DiffEqParameters diffEqParams;
    DitherParameters ditherParams;
    LimiterParameters limiterParams;
    LookaheadLimiterParameters lookaheadParams;

    QJsonObject toJson() const;
    static FilterConfig fromJson(const QJsonObject& json);
};

struct MixerSource {
    int channel = 0;
    std::optional<double> gain;
    std::optional<bool> inverted;
    std::optional<bool> mute;
    std::optional<GainScale> scale;
    double gainValue() const { return gain.value_or(0.0); }
    QJsonObject toJson() const;
    static MixerSource fromJson(const QJsonObject& json);
};

struct MixerMapping {
    int dest = 0;
    std::vector<MixerSource> sources;
    std::optional<bool> mute;
    QJsonObject toJson() const;
    static MixerMapping fromJson(const QJsonObject& json);
};

struct MixerConfig {
    int channelsIn = 2;
    int channelsOut = 2;
    std::vector<MixerMapping> mapping;
    std::optional<std::string> description;
    std::vector<std::string> labels;
    QJsonObject toJson() const;
    static MixerConfig fromJson(const QJsonObject& json);
};

enum class ProcessorType { Compressor, NoiseGate, RACE };
std::string processorTypeToString(ProcessorType t);
ProcessorType stringToProcessorType(const std::string& str);

struct CompressorParameters {
    int channels = 2;
    std::vector<int> monitorChannels;
    std::vector<int> processChannels;
    double attack = 5.0;
    double release = 100.0;
    double threshold = -20.0;
    double factor = 2.0;
    std::optional<double> makeupGain;
    std::optional<bool> softClip;
    std::optional<double> clipLimit;
    QJsonObject toJson() const;
    static CompressorParameters fromJson(const QJsonObject& json);
};

struct NoiseGateParameters {
    int channels = 2;
    std::vector<int> monitorChannels;
    std::vector<int> processChannels;
    double attack = 5.0;
    double release = 100.0;
    double threshold = -60.0;
    double attenuation = -40.0;
    QJsonObject toJson() const;
    static NoiseGateParameters fromJson(const QJsonObject& json);
};

struct RACEParameters {
    int channels = 2;
    int channelA = 0;
    int channelB = 1;
    double delay = 0.25;
    std::optional<bool> subsampleDelay;
    std::optional<DelayUnit> delayUnit;
    double attenuation = 6.0;
    QJsonObject toJson() const;
    static RACEParameters fromJson(const QJsonObject& json);
};

struct ProcessorConfig {
    ProcessorType type = ProcessorType::Compressor;
    CompressorParameters compressorParams;
    NoiseGateParameters noiseGateParams;
    RACEParameters raceParams;
    QJsonObject toJson() const;
    static ProcessorConfig fromJson(const QJsonObject& json);
};

enum class PipelineStepType { Filter, Mixer, Processor };

struct PipelineStep {
    PipelineStepType type = PipelineStepType::Filter;
    std::optional<int> channel;
    std::vector<int> channels;
    std::optional<std::string> name;
    std::vector<std::string> names;
    std::optional<bool> bypassed;

    QJsonObject toJson() const;
    static PipelineStep fromJson(const QJsonObject& json);
};

struct DSPConfiguration {
    DevicesConfig devices;
    std::map<std::string, FilterConfig> filters;
    std::map<std::string, MixerConfig> mixers;
    std::map<std::string, ProcessorConfig> processors;
    std::vector<PipelineStep> pipeline;

    std::string toJsonString() const;
    QJsonObject toJsonObject() const;
    static DSPConfiguration fromJsonObject(const QJsonObject& json);
    static DSPConfiguration fromJsonString(const std::string& jsonStr);
    void validate() const;
};

#endif // DSP_CONFIG_TYPES_H
