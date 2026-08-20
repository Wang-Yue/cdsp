#ifndef DSP_CONFIG_TYPES_H
#define DSP_CONFIG_TYPES_H

#include "config/BiquadCoefficients.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum class Fader { Main = 0, Aux1 = 1, Aux2 = 2, Aux3 = 3, Aux4 = 4 };

extern const std::vector<int> MONITOR_STANDARD_RATES;

std::string faderToString(Fader fader);
Fader stringToFader(const std::string& str);

enum class ProcessingState { Inactive, Starting, Running, Paused, Stalled };

#include "models/LogManager.h"

std::string processingStateToString(ProcessingState state);
ProcessingState uint8ToProcessingState(uint8_t rawByte);
uint8_t processingStateToUint8(ProcessingState state);

std::string logLevelToStdString(LogLevel level);
LogLevel stdStringToLogLevel(const std::string& str);
uint8_t logLevelToRawByte(LogLevel level);
LogLevel rawByteToLogLevel(uint8_t rawByte);

enum class AudioBackendErrorType {
    ConfigParse,
    CommandSend,
    InvalidSamplerate,
    SpectrumCompute,
    EngineNotRunning,
    BufferEmpty
};

struct AudioBackendError {
    AudioBackendErrorType type = AudioBackendErrorType::EngineNotRunning;
    std::string message;
};

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

    bool operator==(const ProcessingStopReason& other) const {
        return type == other.type && message == other.message && formatChangeRate == other.formatChangeRate;
    }
    bool operator!=(const ProcessingStopReason& other) const { return !(*this == other); }
};

struct StateUpdate {
    ProcessingState state = ProcessingState::Inactive;
    ProcessingStopReason stopReason;

    bool operator==(const StateUpdate& other) const { return state == other.state && stopReason == other.stopReason; }
    bool operator!=(const StateUpdate& other) const { return !(*this == other); }
};

struct AudioDevice {
    std::string id;
    std::string name;

    bool operator==(const AudioDevice& other) const { return id == other.id && name == other.name; }
    bool operator!=(const AudioDevice& other) const { return !(*this == other); }
};

struct VuLevels {
    std::vector<float> playback_rms;
    std::vector<float> playback_peak;
    std::vector<float> capture_rms;
    std::vector<float> capture_peak;

    bool operator==(const VuLevels& other) const {
        return playback_rms == other.playback_rms && playback_peak == other.playback_peak &&
               capture_rms == other.capture_rms && capture_peak == other.capture_peak;
    }
    bool operator!=(const VuLevels& other) const { return !(*this == other); }
};

struct SpectrumData {
    std::vector<float> frequencies;
    std::vector<float> magnitudes;
    QDateTime timestamp = QDateTime::currentDateTime();

    bool operator==(const SpectrumData& other) const {
        return frequencies == other.frequencies && magnitudes == other.magnitudes && timestamp == other.timestamp;
    }
    bool operator!=(const SpectrumData& other) const { return !(*this == other); }
};

using Spectrum = SpectrumData;

struct AudioSamplesData {
    std::vector<std::vector<float>> channels;

    const std::vector<float>& left() const {
        static const std::vector<float> empty;
        return !channels.empty() ? channels[0] : empty;
    }

    const std::vector<float>& right() const {
        static const std::vector<float> empty;
        return channels.size() > 1 ? channels[1] : left();
    }

    bool operator==(const AudioSamplesData& other) const { return channels == other.channels; }
    bool operator!=(const AudioSamplesData& other) const { return !(*this == other); }
};

using AudioSamples = AudioSamplesData;

struct SamplerateCapability {
    int samplerate = 0;
    std::vector<std::string> formats;
    QJsonObject toJson() const;
    static SamplerateCapability fromJson(const QJsonObject& json);
    bool operator==(const SamplerateCapability& other) const {
        return samplerate == other.samplerate && formats == other.formats;
    }
    bool operator!=(const SamplerateCapability& other) const { return !(*this == other); }
};

struct ChannelCapability {
    int channels = 0;
    std::vector<SamplerateCapability> samplerates;
    QJsonObject toJson() const;
    static ChannelCapability fromJson(const QJsonObject& json);
    bool operator==(const ChannelCapability& other) const {
        return channels == other.channels && samplerates == other.samplerates;
    }
    bool operator!=(const ChannelCapability& other) const { return !(*this == other); }
};

struct DeviceCapabilitySet {
    std::string mode;
    std::vector<ChannelCapability> capabilities;
    QJsonObject toJson() const;
    static DeviceCapabilitySet fromJson(const QJsonObject& json);
    bool operator==(const DeviceCapabilitySet& other) const {
        return mode == other.mode && capabilities == other.capabilities;
    }
    bool operator!=(const DeviceCapabilitySet& other) const { return !(*this == other); }
};

struct AudioDeviceDescriptor {
    std::string name;
    std::vector<DeviceCapabilitySet> capability_sets;
    QJsonObject toJson() const;
    static AudioDeviceDescriptor fromJson(const QJsonObject& json);
    bool operator==(const AudioDeviceDescriptor& other) const {
        return name == other.name && capability_sets == other.capability_sets;
    }
    bool operator!=(const AudioDeviceDescriptor& other) const { return !(*this == other); }
};

enum class AudioBackendType {
#if defined(ENABLE_COREAUDIO)
    CoreAudio,
#endif
#if defined(ENABLE_WASAPI)
    WASAPI,
#endif
#if defined(ENABLE_ASIO)
    ASIO,
#endif
#if defined(ENABLE_ALSA)
    ALSA,
#endif
#if defined(ENABLE_PIPEWIRE)
    PipeWire,
#endif
    RawFile,
    WavFile,
    SignalGenerator
};

inline bool isHardwareBackend(AudioBackendType type) {
    switch (type) {
#if defined(ENABLE_COREAUDIO)
    case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
    case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
    case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
    case AudioBackendType::ALSA:
#endif
#if defined(ENABLE_PIPEWIRE)
    case AudioBackendType::PipeWire:
#endif
        return true;
    default:
        return false;
    }
}

inline bool backendHasDeviceList(AudioBackendType type) {
    switch (type) {
#if defined(ENABLE_COREAUDIO)
    case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
    case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
    case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
    case AudioBackendType::ALSA:
#endif
        return true;
    default:
        return false;
    }
}

inline bool backendHasDeviceCapabilities(AudioBackendType type) {
    switch (type) {
#if defined(ENABLE_COREAUDIO)
    case AudioBackendType::CoreAudio:
#endif
#if defined(ENABLE_WASAPI)
    case AudioBackendType::WASAPI:
#endif
#if defined(ENABLE_ASIO)
    case AudioBackendType::ASIO:
#endif
#if defined(ENABLE_ALSA)
    case AudioBackendType::ALSA:
#endif
        return true;
    default:
        return false;
    }
}

std::string audioBackendTypeToString(AudioBackendType type);
AudioBackendType stringToAudioBackendType(const std::string& str);

enum class SDMFilter { Clans4, SDM4, Clans5, SDM5, Clans6, SDM6, Clans7, SDM7, Clans8, SDM8 };
std::string sdmFilterToString(SDMFilter f);
SDMFilter stringToSDMFilter(const std::string& str);

enum class TimeUnit { ms, us, s, samples };
std::string timeUnitToString(TimeUnit u);
TimeUnit stringToTimeUnit(const std::string& str);
double timeUnitToSamples(TimeUnit u, double val, double sampleRate);

enum class DelayUnit { ms, us, s, samples, mm };
std::string delayUnitToString(DelayUnit unit);
DelayUnit stringToDelayUnit(const std::string& str);
double delayUnitToSamples(DelayUnit unit, double delay, double sampleRate);

enum class GainScale { dB, linear };
std::string gainScaleToString(GainScale s);
GainScale stringToGainScale(const std::string& str);

enum class ResamplerType { Synchronous, AsyncSinc, AsyncPoly, Slip };
std::string resamplerTypeToString(ResamplerType t);
ResamplerType stringToResamplerType(const std::string& str);

enum class ResamplerProfile { VeryFast, Fast, Balanced, Accurate };
std::string resamplerProfileToString(ResamplerProfile p);
ResamplerProfile stringToResamplerProfile(const std::string& str);

enum class ResamplerInterpolation { Linear, Cubic, Quintic, Septic };
enum class SincInterpolation { Nearest, Linear, Quadratic, Cubic };

std::string resamplerInterpolationToString(ResamplerInterpolation interp);
ResamplerInterpolation stringToResamplerInterpolation(const std::string& str);
std::string sincInterpolationToString(SincInterpolation interp);
SincInterpolation stringToSincInterpolation(const std::string& str);

enum class ConfigErrorType { ParseError, ValidationError, InvalidFilter, InvalidMixer, InvalidPipeline };

struct ConfigError {
    ConfigErrorType type = ConfigErrorType::ValidationError;
    std::string message;

    bool operator==(const ConfigError& other) const { return type == other.type && message == other.message; }
    bool operator!=(const ConfigError& other) const { return !(*this == other); }
};

struct GeneratorConfig {
    std::string type = "Sine";
    std::optional<double> freq = 1000.0;
    double level = -6.0;
    QJsonObject toJson() const;
    static GeneratorConfig fromJson(const QJsonObject& json);

    bool operator==(const GeneratorConfig& other) const {
        return type == other.type && freq == other.freq && level == other.level;
    }
    bool operator!=(const GeneratorConfig& other) const { return !(*this == other); }
};

#if defined(ENABLE_COREAUDIO)
struct CoreAudioCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static CoreAudioCaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const CoreAudioCaptureConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && bypassDoP == o.bypassDoP &&
               dopCutoffHz == o.dopCutoffHz && channelLabels == o.channelLabels;
    }
    bool operator!=(const CoreAudioCaptureConfig& o) const { return !(*this == o); }
};

struct CoreAudioPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dsdEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static CoreAudioPlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const CoreAudioPlaybackConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && exclusive == o.exclusive &&
               outputDoP == o.outputDoP && dsdEncoderFilter == o.dsdEncoderFilter && channelLabels == o.channelLabels;
    }
    bool operator!=(const CoreAudioPlaybackConfig& o) const { return !(*this == o); }
};
#endif

#if defined(ENABLE_WASAPI)
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

    bool operator==(const WASAPICaptureConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && exclusive == o.exclusive &&
               loopback == o.loopback && polling == o.polling && bypassDoP == o.bypassDoP &&
               dopCutoffHz == o.dopCutoffHz && channelLabels == o.channelLabels;
    }
    bool operator!=(const WASAPICaptureConfig& o) const { return !(*this == o); }
};

struct WASAPIPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> exclusive;
    std::optional<bool> polling;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dsdEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static WASAPIPlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const WASAPIPlaybackConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && exclusive == o.exclusive &&
               polling == o.polling && outputDoP == o.outputDoP && dsdEncoderFilter == o.dsdEncoderFilter &&
               channelLabels == o.channelLabels;
    }
    bool operator!=(const WASAPIPlaybackConfig& o) const { return !(*this == o); }
};
#endif

#if defined(ENABLE_ASIO)
struct ASIOCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> bypassDoP;
    std::optional<double> dopCutoffHz;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ASIOCaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const ASIOCaptureConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && bypassDoP == o.bypassDoP &&
               dopCutoffHz == o.dopCutoffHz && channelLabels == o.channelLabels;
    }
    bool operator!=(const ASIOCaptureConfig& o) const { return !(*this == o); }
};

struct ASIOPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dsdEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ASIOPlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const ASIOPlaybackConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && outputDoP == o.outputDoP &&
               dsdEncoderFilter == o.dsdEncoderFilter && channelLabels == o.channelLabels;
    }
    bool operator!=(const ASIOPlaybackConfig& o) const { return !(*this == o); }
};
#endif

#if defined(ENABLE_ALSA)
struct ALSACaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> stopOnInactive;
    std::optional<std::string> linkVolumeControl;
    std::optional<std::string> linkMuteControl;
    std::optional<bool> threaded;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ALSACaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const ALSACaptureConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format &&
               stopOnInactive == o.stopOnInactive && linkVolumeControl == o.linkVolumeControl &&
               linkMuteControl == o.linkMuteControl && threaded == o.threaded && channelLabels == o.channelLabels;
    }
    bool operator!=(const ALSACaptureConfig& o) const { return !(*this == o); }
};

struct ALSAPlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<bool> threaded;
    std::optional<bool> outputDoP;
    std::optional<SDMFilter> dsdEncoderFilter;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static ALSAPlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const ALSAPlaybackConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && threaded == o.threaded &&
               outputDoP == o.outputDoP && dsdEncoderFilter == o.dsdEncoderFilter && channelLabels == o.channelLabels;
    }
    bool operator!=(const ALSAPlaybackConfig& o) const { return !(*this == o); }
};
#endif

#if defined(ENABLE_PIPEWIRE)
struct PipeWireCaptureConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<std::string> nodeName;
    std::optional<std::string> nodeDescription;
    std::optional<std::string> nodeGroupName;
    std::optional<std::string> autoconnectTo;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static PipeWireCaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const PipeWireCaptureConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && nodeName == o.nodeName &&
               nodeDescription == o.nodeDescription && nodeGroupName == o.nodeGroupName &&
               autoconnectTo == o.autoconnectTo && channelLabels == o.channelLabels;
    }
    bool operator!=(const PipeWireCaptureConfig& o) const { return !(*this == o); }
};

struct PipeWirePlaybackConfig {
    int channels = 2;
    std::optional<std::string> device;
    std::optional<std::string> format;
    std::optional<std::string> nodeName;
    std::optional<std::string> nodeDescription;
    std::optional<std::string> nodeGroupName;
    std::optional<std::string> autoconnectTo;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static PipeWirePlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const PipeWirePlaybackConfig& o) const {
        return channels == o.channels && device == o.device && format == o.format && nodeName == o.nodeName &&
               nodeDescription == o.nodeDescription && nodeGroupName == o.nodeGroupName &&
               autoconnectTo == o.autoconnectTo && channelLabels == o.channelLabels;
    }
    bool operator!=(const PipeWirePlaybackConfig& o) const { return !(*this == o); }
};
#endif

struct WavFileCaptureConfig {
    std::string filename;
    std::optional<int> extraSamples;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static WavFileCaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const WavFileCaptureConfig& o) const {
        return filename == o.filename && extraSamples == o.extraSamples && channelLabels == o.channelLabels;
    }
    bool operator!=(const WavFileCaptureConfig& o) const { return !(*this == o); }
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

    bool operator==(const RawFileCaptureConfig& o) const {
        return channels == o.channels && filename == o.filename && format == o.format && skipBytes == o.skipBytes &&
               readBytes == o.readBytes && extraSamples == o.extraSamples && channelLabels == o.channelLabels;
    }
    bool operator!=(const RawFileCaptureConfig& o) const { return !(*this == o); }
};

struct RawFilePlaybackConfig {
    int channels = 2;
    std::string filename;
    std::string format = "S16_LE";
    std::optional<bool> wavHeader;
    std::optional<bool> useRf64;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static RawFilePlaybackConfig fromJson(const QJsonObject& json);

    bool operator==(const RawFilePlaybackConfig& o) const {
        return channels == o.channels && filename == o.filename && format == o.format && wavHeader == o.wavHeader &&
               useRf64 == o.useRf64 && channelLabels == o.channelLabels;
    }
    bool operator!=(const RawFilePlaybackConfig& o) const { return !(*this == o); }
};

struct GeneratorCaptureConfig {
    int channels = 2;
    GeneratorConfig signal;
    std::vector<std::string> channelLabels;
    QJsonObject toJson() const;
    static GeneratorCaptureConfig fromJson(const QJsonObject& json);

    bool operator==(const GeneratorCaptureConfig& o) const {
        return channels == o.channels && signal == o.signal && channelLabels == o.channelLabels;
    }
    bool operator!=(const GeneratorCaptureConfig& o) const { return !(*this == o); }
};

struct CaptureDeviceConfig {
    AudioBackendType backend =
#if defined(ENABLE_COREAUDIO)
        AudioBackendType::CoreAudio;
#elif defined(ENABLE_WASAPI)
        AudioBackendType::WASAPI;
#elif defined(ENABLE_ALSA)
        AudioBackendType::ALSA;
#else
        AudioBackendType::RawFile;
#endif

#if defined(ENABLE_COREAUDIO)
    CoreAudioCaptureConfig coreAudio;
#endif
#if defined(ENABLE_WASAPI)
    WASAPICaptureConfig wasapi;
#endif
#if defined(ENABLE_ASIO)
    ASIOCaptureConfig asio;
#endif
#if defined(ENABLE_ALSA)
    ALSACaptureConfig alsa;
#endif
#if defined(ENABLE_PIPEWIRE)
    PipeWireCaptureConfig pipeWire;
#endif
    WavFileCaptureConfig wavFile;
    RawFileCaptureConfig rawFile;
    GeneratorCaptureConfig generator;
    std::optional<std::string> deviceName() const {
        switch (backend) {
#if defined(ENABLE_COREAUDIO)
        case AudioBackendType::CoreAudio:
            return coreAudio.device;
#endif
#if defined(ENABLE_WASAPI)
        case AudioBackendType::WASAPI:
            return wasapi.device;
#endif
#if defined(ENABLE_ASIO)
        case AudioBackendType::ASIO:
            return asio.device;
#endif
#if defined(ENABLE_ALSA)
        case AudioBackendType::ALSA:
            return alsa.device;
#endif
#if defined(ENABLE_PIPEWIRE)
        case AudioBackendType::PipeWire:
            return pipeWire.device;
#endif
        case AudioBackendType::WavFile:
            return wavFile.filename;
        case AudioBackendType::RawFile:
            return rawFile.filename;
        default:
            return "Generator";
        }
    }

    QJsonObject toJson() const;
    static CaptureDeviceConfig fromJson(const QJsonObject& json);

    bool operator==(const CaptureDeviceConfig& o) const {
        return backend == o.backend &&
#if defined(ENABLE_COREAUDIO)
               coreAudio == o.coreAudio &&
#endif
#if defined(ENABLE_WASAPI)
               wasapi == o.wasapi &&
#endif
#if defined(ENABLE_ASIO)
               asio == o.asio &&
#endif
#if defined(ENABLE_ALSA)
               alsa == o.alsa &&
#endif
#if defined(ENABLE_PIPEWIRE)
               pipeWire == o.pipeWire &&
#endif
               wavFile == o.wavFile && rawFile == o.rawFile && generator == o.generator;
    }
    bool operator!=(const CaptureDeviceConfig& o) const { return !(*this == o); }
};

struct PlaybackDeviceConfig {
    AudioBackendType backend =
#if defined(ENABLE_COREAUDIO)
        AudioBackendType::CoreAudio;
#elif defined(ENABLE_WASAPI)
        AudioBackendType::WASAPI;
#elif defined(ENABLE_ALSA)
        AudioBackendType::ALSA;
#else
        AudioBackendType::RawFile;
#endif

#if defined(ENABLE_COREAUDIO)
    CoreAudioPlaybackConfig coreAudio;
#endif
#if defined(ENABLE_WASAPI)
    WASAPIPlaybackConfig wasapi;
#endif
#if defined(ENABLE_ASIO)
    ASIOPlaybackConfig asio;
#endif
#if defined(ENABLE_ALSA)
    ALSAPlaybackConfig alsa;
#endif
#if defined(ENABLE_PIPEWIRE)
    PipeWirePlaybackConfig pipeWire;
#endif
    RawFilePlaybackConfig rawFile;

    std::optional<std::string> deviceName() const {
        switch (backend) {
#if defined(ENABLE_COREAUDIO)
        case AudioBackendType::CoreAudio:
            return coreAudio.device;
#endif
#if defined(ENABLE_WASAPI)
        case AudioBackendType::WASAPI:
            return wasapi.device;
#endif
#if defined(ENABLE_ASIO)
        case AudioBackendType::ASIO:
            return asio.device;
#endif
#if defined(ENABLE_ALSA)
        case AudioBackendType::ALSA:
            return alsa.device;
#endif
#if defined(ENABLE_PIPEWIRE)
        case AudioBackendType::PipeWire:
            return pipeWire.device;
#endif
        case AudioBackendType::RawFile:
            return rawFile.filename;
        default:
            return std::nullopt;
        }
    }

    QJsonObject toJson() const;
    static PlaybackDeviceConfig fromJson(const QJsonObject& json);

    bool operator==(const PlaybackDeviceConfig& o) const {
        return backend == o.backend &&
#if defined(ENABLE_COREAUDIO)
               coreAudio == o.coreAudio &&
#endif
#if defined(ENABLE_WASAPI)
               wasapi == o.wasapi &&
#endif
#if defined(ENABLE_ASIO)
               asio == o.asio &&
#endif
#if defined(ENABLE_ALSA)
               alsa == o.alsa &&
#endif
#if defined(ENABLE_PIPEWIRE)
               pipeWire == o.pipeWire &&
#endif
               rawFile == o.rawFile;
    }
    bool operator!=(const PlaybackDeviceConfig& o) const { return !(*this == o); }
};

struct ResamplerConfig {
    ResamplerType type = ResamplerType::Synchronous;
    std::optional<std::string> profile;
    std::optional<std::string> interpolation;
    std::optional<int> sincLen;
    std::optional<int> oversamplingFactor;
    std::optional<std::string> window;
    std::optional<double> fCutoff;

    QJsonObject toJson() const;
    static ResamplerConfig fromJson(const QJsonObject& json);

    bool operator==(const ResamplerConfig& o) const {
        return type == o.type && profile == o.profile && interpolation == o.interpolation && sincLen == o.sincLen &&
               oversamplingFactor == o.oversamplingFactor && window == o.window && fCutoff == o.fCutoff;
    }
    bool operator!=(const ResamplerConfig& o) const { return !(*this == o); }
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

    bool operator==(const DevicesConfig& o) const {
        return samplerate == o.samplerate && chunksize == o.chunksize && enableRateAdjust == o.enableRateAdjust &&
               targetLevel == o.targetLevel && adjustPeriod == o.adjustPeriod && resampler == o.resampler &&
               capture == o.capture && playback == o.playback && captureSamplerate == o.captureSamplerate &&
               silenceThreshold == o.silenceThreshold && silenceTimeout == o.silenceTimeout &&
               volumeRampTime == o.volumeRampTime && volumeLimit == o.volumeLimit && queuelimit == o.queuelimit &&
               stopOnRateChange == o.stopOnRateChange && rateMeasureInterval == o.rateMeasureInterval &&
               multithreaded == o.multithreaded && workerThreads == o.workerThreads;
    }
    bool operator!=(const DevicesConfig& o) const { return !(*this == o); }
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
    Clipper,
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

    bool operator==(const GainParameters& o) const {
        return gain == o.gain && scale == o.scale && inverted == o.inverted && mute == o.mute;
    }
    bool operator!=(const GainParameters& o) const { return !(*this == o); }
};

struct LoudnessParameters {
    std::optional<double> referenceLevel;
    std::optional<double> highBoost;
    std::optional<double> lowBoost;
    std::optional<bool> attenuateMid;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
    static LoudnessParameters fromJson(const QJsonObject& json);

    bool operator==(const LoudnessParameters& o) const {
        return referenceLevel == o.referenceLevel && highBoost == o.highBoost && lowBoost == o.lowBoost &&
               attenuateMid == o.attenuateMid && fader == o.fader;
    }
    bool operator!=(const LoudnessParameters& o) const { return !(*this == o); }
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

    bool operator==(const ConvParameters& o) const {
        return type == o.type && values == o.values && filename == o.filename && format == o.format &&
               channel == o.channel && length == o.length && skipBytesLines == o.skipBytesLines &&
               readBytesLines == o.readBytesLines;
    }
    bool operator!=(const ConvParameters& o) const { return !(*this == o); }
};

struct DelayParameters {
    double delay = 0.0;
    DelayUnit delayUnit = DelayUnit::ms;
    std::optional<DelayUnit> unit;
    std::optional<bool> subsample;
    QJsonObject toJson() const;
    static DelayParameters fromJson(const QJsonObject& json);

    bool operator==(const DelayParameters& o) const {
        return delay == o.delay && (delayUnit == o.delayUnit || unit == o.unit) && subsample == o.subsample;
    }
    bool operator!=(const DelayParameters& o) const { return !(*this == o); }
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

    bool operator==(const BiquadComboParameters& o) const {
        return type == o.type && freq == o.freq && order == o.order && gain == o.gain && fls == o.fls && qls == o.qls &&
               gls == o.gls && fp1 == o.fp1 && qp1 == o.qp1 && gp1 == o.gp1 && fp2 == o.fp2 && qp2 == o.qp2 &&
               gp2 == o.gp2 && fp3 == o.fp3 && qp3 == o.qp3 && gp3 == o.gp3 && fhs == o.fhs && qhs == o.qhs &&
               ghs == o.ghs && freqMin == o.freqMin && freqMax == o.freqMax && gains == o.gains;
    }
    bool operator!=(const BiquadComboParameters& o) const { return !(*this == o); }
};

struct DiffEqParameters {
    std::vector<double> a;
    std::vector<double> b;
    QJsonObject toJson() const;
    static DiffEqParameters fromJson(const QJsonObject& json);

    bool operator==(const DiffEqParameters& o) const { return a == o.a && b == o.b; }
    bool operator!=(const DiffEqParameters& o) const { return !(*this == o); }
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

    bool operator==(const DitherParameters& o) const {
        return type == o.type && bits == o.bits && amplitude == o.amplitude;
    }
    bool operator!=(const DitherParameters& o) const { return !(*this == o); }
};

struct ClipperParameters {
    double clipLimit = 0.0;
    std::optional<bool> softClip;
    QJsonObject toJson() const;
    static ClipperParameters fromJson(const QJsonObject& json);

    bool operator==(const ClipperParameters& o) const { return clipLimit == o.clipLimit && softClip == o.softClip; }
    bool operator!=(const ClipperParameters& o) const { return !(*this == o); }
};

struct LookaheadLimiterParameters {
    double limit = 0.0;
    double attack = 5.0;
    double release = 100.0;
    TimeUnit attackUnit = TimeUnit::ms;
    TimeUnit releaseUnit = TimeUnit::ms;
    QJsonObject toJson() const;
    static LookaheadLimiterParameters fromJson(const QJsonObject& json);

    bool operator==(const LookaheadLimiterParameters& o) const {
        return limit == o.limit && attack == o.attack && release == o.release && attackUnit == o.attackUnit &&
               releaseUnit == o.releaseUnit;
    }
    bool operator!=(const LookaheadLimiterParameters& o) const { return !(*this == o); }
};

struct VolumeParameters {
    std::optional<double> rampTime;
    std::optional<double> limit;
    std::optional<Fader> fader;
    QJsonObject toJson() const;
    static VolumeParameters fromJson(const QJsonObject& json);

    bool operator==(const VolumeParameters& o) const {
        return rampTime == o.rampTime && limit == o.limit && fader == o.fader;
    }
    bool operator!=(const VolumeParameters& o) const { return !(*this == o); }
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
    ClipperParameters clipperParams;
    LookaheadLimiterParameters lookaheadParams;

    QJsonObject toJson() const;
    static FilterConfig fromJson(const QJsonObject& json);

    bool operator==(const FilterConfig& o) const {
        return type == o.type && gainParams == o.gainParams && volumeParams == o.volumeParams &&
               loudnessParams == o.loudnessParams && biquadParams == o.biquadParams && convParams == o.convParams &&
               delayParams == o.delayParams && comboParams == o.comboParams && diffEqParams == o.diffEqParams &&
               ditherParams == o.ditherParams && clipperParams == o.clipperParams &&
               lookaheadParams == o.lookaheadParams;
    }
    bool operator!=(const FilterConfig& o) const { return !(*this == o); }
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

    bool operator==(const MixerSource& o) const {
        return channel == o.channel && gain == o.gain && inverted == o.inverted && mute == o.mute && scale == o.scale;
    }
    bool operator!=(const MixerSource& o) const { return !(*this == o); }
};

struct MixerMapping {
    int dest = 0;
    std::vector<MixerSource> sources;
    std::optional<bool> mute;
    QJsonObject toJson() const;
    static MixerMapping fromJson(const QJsonObject& json);

    bool operator==(const MixerMapping& o) const { return dest == o.dest && sources == o.sources && mute == o.mute; }
    bool operator!=(const MixerMapping& o) const { return !(*this == o); }
};

struct MixerConfig {
    int channelsIn = 2;
    int channelsOut = 2;
    std::vector<MixerMapping> mapping;
    std::optional<std::string> description;
    std::vector<std::string> labels;
    QJsonObject toJson() const;
    static MixerConfig fromJson(const QJsonObject& json);

    bool operator==(const MixerConfig& o) const {
        return channelsIn == o.channelsIn && channelsOut == o.channelsOut && mapping == o.mapping &&
               description == o.description && labels == o.labels;
    }
    bool operator!=(const MixerConfig& o) const { return !(*this == o); }
};

enum class ProcessorType { Compressor, NoiseGate, RACE, LookaheadLimiter };
std::string processorTypeToString(ProcessorType t);
ProcessorType stringToProcessorType(const std::string& str);

struct CompressorParameters {
    int channels = 2;
    std::vector<int> monitorChannels;
    std::vector<int> processChannels;
    double attack = 5.0;
    TimeUnit attackUnit = TimeUnit::ms;
    double release = 100.0;
    TimeUnit releaseUnit = TimeUnit::ms;
    double threshold = -20.0;
    double factor = 2.0;
    std::optional<double> makeupGain;
    std::optional<bool> softClip;
    std::optional<double> clipLimit;
    QJsonObject toJson() const;
    static CompressorParameters fromJson(const QJsonObject& json);

    bool operator==(const CompressorParameters& o) const {
        return channels == o.channels && monitorChannels == o.monitorChannels && processChannels == o.processChannels &&
               attack == o.attack && attackUnit == o.attackUnit && release == o.release &&
               releaseUnit == o.releaseUnit && threshold == o.threshold && factor == o.factor &&
               makeupGain == o.makeupGain && softClip == o.softClip && clipLimit == o.clipLimit;
    }
    bool operator!=(const CompressorParameters& o) const { return !(*this == o); }
};

struct NoiseGateParameters {
    int channels = 2;
    std::vector<int> monitorChannels;
    std::vector<int> processChannels;
    double attack = 5.0;
    TimeUnit attackUnit = TimeUnit::ms;
    double release = 100.0;
    TimeUnit releaseUnit = TimeUnit::ms;
    double threshold = -60.0;
    double attenuation = -40.0;
    QJsonObject toJson() const;
    static NoiseGateParameters fromJson(const QJsonObject& json);

    bool operator==(const NoiseGateParameters& o) const {
        return channels == o.channels && monitorChannels == o.monitorChannels && processChannels == o.processChannels &&
               attack == o.attack && attackUnit == o.attackUnit && release == o.release &&
               releaseUnit == o.releaseUnit && threshold == o.threshold && attenuation == o.attenuation;
    }
    bool operator!=(const NoiseGateParameters& o) const { return !(*this == o); }
};

struct RACEParameters {
    int channels = 2;
    int channelA = 0;
    int channelB = 1;
    double delay = 0.25;
    std::optional<bool> subsampleDelay;
    DelayUnit delayUnit = DelayUnit::ms;
    double attenuation = 6.0;
    QJsonObject toJson() const;
    static RACEParameters fromJson(const QJsonObject& json);

    bool operator==(const RACEParameters& o) const {
        return channels == o.channels && channelA == o.channelA && channelB == o.channelB && delay == o.delay &&
               subsampleDelay == o.subsampleDelay && delayUnit == o.delayUnit && attenuation == o.attenuation;
    }
    bool operator!=(const RACEParameters& o) const { return !(*this == o); }
};

struct LookaheadLimiterProcessorParameters {
    int channels = 2;
    std::vector<int> monitorChannels;
    std::vector<int> processChannels;
    double limit = 0.0;
    double attack = 5.0;
    TimeUnit attackUnit = TimeUnit::ms;
    double release = 100.0;
    TimeUnit releaseUnit = TimeUnit::ms;
    std::optional<bool> delayProcessedOnly;
    QJsonObject toJson() const;
    static LookaheadLimiterProcessorParameters fromJson(const QJsonObject& json);

    bool operator==(const LookaheadLimiterProcessorParameters& o) const {
        return channels == o.channels && monitorChannels == o.monitorChannels && processChannels == o.processChannels &&
               limit == o.limit && attack == o.attack && attackUnit == o.attackUnit && release == o.release &&
               releaseUnit == o.releaseUnit && delayProcessedOnly == o.delayProcessedOnly;
    }
    bool operator!=(const LookaheadLimiterProcessorParameters& o) const { return !(*this == o); }
};

struct ProcessorConfig {
    ProcessorType type = ProcessorType::Compressor;
    CompressorParameters compressorParams;
    NoiseGateParameters noiseGateParams;
    RACEParameters raceParams;
    LookaheadLimiterProcessorParameters lookaheadParams;
    QJsonObject toJson() const;
    static ProcessorConfig fromJson(const QJsonObject& json);

    bool operator==(const ProcessorConfig& o) const {
        return type == o.type && compressorParams == o.compressorParams && noiseGateParams == o.noiseGateParams &&
               raceParams == o.raceParams && lookaheadParams == o.lookaheadParams;
    }
    bool operator!=(const ProcessorConfig& o) const { return !(*this == o); }
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

    bool operator==(const PipelineStep& o) const {
        return type == o.type && channel == o.channel && channels == o.channels && name == o.name && names == o.names &&
               bypassed == o.bypassed;
    }
    bool operator!=(const PipelineStep& o) const { return !(*this == o); }
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

    bool operator==(const DSPConfiguration& o) const {
        return devices == o.devices && filters == o.filters && mixers == o.mixers && processors == o.processors &&
               pipeline == o.pipeline;
    }
    bool operator!=(const DSPConfiguration& o) const { return !(*this == o); }
};

#endif // DSP_CONFIG_TYPES_H
