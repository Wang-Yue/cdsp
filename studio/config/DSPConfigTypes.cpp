#include "config/DSPConfigTypes.h"

#include <cmath>
#include <set>
#include <stdexcept>

std::string faderToString(Fader fader) {
    switch (fader) {
    case Fader::Main:
        return "Main";
    case Fader::Aux1:
        return "Aux1";
    case Fader::Aux2:
        return "Aux2";
    case Fader::Aux3:
        return "Aux3";
    case Fader::Aux4:
        return "Aux4";
    }
    return "Main";
}

Fader stringToFader(const std::string& str) {
    if (str == "Aux1" || str == "aux1")
        return Fader::Aux1;
    if (str == "Aux2" || str == "aux2")
        return Fader::Aux2;
    if (str == "Aux3" || str == "aux3")
        return Fader::Aux3;
    if (str == "Aux4" || str == "aux4")
        return Fader::Aux4;
    return Fader::Main;
}

std::string processingStateToString(ProcessingState state) {
    switch (state) {
    case ProcessingState::Inactive:
        return "Inactive";
    case ProcessingState::Starting:
        return "Starting";
    case ProcessingState::Running:
        return "Running";
    case ProcessingState::Paused:
        return "Paused";
    case ProcessingState::Stalled:
        return "Stalled";
    }
    return "Inactive";
}

ProcessingState uint8ToProcessingState(uint8_t rawByte) {
    switch (rawByte) {
    case 1:
        return ProcessingState::Starting;
    case 2:
        return ProcessingState::Running;
    case 3:
        return ProcessingState::Paused;
    case 4:
        return ProcessingState::Stalled;
    default:
        return ProcessingState::Inactive;
    }
}

std::string audioBackendTypeToString(AudioBackendType type) {
    switch (type) {
    case AudioBackendType::CoreAudio:
        return "CoreAudio";
    case AudioBackendType::WASAPI:
        return "WASAPI";
    case AudioBackendType::ASIO:
        return "ASIO";
    case AudioBackendType::ALSA:
        return "ALSA";
    case AudioBackendType::PulseAudio:
        return "PulseAudio";
    case AudioBackendType::RawFile:
        return "RawFile";
    case AudioBackendType::WavFile:
        return "WavFile";
    case AudioBackendType::SignalGenerator:
        return "SignalGenerator";
    }
    return "CoreAudio";
}

AudioBackendType stringToAudioBackendType(const std::string& str) {
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(), ::tolower);

    if (lowerStr == "coreaudio")
        return AudioBackendType::CoreAudio;
    if (lowerStr == "wasapi")
        return AudioBackendType::WASAPI;
    if (lowerStr == "asio")
        return AudioBackendType::ASIO;
    if (lowerStr == "alsa")
        return AudioBackendType::ALSA;
    if (lowerStr == "pulseaudio" || lowerStr == "pulse")
        return AudioBackendType::PulseAudio;
    if (lowerStr == "rawfile" || lowerStr == "file")
        return AudioBackendType::RawFile;
    if (lowerStr == "wavfile")
        return AudioBackendType::WavFile;
    if (lowerStr == "signalgenerator")
        return AudioBackendType::SignalGenerator;

#if defined(_WIN32) || defined(Q_OS_WIN)
    return AudioBackendType::WASAPI;
#elif defined(__APPLE__) || defined(Q_OS_MAC)
    return AudioBackendType::CoreAudio;
#else
    return AudioBackendType::ALSA;
#endif
}

std::string sdmFilterToString(SDMFilter f) {
    switch (f) {
    case SDMFilter::Clans4:
        return "clans-4";
    case SDMFilter::SDM4:
        return "sdm-4";
    case SDMFilter::Clans5:
        return "clans-5";
    case SDMFilter::SDM5:
        return "sdm-5";
    case SDMFilter::Clans6:
        return "clans-6";
    case SDMFilter::SDM6:
        return "sdm-6";
    case SDMFilter::Clans7:
        return "clans-7";
    case SDMFilter::SDM7:
        return "sdm-7";
    case SDMFilter::Clans8:
        return "clans-8";
    case SDMFilter::SDM8:
        return "sdm-8";
    }
    return "sdm-6";
}

SDMFilter stringToSDMFilter(const std::string& str) {
    if (str == "clans-4")
        return SDMFilter::Clans4;
    if (str == "sdm-4")
        return SDMFilter::SDM4;
    if (str == "clans-5")
        return SDMFilter::Clans5;
    if (str == "sdm-5")
        return SDMFilter::SDM5;
    if (str == "clans-6")
        return SDMFilter::Clans6;
    if (str == "sdm-6")
        return SDMFilter::SDM6;
    if (str == "clans-7")
        return SDMFilter::Clans7;
    if (str == "sdm-7")
        return SDMFilter::SDM7;
    if (str == "clans-8")
        return SDMFilter::Clans8;
    if (str == "sdm-8")
        return SDMFilter::SDM8;
    return SDMFilter::SDM6;
}

std::string delayUnitToString(DelayUnit unit) {
    switch (unit) {
    case DelayUnit::ms:
        return "ms";
    case DelayUnit::us:
        return "us";
    case DelayUnit::samples:
        return "samples";
    case DelayUnit::mm:
        return "mm";
    }
    return "ms";
}

DelayUnit stringToDelayUnit(const std::string& str) {
    if (str == "us")
        return DelayUnit::us;
    if (str == "samples")
        return DelayUnit::samples;
    if (str == "mm")
        return DelayUnit::mm;
    return DelayUnit::ms;
}

double delayUnitToSamples(DelayUnit unit, double delay, double sampleRate) {
    switch (unit) {
    case DelayUnit::ms:
        return delay / 1000.0 * sampleRate;
    case DelayUnit::us:
        return delay / 1000000.0 * sampleRate;
    case DelayUnit::samples:
        return delay;
    case DelayUnit::mm:
        return delay / 1000.0 * sampleRate / 343.0;
    }
    return delay / 1000.0 * sampleRate;
}

std::string gainScaleToString(GainScale s) {
    return s == GainScale::linear ? "linear" : "dB";
}

GainScale stringToGainScale(const std::string& str) {
    return str == "linear" ? GainScale::linear : GainScale::dB;
}

std::string resamplerTypeToString(ResamplerType t) {
    switch (t) {
    case ResamplerType::Synchronous:
        return "Synchronous";
    case ResamplerType::AsyncSinc:
        return "AsyncSinc";
    case ResamplerType::AsyncPoly:
        return "AsyncPoly";
    case ResamplerType::Slip:
        return "Slip";
    }
    return "Synchronous";
}

std::string sincInterpolationToString(SincInterpolation interp) {
    switch (interp) {
    case SincInterpolation::Nearest:
        return "Nearest";
    case SincInterpolation::Linear:
        return "Linear";
    case SincInterpolation::Quadratic:
        return "Quadratic";
    case SincInterpolation::Cubic:
        return "Cubic";
    }
    return "Linear";
}

SincInterpolation stringToSincInterpolation(const std::string& str) {
    if (str == "Nearest")
        return SincInterpolation::Nearest;
    if (str == "Quadratic")
        return SincInterpolation::Quadratic;
    if (str == "Cubic")
        return SincInterpolation::Cubic;
    return SincInterpolation::Linear;
}

std::string resamplerInterpolationToString(ResamplerInterpolation interp) {
    switch (interp) {
    case ResamplerInterpolation::Linear:
        return "Linear";
    case ResamplerInterpolation::Cubic:
        return "Cubic";
    case ResamplerInterpolation::Quintic:
        return "Quintic";
    case ResamplerInterpolation::Septic:
        return "Septic";
    }
    return "Linear";
}

ResamplerInterpolation stringToResamplerInterpolation(const std::string& str) {
    if (str == "Cubic")
        return ResamplerInterpolation::Cubic;
    if (str == "Quintic")
        return ResamplerInterpolation::Quintic;
    if (str == "Septic")
        return ResamplerInterpolation::Septic;
    return ResamplerInterpolation::Linear;
}

ResamplerType stringToResamplerType(const std::string& str) {
    if (str == "AsyncSinc")
        return ResamplerType::AsyncSinc;
    if (str == "AsyncPoly")
        return ResamplerType::AsyncPoly;
    if (str == "Slip")
        return ResamplerType::Slip;
    return ResamplerType::Synchronous;
}

std::string resamplerProfileToString(ResamplerProfile p) {
    switch (p) {
    case ResamplerProfile::VeryFast:
        return "VeryFast";
    case ResamplerProfile::Fast:
        return "Fast";
    case ResamplerProfile::Balanced:
        return "Balanced";
    case ResamplerProfile::Accurate:
        return "Accurate";
    }
    return "Balanced";
}

ResamplerProfile stringToResamplerProfile(const std::string& str) {
    if (str == "VeryFast")
        return ResamplerProfile::VeryFast;
    if (str == "Fast")
        return ResamplerProfile::Fast;
    if (str == "Accurate")
        return ResamplerProfile::Accurate;
    return ResamplerProfile::Balanced;
}

std::string filterTypeToString(FilterType t) {
    switch (t) {
    case FilterType::Gain:
        return "Gain";
    case FilterType::Volume:
        return "Volume";
    case FilterType::Loudness:
        return "Loudness";
    case FilterType::Biquad:
        return "Biquad";
    case FilterType::Conv:
        return "Conv";
    case FilterType::Delay:
        return "Delay";
    case FilterType::BiquadCombo:
        return "BiquadCombo";
    case FilterType::DiffEq:
        return "DiffEq";
    case FilterType::Dither:
        return "Dither";
    case FilterType::Clipper:
        return "Clipper";
    case FilterType::LookaheadLimiter:
        return "LookaheadLimiter";
    }
    return "Gain";
}

FilterType stringToFilterType(const std::string& str) {
    if (str == "Volume")
        return FilterType::Volume;
    if (str == "Loudness")
        return FilterType::Loudness;
    if (str == "Biquad")
        return FilterType::Biquad;
    if (str == "Conv")
        return FilterType::Conv;
    if (str == "Delay")
        return FilterType::Delay;
    if (str == "BiquadCombo")
        return FilterType::BiquadCombo;
    if (str == "DiffEq")
        return FilterType::DiffEq;
    if (str == "Dither")
        return FilterType::Dither;
    if (str == "Clipper")
        return FilterType::Clipper;
    if (str == "LookaheadLimiter")
        return FilterType::LookaheadLimiter;
    return FilterType::Gain;
}

std::string biquadComboTypeToString(BiquadComboType t) {
    switch (t) {
    case BiquadComboType::ButterworthHighpass:
        return "ButterworthHighpass";
    case BiquadComboType::ButterworthLowpass:
        return "ButterworthLowpass";
    case BiquadComboType::LinkwitzRileyHighpass:
        return "LinkwitzRileyHighpass";
    case BiquadComboType::LinkwitzRileyLowpass:
        return "LinkwitzRileyLowpass";
    case BiquadComboType::Tilt:
        return "Tilt";
    case BiquadComboType::FivePointPeq:
        return "FivePointPeq";
    case BiquadComboType::GraphicEqualizer:
        return "GraphicEqualizer";
    }
    return "ButterworthLowpass";
}

BiquadComboType stringToBiquadComboType(const std::string& str) {
    if (str == "ButterworthHighpass")
        return BiquadComboType::ButterworthHighpass;
    if (str == "ButterworthLowpass")
        return BiquadComboType::ButterworthLowpass;
    if (str == "LinkwitzRileyHighpass")
        return BiquadComboType::LinkwitzRileyHighpass;
    if (str == "LinkwitzRileyLowpass")
        return BiquadComboType::LinkwitzRileyLowpass;
    if (str == "Tilt")
        return BiquadComboType::Tilt;
    if (str == "FivePointPeq")
        return BiquadComboType::FivePointPeq;
    if (str == "GraphicEqualizer")
        return BiquadComboType::GraphicEqualizer;
    return BiquadComboType::ButterworthLowpass;
}

std::string ditherTypeToString(DitherType t) {
    switch (t) {
    case DitherType::None:
        return "None";
    case DitherType::Flat:
        return "Flat";
    case DitherType::Highpass:
        return "Highpass";
    case DitherType::Fweighted441:
        return "Fweighted441";
    case DitherType::FweightedLong441:
        return "FweightedLong441";
    case DitherType::FweightedShort441:
        return "FweightedShort441";
    case DitherType::Gesemann441:
        return "Gesemann441";
    case DitherType::Gesemann48:
        return "Gesemann48";
    case DitherType::Lipshitz441:
        return "Lipshitz441";
    case DitherType::LipshitzLong441:
        return "LipshitzLong441";
    case DitherType::Shibata441:
        return "Shibata441";
    case DitherType::ShibataHigh441:
        return "ShibataHigh441";
    case DitherType::ShibataLow441:
        return "ShibataLow441";
    case DitherType::Shibata48:
        return "Shibata48";
    case DitherType::ShibataHigh48:
        return "ShibataHigh48";
    case DitherType::ShibataLow48:
        return "ShibataLow48";
    case DitherType::Shibata882:
        return "Shibata882";
    case DitherType::ShibataLow882:
        return "ShibataLow882";
    case DitherType::Shibata96:
        return "Shibata96";
    case DitherType::ShibataLow96:
        return "ShibataLow96";
    case DitherType::Shibata192:
        return "Shibata192";
    case DitherType::ShibataLow192:
        return "ShibataLow192";
    }
    return "Flat";
}

DitherType stringToDitherType(const std::string& str) {
    if (str == "None")
        return DitherType::None;
    if (str == "Highpass")
        return DitherType::Highpass;
    if (str == "Fweighted441")
        return DitherType::Fweighted441;
    if (str == "FweightedLong441")
        return DitherType::FweightedLong441;
    if (str == "FweightedShort441")
        return DitherType::FweightedShort441;
    if (str == "Gesemann441")
        return DitherType::Gesemann441;
    if (str == "Gesemann48")
        return DitherType::Gesemann48;
    if (str == "Lipshitz441")
        return DitherType::Lipshitz441;
    if (str == "LipshitzLong441")
        return DitherType::LipshitzLong441;
    if (str == "Shibata441")
        return DitherType::Shibata441;
    if (str == "ShibataHigh441")
        return DitherType::ShibataHigh441;
    if (str == "ShibataLow441")
        return DitherType::ShibataLow441;
    if (str == "Shibata48")
        return DitherType::Shibata48;
    if (str == "ShibataHigh48")
        return DitherType::ShibataHigh48;
    if (str == "ShibataLow48")
        return DitherType::ShibataLow48;
    if (str == "Shibata882")
        return DitherType::Shibata882;
    if (str == "ShibataLow882")
        return DitherType::ShibataLow882;
    if (str == "Shibata96")
        return DitherType::Shibata96;
    if (str == "ShibataLow96")
        return DitherType::ShibataLow96;
    if (str == "Shibata192")
        return DitherType::Shibata192;
    if (str == "ShibataLow192")
        return DitherType::ShibataLow192;
    return DitherType::Flat;
}

std::string processorTypeToString(ProcessorType t) {
    switch (t) {
    case ProcessorType::Compressor:
        return "Compressor";
    case ProcessorType::NoiseGate:
        return "NoiseGate";
    case ProcessorType::RACE:
        return "RACE";
    }
    return "Compressor";
}

ProcessorType stringToProcessorType(const std::string& str) {
    if (str == "NoiseGate")
        return ProcessorType::NoiseGate;
    if (str == "RACE")
        return ProcessorType::RACE;
    return ProcessorType::Compressor;
}

// JSON Encoders & Decoders

GeneratorConfig GeneratorConfig::fromJson(const QJsonObject& json) {
    GeneratorConfig cfg;
    if (json.contains("type"))
        cfg.type = json["type"].toString().toStdString();
    if (json.contains("freq"))
        cfg.freq = json["freq"].toDouble();
    if (json.contains("level"))
        cfg.level = json["level"].toDouble();
    return cfg;
}

QJsonObject GeneratorConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(type);
    if (freq.has_value())
        obj["freq"] = freq.value();
    obj["level"] = level;
    return obj;
}

CoreAudioCaptureConfig CoreAudioCaptureConfig::fromJson(const QJsonObject& json) {
    CoreAudioCaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("bypass_dop"))
        cfg.bypassDoP = json["bypass_dop"].toBool();
    if (json.contains("dop_cutoff_hz"))
        cfg.dopCutoffHz = json["dop_cutoff_hz"].toDouble();
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject CoreAudioCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "CoreAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (bypassDoP.has_value())
        obj["bypass_dop"] = bypassDoP.value();
    if (dopCutoffHz.has_value())
        obj["dop_cutoff_hz"] = dopCutoffHz.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

CoreAudioPlaybackConfig CoreAudioPlaybackConfig::fromJson(const QJsonObject& json) {
    CoreAudioPlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("exclusive"))
        cfg.exclusive = json["exclusive"].toBool();
    if (json.contains("output_dop"))
        cfg.outputDoP = json["output_dop"].toBool();
    if (json.contains("dsd_encoder_filter"))
        cfg.dsdEncoderFilter = stringToSDMFilter(json["dsd_encoder_filter"].toString().toStdString());
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject CoreAudioPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "CoreAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (exclusive.has_value())
        obj["exclusive"] = exclusive.value();
    if (outputDoP.has_value())
        obj["output_dop"] = outputDoP.value();
    if (dsdEncoderFilter.has_value())
        obj["dsd_encoder_filter"] = QString::fromStdString(sdmFilterToString(dsdEncoderFilter.value()));
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

WavFileCaptureConfig WavFileCaptureConfig::fromJson(const QJsonObject& json) {
    WavFileCaptureConfig cfg;
    if (json.contains("filename"))
        cfg.filename = json["filename"].toString().toStdString();
    if (json.contains("extra_samples"))
        cfg.extraSamples = json["extra_samples"].toInt();
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject WavFileCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "WavFile";
    obj["filename"] = QString::fromStdString(filename);
    if (extraSamples.has_value())
        obj["extra_samples"] = extraSamples.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

RawFileCaptureConfig RawFileCaptureConfig::fromJson(const QJsonObject& json) {
    RawFileCaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("filename"))
        cfg.filename = json["filename"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("skip_bytes"))
        cfg.skipBytes = json["skip_bytes"].toInt();
    if (json.contains("read_bytes"))
        cfg.readBytes = json["read_bytes"].toInt();
    if (json.contains("extra_samples"))
        cfg.extraSamples = json["extra_samples"].toInt();
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject RawFileCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "RawFile";
    obj["channels"] = channels;
    obj["filename"] = QString::fromStdString(filename);
    obj["format"] = QString::fromStdString(format);
    if (skipBytes.has_value())
        obj["skip_bytes"] = skipBytes.value();
    if (readBytes.has_value())
        obj["read_bytes"] = readBytes.value();
    if (extraSamples.has_value())
        obj["extra_samples"] = extraSamples.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

RawFilePlaybackConfig RawFilePlaybackConfig::fromJson(const QJsonObject& json) {
    RawFilePlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("filename"))
        cfg.filename = json["filename"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("wav_header"))
        cfg.wavHeader = json["wav_header"].toBool();
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject RawFilePlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "File";
    obj["channels"] = channels;
    obj["filename"] = QString::fromStdString(filename);
    obj["format"] = QString::fromStdString(format);
    if (wavHeader.has_value())
        obj["wav_header"] = wavHeader.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

GeneratorCaptureConfig GeneratorCaptureConfig::fromJson(const QJsonObject& json) {
    GeneratorCaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("signal"))
        cfg.signal = GeneratorConfig::fromJson(json["signal"].toObject());
    if (json.contains("channel_labels")) {
        QJsonArray arr = json["channel_labels"].toArray();
        for (const auto& val : arr)
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject GeneratorCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "SignalGenerator";
    obj["channels"] = channels;
    obj["signal"] = signal.toJson();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

WASAPICaptureConfig WASAPICaptureConfig::fromJson(const QJsonObject& json) {
    WASAPICaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("exclusive"))
        cfg.exclusive = json["exclusive"].toBool();
    if (json.contains("loopback"))
        cfg.loopback = json["loopback"].toBool();
    if (json.contains("polling"))
        cfg.polling = json["polling"].toBool();
    if (json.contains("bypass_dop"))
        cfg.bypassDoP = json["bypass_dop"].toBool();
    if (json.contains("dop_cutoff_hz"))
        cfg.dopCutoffHz = json["dop_cutoff_hz"].toDouble();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject WASAPICaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "WASAPI";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (exclusive.has_value())
        obj["exclusive"] = exclusive.value();
    if (loopback.has_value())
        obj["loopback"] = loopback.value();
    if (polling.has_value())
        obj["polling"] = polling.value();
    if (bypassDoP.has_value())
        obj["bypass_dop"] = bypassDoP.value();
    if (dopCutoffHz.has_value())
        obj["dop_cutoff_hz"] = dopCutoffHz.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

WASAPIPlaybackConfig WASAPIPlaybackConfig::fromJson(const QJsonObject& json) {
    WASAPIPlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("exclusive"))
        cfg.exclusive = json["exclusive"].toBool();
    if (json.contains("polling"))
        cfg.polling = json["polling"].toBool();
    if (json.contains("output_dop"))
        cfg.outputDoP = json["output_dop"].toBool();
    if (json.contains("dsd_encoder_filter"))
        cfg.dsdEncoderFilter = stringToSDMFilter(json["dsd_encoder_filter"].toString().toStdString());
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject WASAPIPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "WASAPI";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (exclusive.has_value())
        obj["exclusive"] = exclusive.value();
    if (polling.has_value())
        obj["polling"] = polling.value();
    if (outputDoP.has_value())
        obj["output_dop"] = outputDoP.value();
    if (dsdEncoderFilter.has_value())
        obj["dsd_encoder_filter"] = QString::fromStdString(sdmFilterToString(dsdEncoderFilter.value()));
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

ASIOCaptureConfig ASIOCaptureConfig::fromJson(const QJsonObject& json) {
    ASIOCaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("bypass_dop"))
        cfg.bypassDoP = json["bypass_dop"].toBool();
    if (json.contains("dop_cutoff_hz"))
        cfg.dopCutoffHz = json["dop_cutoff_hz"].toDouble();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject ASIOCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "ASIO";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (bypassDoP.has_value())
        obj["bypass_dop"] = bypassDoP.value();
    if (dopCutoffHz.has_value())
        obj["dop_cutoff_hz"] = dopCutoffHz.value();
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

ASIOPlaybackConfig ASIOPlaybackConfig::fromJson(const QJsonObject& json) {
    ASIOPlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("output_dop"))
        cfg.outputDoP = json["output_dop"].toBool();
    if (json.contains("dsd_encoder_filter"))
        cfg.dsdEncoderFilter = stringToSDMFilter(json["dsd_encoder_filter"].toString().toStdString());
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject ASIOPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "ASIO";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (outputDoP.has_value())
        obj["output_dop"] = outputDoP.value();
    if (dsdEncoderFilter.has_value())
        obj["dsd_encoder_filter"] = QString::fromStdString(sdmFilterToString(dsdEncoderFilter.value()));
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

ALSACaptureConfig ALSACaptureConfig::fromJson(const QJsonObject& json) {
    ALSACaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("stop_on_inactive"))
        cfg.stopOnInactive = json["stop_on_inactive"].toBool();
    if (json.contains("link_volume_control"))
        cfg.linkVolumeControl = json["link_volume_control"].toString().toStdString();
    if (json.contains("link_mute_control"))
        cfg.linkMuteControl = json["link_mute_control"].toString().toStdString();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject ALSACaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "ALSA";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (stopOnInactive.has_value())
        obj["stop_on_inactive"] = stopOnInactive.value();
    if (linkVolumeControl.has_value())
        obj["link_volume_control"] = QString::fromStdString(linkVolumeControl.value());
    if (linkMuteControl.has_value())
        obj["link_mute_control"] = QString::fromStdString(linkMuteControl.value());
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

ALSAPlaybackConfig ALSAPlaybackConfig::fromJson(const QJsonObject& json) {
    ALSAPlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("stop_on_inactive"))
        cfg.stopOnInactive = json["stop_on_inactive"].toBool();
    if (json.contains("link_volume_control"))
        cfg.linkVolumeControl = json["link_volume_control"].toString().toStdString();
    if (json.contains("link_mute_control"))
        cfg.linkMuteControl = json["link_mute_control"].toString().toStdString();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject ALSAPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "ALSA";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (stopOnInactive.has_value())
        obj["stop_on_inactive"] = stopOnInactive.value();
    if (linkVolumeControl.has_value())
        obj["link_volume_control"] = QString::fromStdString(linkVolumeControl.value());
    if (linkMuteControl.has_value())
        obj["link_mute_control"] = QString::fromStdString(linkMuteControl.value());
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

PulseAudioCaptureConfig PulseAudioCaptureConfig::fromJson(const QJsonObject& json) {
    PulseAudioCaptureConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("stop_on_inactive"))
        cfg.stopOnInactive = json["stop_on_inactive"].toBool();
    if (json.contains("link_volume_control"))
        cfg.linkVolumeControl = json["link_volume_control"].toString().toStdString();
    if (json.contains("link_mute_control"))
        cfg.linkMuteControl = json["link_mute_control"].toString().toStdString();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject PulseAudioCaptureConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "PulseAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (stopOnInactive.has_value())
        obj["stop_on_inactive"] = stopOnInactive.value();
    if (linkVolumeControl.has_value())
        obj["link_volume_control"] = QString::fromStdString(linkVolumeControl.value());
    if (linkMuteControl.has_value())
        obj["link_mute_control"] = QString::fromStdString(linkMuteControl.value());
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

PulseAudioPlaybackConfig PulseAudioPlaybackConfig::fromJson(const QJsonObject& json) {
    PulseAudioPlaybackConfig cfg;
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("device"))
        cfg.device = json["device"].toString().toStdString();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("stop_on_inactive"))
        cfg.stopOnInactive = json["stop_on_inactive"].toBool();
    if (json.contains("link_volume_control"))
        cfg.linkVolumeControl = json["link_volume_control"].toString().toStdString();
    if (json.contains("link_mute_control"))
        cfg.linkMuteControl = json["link_mute_control"].toString().toStdString();
    if (json.contains("channel_labels")) {
        for (const auto& val : json["channel_labels"].toArray())
            cfg.channelLabels.push_back(val.toString().toStdString());
    }
    return cfg;
}

QJsonObject PulseAudioPlaybackConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = "PulseAudio";
    obj["channels"] = channels;
    if (device.has_value() && !device.value().empty())
        obj["device"] = QString::fromStdString(device.value());
    if (format.has_value())
        obj["format"] = QString::fromStdString(format.value());
    if (stopOnInactive.has_value())
        obj["stop_on_inactive"] = stopOnInactive.value();
    if (linkVolumeControl.has_value())
        obj["link_volume_control"] = QString::fromStdString(linkVolumeControl.value());
    if (linkMuteControl.has_value())
        obj["link_mute_control"] = QString::fromStdString(linkMuteControl.value());
    if (!channelLabels.empty()) {
        QJsonArray arr;
        for (const auto& l : channelLabels)
            arr.append(QString::fromStdString(l));
        obj["channel_labels"] = arr;
    }
    return obj;
}

CaptureDeviceConfig CaptureDeviceConfig::fromJson(const QJsonObject& json) {
    CaptureDeviceConfig cfg;
    std::string typeStr = json["type"].toString().toStdString();
    cfg.backend = stringToAudioBackendType(typeStr);
    switch (cfg.backend) {
    case AudioBackendType::CoreAudio:
        cfg.coreAudio = CoreAudioCaptureConfig::fromJson(json);
        break;
    case AudioBackendType::WASAPI:
        cfg.wasapi = WASAPICaptureConfig::fromJson(json);
        break;
    case AudioBackendType::ASIO:
        cfg.asio = ASIOCaptureConfig::fromJson(json);
        break;
    case AudioBackendType::ALSA:
        cfg.alsa = ALSACaptureConfig::fromJson(json);
        break;
    case AudioBackendType::PulseAudio:
        cfg.pulseAudio = PulseAudioCaptureConfig::fromJson(json);
        break;
    case AudioBackendType::WavFile:
        cfg.wavFile = WavFileCaptureConfig::fromJson(json);
        break;
    case AudioBackendType::RawFile:
        cfg.rawFile = RawFileCaptureConfig::fromJson(json);
        break;
    case AudioBackendType::SignalGenerator:
        cfg.generator = GeneratorCaptureConfig::fromJson(json);
        break;
    }
    return cfg;
}

QJsonObject CaptureDeviceConfig::toJson() const {
    switch (backend) {
    case AudioBackendType::CoreAudio:
        return coreAudio.toJson();
    case AudioBackendType::WASAPI:
        return wasapi.toJson();
    case AudioBackendType::ASIO:
        return asio.toJson();
    case AudioBackendType::ALSA:
        return alsa.toJson();
    case AudioBackendType::PulseAudio:
        return pulseAudio.toJson();
    case AudioBackendType::WavFile:
        return wavFile.toJson();
    case AudioBackendType::RawFile:
        return rawFile.toJson();
    case AudioBackendType::SignalGenerator:
        return generator.toJson();
    }
    return coreAudio.toJson();
}

PlaybackDeviceConfig PlaybackDeviceConfig::fromJson(const QJsonObject& json) {
    PlaybackDeviceConfig cfg;
    std::string typeStr = json["type"].toString().toStdString();
    cfg.backend = stringToAudioBackendType(typeStr);
    switch (cfg.backend) {
    case AudioBackendType::CoreAudio:
        cfg.coreAudio = CoreAudioPlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::WASAPI:
        cfg.wasapi = WASAPIPlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::ASIO:
        cfg.asio = ASIOPlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::ALSA:
        cfg.alsa = ALSAPlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::PulseAudio:
        cfg.pulseAudio = PulseAudioPlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::RawFile:
    case AudioBackendType::WavFile:
        cfg.rawFile = RawFilePlaybackConfig::fromJson(json);
        break;
    case AudioBackendType::SignalGenerator:
        cfg.coreAudio = CoreAudioPlaybackConfig::fromJson(json);
        break;
    }
    return cfg;
}

QJsonObject PlaybackDeviceConfig::toJson() const {
    switch (backend) {
    case AudioBackendType::CoreAudio:
        return coreAudio.toJson();
    case AudioBackendType::WASAPI:
        return wasapi.toJson();
    case AudioBackendType::ASIO:
        return asio.toJson();
    case AudioBackendType::ALSA:
        return alsa.toJson();
    case AudioBackendType::PulseAudio:
        return pulseAudio.toJson();
    case AudioBackendType::RawFile:
        return rawFile.toJson();
    case AudioBackendType::WavFile:
        return rawFile.toJson();
    case AudioBackendType::SignalGenerator:
        return coreAudio.toJson();
    }
    return coreAudio.toJson();
}

ResamplerConfig ResamplerConfig::fromJson(const QJsonObject& json) {
    ResamplerConfig cfg;
    if (json.contains("type"))
        cfg.type = stringToResamplerType(json["type"].toString().toStdString());
    if (json.contains("profile"))
        cfg.profile = json["profile"].toString().toStdString();
    if (json.contains("interpolation"))
        cfg.interpolation = json["interpolation"].toString().toStdString();
    if (json.contains("sinc_len"))
        cfg.sincLen = json["sinc_len"].toInt();
    if (json.contains("oversampling_factor"))
        cfg.oversamplingFactor = json["oversampling_factor"].toInt();
    if (json.contains("window"))
        cfg.window = json["window"].toString().toStdString();
    if (json.contains("f_cutoff"))
        cfg.fCutoff = json["f_cutoff"].toDouble();
    return cfg;
}

QJsonObject ResamplerConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(resamplerTypeToString(type));
    if (profile.has_value())
        obj["profile"] = QString::fromStdString(profile.value());
    if (interpolation.has_value())
        obj["interpolation"] = QString::fromStdString(interpolation.value());
    if (sincLen.has_value())
        obj["sinc_len"] = sincLen.value();
    if (oversamplingFactor.has_value())
        obj["oversampling_factor"] = oversamplingFactor.value();
    if (window.has_value())
        obj["window"] = QString::fromStdString(window.value());
    if (fCutoff.has_value())
        obj["f_cutoff"] = fCutoff.value();
    return obj;
}

DevicesConfig DevicesConfig::fromJson(const QJsonObject& json) {
    DevicesConfig cfg;
    if (json.contains("samplerate"))
        cfg.samplerate = json["samplerate"].toInt();
    if (json.contains("chunksize"))
        cfg.chunksize = json["chunksize"].toInt();
    if (json.contains("capture"))
        cfg.capture = CaptureDeviceConfig::fromJson(json["capture"].toObject());
    if (json.contains("playback"))
        cfg.playback = PlaybackDeviceConfig::fromJson(json["playback"].toObject());
    if (json.contains("enable_rate_adjust"))
        cfg.enableRateAdjust = json["enable_rate_adjust"].toBool();
    if (json.contains("target_level"))
        cfg.targetLevel = json["target_level"].toInt();
    if (json.contains("adjust_interval_s"))
        cfg.adjustPeriod = json["adjust_interval_s"].toDouble();
    if (json.contains("resampler"))
        cfg.resampler = ResamplerConfig::fromJson(json["resampler"].toObject());
    if (json.contains("silence_threshold"))
        cfg.silenceThreshold = json["silence_threshold"].toDouble();
    if (json.contains("silence_timeout_s"))
        cfg.silenceTimeout = json["silence_timeout_s"].toDouble();
    if (json.contains("volume_ramp_time_ms"))
        cfg.volumeRampTime = json["volume_ramp_time_ms"].toDouble();
    if (json.contains("volume_limit"))
        cfg.volumeLimit = json["volume_limit"].toDouble();
    if (json.contains("queuelimit"))
        cfg.queuelimit = json["queuelimit"].toInt();
    if (json.contains("stop_on_rate_change"))
        cfg.stopOnRateChange = json["stop_on_rate_change"].toBool();
    if (json.contains("rate_measure_interval_s"))
        cfg.rateMeasureInterval = json["rate_measure_interval_s"].toDouble();
    if (json.contains("multithreaded"))
        cfg.multithreaded = json["multithreaded"].toBool();
    if (json.contains("worker_threads"))
        cfg.workerThreads = json["worker_threads"].toInt();
    return cfg;
}

QJsonObject DevicesConfig::toJson() const {
    QJsonObject obj;
    obj["samplerate"] = samplerate;
    obj["chunksize"] = chunksize;
    obj["capture"] = capture.toJson();
    obj["playback"] = playback.toJson();

    if (enableRateAdjust.has_value())
        obj["enable_rate_adjust"] = enableRateAdjust.value();
    if (targetLevel.has_value())
        obj["target_level"] = targetLevel.value();
    if (adjustPeriod.has_value())
        obj["adjust_interval_s"] = adjustPeriod.value();
    if (resampler.has_value())
        obj["resampler"] = resampler.value().toJson();
    if (captureSamplerate.has_value())
        obj["capture_samplerate"] = captureSamplerate.value();
    if (silenceThreshold.has_value())
        obj["silence_threshold"] = silenceThreshold.value();
    if (silenceTimeout.has_value())
        obj["silence_timeout_s"] = silenceTimeout.value();
    if (volumeRampTime.has_value())
        obj["volume_ramp_time_ms"] = volumeRampTime.value();
    if (volumeLimit.has_value())
        obj["volume_limit"] = volumeLimit.value();
    if (queuelimit.has_value())
        obj["queuelimit"] = queuelimit.value();
    if (stopOnRateChange.has_value())
        obj["stop_on_rate_change"] = stopOnRateChange.value();
    if (rateMeasureInterval.has_value())
        obj["rate_measure_interval_s"] = rateMeasureInterval.value();
    if (multithreaded.has_value())
        obj["multithreaded"] = multithreaded.value();
    if (workerThreads.has_value())
        obj["worker_threads"] = workerThreads.value();

    return obj;
}

GainParameters GainParameters::fromJson(const QJsonObject& json) {
    GainParameters p;
    if (json.contains("gain"))
        p.gain = json["gain"].toDouble();
    if (json.contains("scale"))
        p.scale = stringToGainScale(json["scale"].toString().toStdString());
    if (json.contains("inverted"))
        p.inverted = json["inverted"].toBool();
    if (json.contains("mute"))
        p.mute = json["mute"].toBool();
    return p;
}

QJsonObject GainParameters::toJson() const {
    QJsonObject obj;
    if (gain.has_value())
        obj["gain"] = gain.value();
    if (scale.has_value())
        obj["scale"] = QString::fromStdString(gainScaleToString(scale.value()));
    if (inverted.has_value())
        obj["inverted"] = inverted.value();
    if (mute.has_value())
        obj["mute"] = mute.value();
    return obj;
}

LoudnessParameters LoudnessParameters::fromJson(const QJsonObject& json) {
    LoudnessParameters p;
    if (json.contains("reference_level"))
        p.referenceLevel = json["reference_level"].toDouble();
    if (json.contains("high_boost"))
        p.highBoost = json["high_boost"].toDouble();
    if (json.contains("low_boost"))
        p.lowBoost = json["low_boost"].toDouble();
    if (json.contains("attenuate_mid"))
        p.attenuateMid = json["attenuate_mid"].toBool();
    if (json.contains("fader"))
        p.fader = stringToFader(json["fader"].toString().toStdString());
    return p;
}

QJsonObject LoudnessParameters::toJson() const {
    QJsonObject obj;
    if (referenceLevel.has_value())
        obj["reference_level"] = referenceLevel.value();
    if (highBoost.has_value())
        obj["high_boost"] = highBoost.value();
    if (lowBoost.has_value())
        obj["low_boost"] = lowBoost.value();
    if (attenuateMid.has_value())
        obj["attenuate_mid"] = attenuateMid.value();
    if (fader.has_value())
        obj["fader"] = QString::fromStdString(faderToString(fader.value()));
    return obj;
}

ConvParameters ConvParameters::fromJson(const QJsonObject& json) {
    ConvParameters p;
    std::string typeStr = json["type"].toString().toStdString();
    if (typeStr == "Values")
        p.type = ConvType::Values;
    else if (typeStr == "Wav")
        p.type = ConvType::Wav;
    else if (typeStr == "Dummy")
        p.type = ConvType::Dummy;
    else
        p.type = ConvType::Raw;

    if (json.contains("values")) {
        QJsonArray arr = json["values"].toArray();
        for (const auto& v : arr)
            p.values.push_back(v.toDouble());
    }
    if (json.contains("filename"))
        p.filename = json["filename"].toString().toStdString();
    if (json.contains("format"))
        p.format = json["format"].toString().toStdString();
    if (json.contains("channel"))
        p.channel = json["channel"].toInt();
    if (json.contains("length"))
        p.length = json["length"].toInt();
    if (json.contains("skip_bytes_lines"))
        p.skipBytesLines = json["skip_bytes_lines"].toInt();
    if (json.contains("read_bytes_lines"))
        p.readBytesLines = json["read_bytes_lines"].toInt();
    return p;
}

QJsonObject ConvParameters::toJson() const {
    QJsonObject obj;
    std::string typeStr = "Raw";
    switch (type) {
    case ConvType::Values:
        typeStr = "Values";
        break;
    case ConvType::Wav:
        typeStr = "Wav";
        break;
    case ConvType::Raw:
        typeStr = "Raw";
        break;
    case ConvType::Dummy:
        typeStr = "Dummy";
        break;
    }
    obj["type"] = QString::fromStdString(typeStr);
    if (!values.empty()) {
        QJsonArray arr;
        for (double v : values)
            arr.append(v);
        obj["values"] = arr;
    }
    if (!filename.empty())
        obj["filename"] = QString::fromStdString(filename);
    if (!format.empty())
        obj["format"] = QString::fromStdString(format);
    if (channel.has_value())
        obj["channel"] = channel.value();
    if (length.has_value())
        obj["length"] = length.value();
    if (skipBytesLines.has_value())
        obj["skip_bytes_lines"] = skipBytesLines.value();
    if (readBytesLines.has_value())
        obj["read_bytes_lines"] = readBytesLines.value();
    return obj;
}

DelayParameters DelayParameters::fromJson(const QJsonObject& json) {
    DelayParameters p;
    if (json.contains("delay"))
        p.delay = json["delay"].toDouble();
    if (json.contains("unit"))
        p.unit = stringToDelayUnit(json["unit"].toString().toStdString());
    if (json.contains("subsample"))
        p.subsample = json["subsample"].toBool();
    return p;
}

QJsonObject DelayParameters::toJson() const {
    QJsonObject obj;
    obj["delay"] = delay;
    if (unit.has_value())
        obj["unit"] = QString::fromStdString(delayUnitToString(unit.value()));
    if (subsample.has_value())
        obj["subsample"] = subsample.value();
    return obj;
}

BiquadComboParameters BiquadComboParameters::fromJson(const QJsonObject& json) {
    BiquadComboParameters p;
    if (json.contains("type"))
        p.type = stringToBiquadComboType(json["type"].toString().toStdString());
    if (json.contains("freq"))
        p.freq = json["freq"].toDouble();
    if (json.contains("order"))
        p.order = json["order"].toInt();
    if (json.contains("gain"))
        p.gain = json["gain"].toDouble();
    if (json.contains("fls"))
        p.fls = json["fls"].toDouble();
    if (json.contains("qls"))
        p.qls = json["qls"].toDouble();
    if (json.contains("gls"))
        p.gls = json["gls"].toDouble();
    if (json.contains("fp1"))
        p.fp1 = json["fp1"].toDouble();
    if (json.contains("qp1"))
        p.qp1 = json["qp1"].toDouble();
    if (json.contains("gp1"))
        p.gp1 = json["gp1"].toDouble();
    if (json.contains("fp2"))
        p.fp2 = json["fp2"].toDouble();
    if (json.contains("qp2"))
        p.qp2 = json["qp2"].toDouble();
    if (json.contains("gp2"))
        p.gp2 = json["gp2"].toDouble();
    if (json.contains("fp3"))
        p.fp3 = json["fp3"].toDouble();
    if (json.contains("qp3"))
        p.qp3 = json["qp3"].toDouble();
    if (json.contains("gp3"))
        p.gp3 = json["gp3"].toDouble();
    if (json.contains("fhs"))
        p.fhs = json["fhs"].toDouble();
    if (json.contains("qhs"))
        p.qhs = json["qhs"].toDouble();
    if (json.contains("ghs"))
        p.ghs = json["ghs"].toDouble();
    if (json.contains("freq_min"))
        p.freqMin = json["freq_min"].toDouble();
    if (json.contains("freq_max"))
        p.freqMax = json["freq_max"].toDouble();
    if (json.contains("gains")) {
        QJsonArray arr = json["gains"].toArray();
        for (const auto& v : arr)
            p.gains.push_back(v.toDouble());
    }
    return p;
}

QJsonObject BiquadComboParameters::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(biquadComboTypeToString(type));
    if (freq.has_value())
        obj["freq"] = freq.value();
    if (order.has_value())
        obj["order"] = order.value();
    if (gain.has_value())
        obj["gain"] = gain.value();
    if (fls.has_value())
        obj["fls"] = fls.value();
    if (qls.has_value())
        obj["qls"] = qls.value();
    if (gls.has_value())
        obj["gls"] = gls.value();
    if (fp1.has_value())
        obj["fp1"] = fp1.value();
    if (qp1.has_value())
        obj["qp1"] = qp1.value();
    if (gp1.has_value())
        obj["gp1"] = gp1.value();
    if (fp2.has_value())
        obj["fp2"] = fp2.value();
    if (qp2.has_value())
        obj["qp2"] = qp2.value();
    if (gp2.has_value())
        obj["gp2"] = gp2.value();
    if (fp3.has_value())
        obj["fp3"] = fp3.value();
    if (qp3.has_value())
        obj["qp3"] = qp3.value();
    if (gp3.has_value())
        obj["gp3"] = gp3.value();
    if (fhs.has_value())
        obj["fhs"] = fhs.value();
    if (qhs.has_value())
        obj["qhs"] = qhs.value();
    if (ghs.has_value())
        obj["ghs"] = ghs.value();
    if (freqMin.has_value())
        obj["freq_min"] = freqMin.value();
    if (freqMax.has_value())
        obj["freq_max"] = freqMax.value();
    if (!gains.empty()) {
        QJsonArray arr;
        for (double g : gains)
            arr.append(g);
        obj["gains"] = arr;
    }
    return obj;
}

DiffEqParameters DiffEqParameters::fromJson(const QJsonObject& json) {
    DiffEqParameters p;
    if (json.contains("a")) {
        QJsonArray arr = json["a"].toArray();
        for (const auto& val : arr)
            p.a.push_back(val.toDouble());
    }
    if (json.contains("b")) {
        QJsonArray arr = json["b"].toArray();
        for (const auto& val : arr)
            p.b.push_back(val.toDouble());
    }
    return p;
}

QJsonObject DiffEqParameters::toJson() const {
    QJsonObject obj;
    if (!a.empty()) {
        QJsonArray arr;
        for (double val : a)
            arr.append(val);
        obj["a"] = arr;
    }
    if (!b.empty()) {
        QJsonArray arr;
        for (double val : b)
            arr.append(val);
        obj["b"] = arr;
    }
    return obj;
}

DitherParameters DitherParameters::fromJson(const QJsonObject& json) {
    DitherParameters p;
    if (json.contains("type"))
        p.type = stringToDitherType(json["type"].toString().toStdString());
    if (json.contains("bits"))
        p.bits = json["bits"].toInt();
    if (json.contains("amplitude"))
        p.amplitude = json["amplitude"].toDouble();
    return p;
}

QJsonObject DitherParameters::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(ditherTypeToString(type));
    obj["bits"] = bits;
    if (amplitude.has_value())
        obj["amplitude"] = amplitude.value();
    return obj;
}

ClipperParameters ClipperParameters::fromJson(const QJsonObject& json) {
    ClipperParameters p;
    if (json.contains("clip_limit"))
        p.clipLimit = json["clip_limit"].toDouble();
    if (json.contains("soft_clip"))
        p.softClip = json["soft_clip"].toBool();
    return p;
}

QJsonObject ClipperParameters::toJson() const {
    QJsonObject obj;
    obj["clip_limit"] = clipLimit;
    if (softClip.has_value())
        obj["soft_clip"] = softClip.value();
    return obj;
}

LookaheadLimiterParameters LookaheadLimiterParameters::fromJson(const QJsonObject& json) {
    LookaheadLimiterParameters p;
    if (json.contains("limit"))
        p.limit = json["limit"].toDouble();
    if (json.contains("attack"))
        p.attack = json["attack"].toDouble();
    if (json.contains("release"))
        p.release = json["release"].toDouble();
    if (json.contains("attack_unit"))
        p.attackUnit = stringToDelayUnit(json["attack_unit"].toString().toStdString());
    if (json.contains("release_unit"))
        p.releaseUnit = stringToDelayUnit(json["release_unit"].toString().toStdString());
    return p;
}

QJsonObject LookaheadLimiterParameters::toJson() const {
    QJsonObject obj;
    obj["limit"] = limit;
    obj["attack"] = attack;
    obj["release"] = release;
    if (attackUnit.has_value())
        obj["attack_unit"] = QString::fromStdString(delayUnitToString(attackUnit.value()));
    if (releaseUnit.has_value())
        obj["release_unit"] = QString::fromStdString(delayUnitToString(releaseUnit.value()));
    return obj;
}

VolumeParameters VolumeParameters::fromJson(const QJsonObject& json) {
    VolumeParameters p;
    if (json.contains("ramp_time_ms"))
        p.rampTime = json["ramp_time_ms"].toDouble();
    if (json.contains("limit"))
        p.limit = json["limit"].toDouble();
    if (json.contains("fader"))
        p.fader = stringToFader(json["fader"].toString().toStdString());
    return p;
}

QJsonObject VolumeParameters::toJson() const {
    QJsonObject obj;
    if (rampTime.has_value())
        obj["ramp_time_ms"] = rampTime.value();
    if (limit.has_value())
        obj["limit"] = limit.value();
    if (fader.has_value())
        obj["fader"] = QString::fromStdString(faderToString(fader.value()));
    return obj;
}

FilterConfig FilterConfig::fromJson(const QJsonObject& json) {
    FilterConfig f;
    f.type = stringToFilterType(json["type"].toString().toStdString());
    QJsonObject pObj = json["parameters"].toObject();
    switch (f.type) {
    case FilterType::Gain:
        f.gainParams = GainParameters::fromJson(pObj);
        break;
    case FilterType::Volume:
        f.volumeParams = VolumeParameters::fromJson(pObj);
        break;
    case FilterType::Loudness:
        f.loudnessParams = LoudnessParameters::fromJson(pObj);
        break;
    case FilterType::Biquad: {
        BiquadParameters b;
        if (pObj.contains("type"))
            b.type = stringToBiquadType(pObj["type"].toString().toStdString());
        if (pObj.contains("freq"))
            b.freq = pObj["freq"].toDouble();
        if (pObj.contains("gain"))
            b.gain = pObj["gain"].toDouble();
        if (pObj.contains("q"))
            b.q = pObj["q"].toDouble();
        if (pObj.contains("bandwidth"))
            b.bandwidth = pObj["bandwidth"].toDouble();
        if (pObj.contains("slope"))
            b.slope = pObj["slope"].toDouble();
        if (pObj.contains("b0"))
            b.b0 = pObj["b0"].toDouble();
        if (pObj.contains("b1"))
            b.b1 = pObj["b1"].toDouble();
        if (pObj.contains("b2"))
            b.b2 = pObj["b2"].toDouble();
        if (pObj.contains("a1"))
            b.a1 = pObj["a1"].toDouble();
        if (pObj.contains("a2"))
            b.a2 = pObj["a2"].toDouble();
        if (pObj.contains("freq_z"))
            b.freqNotch = pObj["freq_z"].toDouble();
        if (pObj.contains("freq_p"))
            b.freqPole = pObj["freq_p"].toDouble();
        if (pObj.contains("q_p"))
            b.qP = pObj["q_p"].toDouble();
        if (pObj.contains("normalize_at_dc"))
            b.normalizeAtDc = pObj["normalize_at_dc"].toBool();
        if (pObj.contains("freq_act"))
            b.freqAct = pObj["freq_act"].toDouble();
        if (pObj.contains("q_act"))
            b.qAct = pObj["q_act"].toDouble();
        if (pObj.contains("freq_target"))
            b.freqTarget = pObj["freq_target"].toDouble();
        if (pObj.contains("q_target"))
            b.qTarget = pObj["q_target"].toDouble();
        f.biquadParams = b;
        break;
    }
    case FilterType::Conv:
        f.convParams = ConvParameters::fromJson(pObj);
        break;
    case FilterType::Delay:
        f.delayParams = DelayParameters::fromJson(pObj);
        break;
    case FilterType::BiquadCombo:
        f.comboParams = BiquadComboParameters::fromJson(pObj);
        break;
    case FilterType::DiffEq:
        f.diffEqParams = DiffEqParameters::fromJson(pObj);
        break;
    case FilterType::Dither:
        f.ditherParams = DitherParameters::fromJson(pObj);
        break;
    case FilterType::Clipper:
        f.clipperParams = ClipperParameters::fromJson(pObj);
        break;
    case FilterType::LookaheadLimiter:
        f.lookaheadParams = LookaheadLimiterParameters::fromJson(pObj);
        break;
    }
    return f;
}

QJsonObject FilterConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(filterTypeToString(type));
    QJsonObject pObj;
    switch (type) {
    case FilterType::Gain:
        pObj = gainParams.toJson();
        break;
    case FilterType::Volume:
        pObj = volumeParams.toJson();
        break;
    case FilterType::Loudness:
        pObj = loudnessParams.toJson();
        break;
    case FilterType::Biquad: {
        QJsonObject bObj;
        if (biquadParams.type.has_value())
            bObj["type"] = QString::fromStdString(biquadTypeToString(biquadParams.type.value()));
        if (biquadParams.freq.has_value())
            bObj["freq"] = biquadParams.freq.value();
        if (biquadParams.gain.has_value())
            bObj["gain"] = biquadParams.gain.value();
        if (biquadParams.q.has_value())
            bObj["q"] = biquadParams.q.value();
        if (biquadParams.bandwidth.has_value())
            bObj["bandwidth"] = biquadParams.bandwidth.value();
        if (biquadParams.slope.has_value())
            bObj["slope"] = biquadParams.slope.value();
        if (biquadParams.b0.has_value())
            bObj["b0"] = biquadParams.b0.value();
        if (biquadParams.b1.has_value())
            bObj["b1"] = biquadParams.b1.value();
        if (biquadParams.b2.has_value())
            bObj["b2"] = biquadParams.b2.value();
        if (biquadParams.a1.has_value())
            bObj["a1"] = biquadParams.a1.value();
        if (biquadParams.a2.has_value())
            bObj["a2"] = biquadParams.a2.value();
        if (biquadParams.freqNotch.has_value())
            bObj["freq_z"] = biquadParams.freqNotch.value();
        if (biquadParams.freqPole.has_value())
            bObj["freq_p"] = biquadParams.freqPole.value();
        if (biquadParams.qP.has_value())
            bObj["q_p"] = biquadParams.qP.value();
        if (biquadParams.normalizeAtDc.has_value())
            bObj["normalize_at_dc"] = biquadParams.normalizeAtDc.value();
        if (biquadParams.freqAct.has_value())
            bObj["freq_act"] = biquadParams.freqAct.value();
        if (biquadParams.qAct.has_value())
            bObj["q_act"] = biquadParams.qAct.value();
        if (biquadParams.freqTarget.has_value())
            bObj["freq_target"] = biquadParams.freqTarget.value();
        if (biquadParams.qTarget.has_value())
            bObj["q_target"] = biquadParams.qTarget.value();
        pObj = bObj;
        break;
    }
    case FilterType::Conv:
        pObj = convParams.toJson();
        break;
    case FilterType::Delay:
        pObj = delayParams.toJson();
        break;
    case FilterType::BiquadCombo:
        pObj = comboParams.toJson();
        break;
    case FilterType::DiffEq:
        pObj = diffEqParams.toJson();
        break;
    case FilterType::Dither:
        pObj = ditherParams.toJson();
        break;
    case FilterType::Clipper:
        pObj = clipperParams.toJson();
        break;
    case FilterType::LookaheadLimiter:
        pObj = lookaheadParams.toJson();
        break;
    }
    obj["parameters"] = pObj;
    return obj;
}

MixerSource MixerSource::fromJson(const QJsonObject& json) {
    MixerSource s;
    if (json.contains("channel"))
        s.channel = json["channel"].toInt();
    if (json.contains("gain"))
        s.gain = json["gain"].toDouble();
    if (json.contains("inverted"))
        s.inverted = json["inverted"].toBool();
    if (json.contains("mute"))
        s.mute = json["mute"].toBool();
    if (json.contains("scale"))
        s.scale = stringToGainScale(json["scale"].toString().toStdString());
    return s;
}

QJsonObject MixerSource::toJson() const {
    QJsonObject obj;
    obj["channel"] = channel;
    if (gain.has_value())
        obj["gain"] = gain.value();
    if (inverted.has_value())
        obj["inverted"] = inverted.value();
    if (mute.has_value())
        obj["mute"] = mute.value();
    if (scale.has_value())
        obj["scale"] = QString::fromStdString(gainScaleToString(scale.value()));
    return obj;
}

MixerMapping MixerMapping::fromJson(const QJsonObject& json) {
    MixerMapping m;
    if (json.contains("dest"))
        m.dest = json["dest"].toInt();
    if (json.contains("sources")) {
        QJsonArray arr = json["sources"].toArray();
        for (const auto& val : arr)
            m.sources.push_back(MixerSource::fromJson(val.toObject()));
    }
    if (json.contains("mute"))
        m.mute = json["mute"].toBool();
    return m;
}

QJsonObject MixerMapping::toJson() const {
    QJsonObject obj;
    obj["dest"] = dest;
    QJsonArray srcArr;
    for (const auto& s : sources)
        srcArr.append(s.toJson());
    obj["sources"] = srcArr;
    if (mute.has_value())
        obj["mute"] = mute.value();
    return obj;
}

MixerConfig MixerConfig::fromJson(const QJsonObject& json) {
    MixerConfig mc;
    if (json.contains("channels")) {
        QJsonObject chObj = json["channels"].toObject();
        if (chObj.contains("in"))
            mc.channelsIn = chObj["in"].toInt();
        if (chObj.contains("out"))
            mc.channelsOut = chObj["out"].toInt();
    } else {
        if (json.contains("channels_in"))
            mc.channelsIn = json["channels_in"].toInt();
        if (json.contains("channels_out"))
            mc.channelsOut = json["channels_out"].toInt();
    }
    if (json.contains("mapping")) {
        QJsonArray arr = json["mapping"].toArray();
        for (const auto& val : arr)
            mc.mapping.push_back(MixerMapping::fromJson(val.toObject()));
    }
    if (json.contains("description"))
        mc.description = json["description"].toString().toStdString();
    if (json.contains("labels")) {
        QJsonArray arr = json["labels"].toArray();
        for (const auto& val : arr)
            mc.labels.push_back(val.toString().toStdString());
    }
    return mc;
}

QJsonObject MixerConfig::toJson() const {
    QJsonObject obj;
    QJsonObject chObj;
    chObj["in"] = channelsIn;
    chObj["out"] = channelsOut;
    obj["channels"] = chObj;

    QJsonArray mapArr;
    for (const auto& m : mapping)
        mapArr.append(m.toJson());
    obj["mapping"] = mapArr;

    if (description.has_value() && !description.value().empty()) {
        obj["description"] = QString::fromStdString(description.value());
    }
    if (!labels.empty()) {
        QJsonArray arr;
        for (const auto& l : labels)
            arr.append(QString::fromStdString(l));
        obj["labels"] = arr;
    }
    return obj;
}

CompressorParameters CompressorParameters::fromJson(const QJsonObject& json) {
    CompressorParameters p;
    if (json.contains("channels"))
        p.channels = json["channels"].toInt();
    if (json.contains("monitor_channels")) {
        QJsonArray arr = json["monitor_channels"].toArray();
        for (const auto& val : arr)
            p.monitorChannels.push_back(val.toInt());
    }
    if (json.contains("process_channels")) {
        QJsonArray arr = json["process_channels"].toArray();
        for (const auto& val : arr)
            p.processChannels.push_back(val.toInt());
    }
    if (json.contains("attack"))
        p.attack = json["attack"].toDouble();
    if (json.contains("release"))
        p.release = json["release"].toDouble();
    if (json.contains("threshold"))
        p.threshold = json["threshold"].toDouble();
    if (json.contains("factor"))
        p.factor = json["factor"].toDouble();
    if (json.contains("makeup_gain"))
        p.makeupGain = json["makeup_gain"].toDouble();
    if (json.contains("soft_clip"))
        p.softClip = json["soft_clip"].toBool();
    if (json.contains("clip_limit"))
        p.clipLimit = json["clip_limit"].toDouble();
    return p;
}

QJsonObject CompressorParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    if (!monitorChannels.empty()) {
        QJsonArray arr;
        for (int ch : monitorChannels)
            arr.append(ch);
        obj["monitor_channels"] = arr;
    }
    if (!processChannels.empty()) {
        QJsonArray arr;
        for (int ch : processChannels)
            arr.append(ch);
        obj["process_channels"] = arr;
    }
    obj["attack"] = attack;
    obj["release"] = release;
    obj["threshold"] = threshold;
    obj["factor"] = factor;
    if (makeupGain.has_value())
        obj["makeup_gain"] = makeupGain.value();
    if (softClip.has_value())
        obj["soft_clip"] = softClip.value();
    if (clipLimit.has_value())
        obj["clip_limit"] = clipLimit.value();
    return obj;
}

NoiseGateParameters NoiseGateParameters::fromJson(const QJsonObject& json) {
    NoiseGateParameters p;
    if (json.contains("channels"))
        p.channels = json["channels"].toInt();
    if (json.contains("monitor_channels")) {
        QJsonArray arr = json["monitor_channels"].toArray();
        for (const auto& val : arr)
            p.monitorChannels.push_back(val.toInt());
    }
    if (json.contains("process_channels")) {
        QJsonArray arr = json["process_channels"].toArray();
        for (const auto& val : arr)
            p.processChannels.push_back(val.toInt());
    }
    if (json.contains("attack"))
        p.attack = json["attack"].toDouble();
    if (json.contains("release"))
        p.release = json["release"].toDouble();
    if (json.contains("threshold"))
        p.threshold = json["threshold"].toDouble();
    if (json.contains("attenuation"))
        p.attenuation = json["attenuation"].toDouble();
    return p;
}

QJsonObject NoiseGateParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    if (!monitorChannels.empty()) {
        QJsonArray arr;
        for (int ch : monitorChannels)
            arr.append(ch);
        obj["monitor_channels"] = arr;
    }
    if (!processChannels.empty()) {
        QJsonArray arr;
        for (int ch : processChannels)
            arr.append(ch);
        obj["process_channels"] = arr;
    }
    obj["attack"] = attack;
    obj["release"] = release;
    obj["threshold"] = threshold;
    obj["attenuation"] = attenuation;
    return obj;
}

RACEParameters RACEParameters::fromJson(const QJsonObject& json) {
    RACEParameters p;
    if (json.contains("channels"))
        p.channels = json["channels"].toInt();
    if (json.contains("channel_a"))
        p.channelA = json["channel_a"].toInt();
    if (json.contains("channel_b"))
        p.channelB = json["channel_b"].toInt();
    if (json.contains("delay"))
        p.delay = json["delay"].toDouble();
    if (json.contains("subsample_delay"))
        p.subsampleDelay = json["subsample_delay"].toBool();
    if (json.contains("delay_unit"))
        p.delayUnit = stringToDelayUnit(json["delay_unit"].toString().toStdString());
    if (json.contains("attenuation"))
        p.attenuation = json["attenuation"].toDouble();
    return p;
}

QJsonObject RACEParameters::toJson() const {
    QJsonObject obj;
    obj["channels"] = channels;
    obj["channel_a"] = channelA;
    obj["channel_b"] = channelB;
    obj["delay"] = delay;
    if (subsampleDelay.has_value())
        obj["subsample_delay"] = subsampleDelay.value();
    if (delayUnit.has_value())
        obj["delay_unit"] = QString::fromStdString(delayUnitToString(delayUnit.value()));
    obj["attenuation"] = attenuation;
    return obj;
}

ProcessorConfig ProcessorConfig::fromJson(const QJsonObject& json) {
    ProcessorConfig p;
    p.type = stringToProcessorType(json["type"].toString().toStdString());
    QJsonObject pObj = json["parameters"].toObject();
    switch (p.type) {
    case ProcessorType::Compressor:
        p.compressorParams = CompressorParameters::fromJson(pObj);
        break;
    case ProcessorType::NoiseGate:
        p.noiseGateParams = NoiseGateParameters::fromJson(pObj);
        break;
    case ProcessorType::RACE:
        p.raceParams = RACEParameters::fromJson(pObj);
        break;
    }
    return p;
}

QJsonObject ProcessorConfig::toJson() const {
    QJsonObject obj;
    obj["type"] = QString::fromStdString(processorTypeToString(type));
    QJsonObject pObj;
    switch (type) {
    case ProcessorType::Compressor:
        pObj = compressorParams.toJson();
        break;
    case ProcessorType::NoiseGate:
        pObj = noiseGateParams.toJson();
        break;
    case ProcessorType::RACE:
        pObj = raceParams.toJson();
        break;
    }
    obj["parameters"] = pObj;
    return obj;
}

PipelineStep PipelineStep::fromJson(const QJsonObject& json) {
    PipelineStep step;
    std::string typeStr = json["type"].toString().toStdString();
    if (typeStr == "Mixer")
        step.type = PipelineStepType::Mixer;
    else if (typeStr == "Processor")
        step.type = PipelineStepType::Processor;
    else
        step.type = PipelineStepType::Filter;

    if (json.contains("channel"))
        step.channel = json["channel"].toInt();
    if (json.contains("channels")) {
        QJsonArray arr = json["channels"].toArray();
        for (const auto& val : arr)
            step.channels.push_back(val.toInt());
    }
    if (json.contains("name"))
        step.name = json["name"].toString().toStdString();
    if (json.contains("names")) {
        QJsonArray arr = json["names"].toArray();
        for (const auto& val : arr)
            step.names.push_back(val.toString().toStdString());
    }
    if (json.contains("bypassed"))
        step.bypassed = json["bypassed"].toBool();
    return step;
}

QJsonObject PipelineStep::toJson() const {
    QJsonObject obj;
    std::string typeStr = "Filter";
    switch (type) {
    case PipelineStepType::Filter:
        typeStr = "Filter";
        break;
    case PipelineStepType::Mixer:
        typeStr = "Mixer";
        break;
    case PipelineStepType::Processor:
        typeStr = "Processor";
        break;
    }
    obj["type"] = QString::fromStdString(typeStr);
    if (channel.has_value())
        obj["channel"] = channel.value();
    if (!channels.empty()) {
        QJsonArray arr;
        for (int c : channels)
            arr.append(c);
        obj["channels"] = arr;
    }
    if (type == PipelineStepType::Filter) {
        QJsonArray arr;
        if (!names.empty()) {
            for (const auto& n : names)
                arr.append(QString::fromStdString(n));
        } else if (name.has_value()) {
            arr.append(QString::fromStdString(name.value()));
        }
        if (!arr.isEmpty()) {
            obj["names"] = arr;
        }
    } else {
        if (name.has_value())
            obj["name"] = QString::fromStdString(name.value());
    }
    if (bypassed.has_value())
        obj["bypassed"] = bypassed.value();
    return obj;
}

DSPConfiguration DSPConfiguration::fromJsonObject(const QJsonObject& json) {
    DSPConfiguration config;
    if (json.contains("devices"))
        config.devices = DevicesConfig::fromJson(json["devices"].toObject());

    if (json.contains("filters")) {
        QJsonObject fObj = json["filters"].toObject();
        for (auto it = fObj.begin(); it != fObj.end(); ++it) {
            config.filters[it.key().toStdString()] = FilterConfig::fromJson(it.value().toObject());
        }
    }

    if (json.contains("mixers")) {
        QJsonObject mObj = json["mixers"].toObject();
        for (auto it = mObj.begin(); it != mObj.end(); ++it) {
            config.mixers[it.key().toStdString()] = MixerConfig::fromJson(it.value().toObject());
        }
    }

    if (json.contains("processors")) {
        QJsonObject pObj = json["processors"].toObject();
        for (auto it = pObj.begin(); it != pObj.end(); ++it) {
            config.processors[it.key().toStdString()] = ProcessorConfig::fromJson(it.value().toObject());
        }
    }

    if (json.contains("pipeline")) {
        QJsonArray pipeArr = json["pipeline"].toArray();
        for (const auto& val : pipeArr) {
            config.pipeline.push_back(PipelineStep::fromJson(val.toObject()));
        }
    }
    return config;
}

DSPConfiguration DSPConfiguration::fromJsonString(const std::string& jsonStr) {
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(jsonStr));
    return fromJsonObject(doc.object());
}

QJsonObject DSPConfiguration::toJsonObject() const {
    QJsonObject root;
    root["devices"] = devices.toJson();

    if (!filters.empty()) {
        QJsonObject fObj;
        for (const auto& [k, v] : filters) {
            fObj[QString::fromStdString(k)] = v.toJson();
        }
        root["filters"] = fObj;
    }

    if (!mixers.empty()) {
        QJsonObject mObj;
        for (const auto& [k, v] : mixers) {
            mObj[QString::fromStdString(k)] = v.toJson();
        }
        root["mixers"] = mObj;
    }

    if (!processors.empty()) {
        QJsonObject pObj;
        for (const auto& [k, v] : processors) {
            pObj[QString::fromStdString(k)] = v.toJson();
        }
        root["processors"] = pObj;
    }

    if (!pipeline.empty()) {
        QJsonArray pipeArr;
        for (const auto& step : pipeline) {
            pipeArr.append(step.toJson());
        }
        root["pipeline"] = pipeArr;
    }

    return root;
}

std::string DSPConfiguration::toJsonString() const {
    QJsonDocument doc(toJsonObject());
    return doc.toJson(QJsonDocument::Compact).toStdString();
}
