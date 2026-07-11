#ifndef DSP_CONFIG_TYPES_H
#define DSP_CONFIG_TYPES_H

#include "config/BiquadCoefficients.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

enum class Fader {
    Main = 0,
    Aux1 = 1,
    Aux2 = 2,
    Aux3 = 3,
    Aux4 = 4
};

std::string faderToString(Fader fader);
Fader stringToFader(const std::string& str);

enum class ProcessingState {
    Inactive,
    Starting,
    Running,
    Paused,
    Stalled
};

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

enum class AudioBackendType {
    CoreAudio,
    RawFile,
    WavFile,
    SignalGenerator
};

std::string audioBackendTypeToString(AudioBackendType type);
AudioBackendType stringToAudioBackendType(const std::string& str);

enum class SDMFilter {
    Clans4, SDM4, Clans5, SDM5, Clans6, SDM6, Clans7, SDM7, Clans8, SDM8
};
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

struct GeneratorConfig {
    std::string type = "Sine";
    std::optional<double> freq = 1000.0;
    double level = -6.0;
    QJsonObject toJson() const;
};

struct CoreAudioCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    QJsonObject toJson() const;
};

struct CoreAudioPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dopEncoderFilter;
    QJsonObject toJson() const;
};

struct WavFileCaptureConfig {
    std::string filename;
    std::optional<int> extraSamples;
    QJsonObject toJson() const;
};

struct RawFileCaptureConfig {
    int channels = 2;
    std::string filename;
    std::string format = "S16_LE";
    std::optional<int> skipBytes;
    std::optional<int> readBytes;
    std::optional<int> extraSamples;
    QJsonObject toJson() const;
};

struct RawFilePlaybackConfig {
    int channels = 2;
    std::string filename;
    std::string format = "S16_LE";
    std::optional<bool> wavHeader;
    QJsonObject toJson() const;
};

struct GeneratorCaptureConfig {
    int channels = 2;
    GeneratorConfig signal;
    QJsonObject toJson() const;
};

struct CaptureDeviceConfig {
    AudioBackendType backend = AudioBackendType::CoreAudio;
    CoreAudioCaptureConfig coreAudio;
    WavFileCaptureConfig wavFile;
    RawFileCaptureConfig rawFile;
    GeneratorCaptureConfig generator;

    QJsonObject toJson() const;
};

struct PlaybackDeviceConfig {
    AudioBackendType backend = AudioBackendType::CoreAudio;
    CoreAudioPlaybackConfig coreAudio;
    RawFilePlaybackConfig rawFile;

    QJsonObject toJson() const;
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
};

enum class FilterType {
    Gain, Volume, Loudness, Biquad, Conv, Delay, BiquadCombo, DiffEq, Dither, Limiter, LookaheadLimiter
};
std::string filterTypeToString(FilterType t);

struct GainParameters {
    std::optional<double> gain;
    std::optional<GainScale> scale;
    std::optional<bool> inverted;
    std::optional<bool> mute;
    QJsonObject toJson() const;
};

struct LoudnessParameters {
    std::optional<double> referenceLevel;
    std::optional<double> highBoost;
    std::optional<double> lowBoost;
    std::optional<bool> attenuateMid;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
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
};

struct DelayParameters {
    double delay = 0.0;
    std::optional<DelayUnit> unit;
    std::optional<bool> subsample;
    QJsonObject toJson() const;
};

enum class BiquadComboType {
    ButterworthHighpass, ButterworthLowpass, LinkwitzRileyHighpass, LinkwitzRileyLowpass, Tilt, FivePointPeq, GraphicEqualizer
};
std::string biquadComboTypeToString(BiquadComboType t);

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
};

struct DiffEqParameters {
    std::vector<double> a;
    std::vector<double> b;
    QJsonObject toJson() const;
};

enum class DitherType {
    None, Flat, Highpass, Fweighted441, FweightedLong441, FweightedShort441, Gesemann441, Gesemann48, Lipshitz441, LipshitzLong441, Shibata441, ShibataHigh441, ShibataLow441, Shibata48, ShibataHigh48, ShibataLow48, Shibata882, ShibataLow882, Shibata96, ShibataLow96, Shibata192, ShibataLow192
};
std::string ditherTypeToString(DitherType t);

struct DitherParameters {
    DitherType type = DitherType::Flat;
    int bits = 16;
    std::optional<double> amplitude;
    QJsonObject toJson() const;
};

struct LimiterParameters {
    double clipLimit = 0.0;
    std::optional<bool> softClip;
    QJsonObject toJson() const;
};

struct LookaheadLimiterParameters {
    double limit = 0.0;
    double attack = 5.0;
    double release = 100.0;
    std::optional<DelayUnit> unit;
    QJsonObject toJson() const;
};

struct VolumeParameters {
    std::optional<double> rampTime;
    std::optional<double> limit;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
};

struct FilterConfig {
    FilterType type;
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
};

struct MixerSource {
    int channel = 0;
    std::optional<double> gain;
    std::optional<bool> inverted;
    std::optional<bool> mute;
    std::optional<GainScale> scale;
    double gainValue() const { return gain.value_or(0.0); }
    QJsonObject toJson() const;
};

struct MixerMapping {
    int dest = 0;
    std::vector<MixerSource> sources;
    std::optional<bool> mute;
    QJsonObject toJson() const;
};

struct MixerConfig {
    int channelsIn = 2;
    int channelsOut = 2;
    std::vector<MixerMapping> mapping;
    std::optional<std::string> description;
    QJsonObject toJson() const;
};

enum class ProcessorType { Compressor, NoiseGate, RACE };
std::string processorTypeToString(ProcessorType t);

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
};

struct ProcessorConfig {
    ProcessorType type;
    CompressorParameters compressorParams;
    NoiseGateParameters noiseGateParams;
    RACEParameters raceParams;
    QJsonObject toJson() const;
};

enum class PipelineStepType { Filter, Mixer, Processor };

struct PipelineStep {
    PipelineStepType type;
    std::optional<int> channel;
    std::vector<int> channels;
    std::optional<std::string> name;
    std::vector<std::string> names;
    std::optional<bool> bypassed;

    QJsonObject toJson() const;
};

struct DSPConfiguration {
    DevicesConfig devices;
    std::map<std::string, FilterConfig> filters;
    std::map<std::string, MixerConfig> mixers;
    std::map<std::string, ProcessorConfig> processors;
    std::vector<PipelineStep> pipeline;

    std::string toJsonString() const;
    QJsonObject toJsonObject() const;
    void validate() const;
};

#endif // DSP_CONFIG_TYPES_H
