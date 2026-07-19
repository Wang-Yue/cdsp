#include "models/DeviceConfig.h"

#include <QFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>

std::vector<int> DeviceConfig::supportedChannels() const {
    if (capabilities.capability_sets.empty())
        return {};
    std::set<int> chs;
    for (const auto& cap : capabilities.capability_sets[0].capabilities) {
        chs.insert(cap.channels);
    }
    return std::vector<int>(chs.begin(), chs.end());
}

std::vector<int> DeviceConfig::supportedRates() const {
    if (capabilities.capability_sets.empty())
        return {};
    const auto& set = capabilities.capability_sets[0];

    const ChannelCapability* selectedCap = nullptr;
    for (const auto& cap : set.capabilities) {
        if (cap.channels == deviceChannels) {
            selectedCap = &cap;
            break;
        }
    }
    if (!selectedCap && !set.capabilities.empty()) {
        selectedCap = &set.capabilities[0];
    }

    std::set<int> rates;
    if (selectedCap) {
        for (const auto& sr : selectedCap->samplerates) {
            rates.insert(sr.samplerate);
        }
    } else {
        for (const auto& cap : set.capabilities) {
            for (const auto& sr : cap.samplerates) {
                rates.insert(sr.samplerate);
            }
        }
    }
    return std::vector<int>(rates.begin(), rates.end());
}

static int formatPriority(const std::string& fmt) {
    if (fmt == "S32")
        return 4;
    if (fmt == "S24")
        return 3;
    if (fmt == "S16")
        return 2;
    if (fmt == "F32")
        return 1;
    if (fmt == "F64")
        return 0;
    return -1;
}

std::vector<std::string> DeviceConfig::supportedFormats() const {
    if (capabilities.capability_sets.empty())
        return {};
    const auto& set = capabilities.capability_sets[0];

    const ChannelCapability* selectedCap = nullptr;
    for (const auto& cap : set.capabilities) {
        if (cap.channels == deviceChannels) {
            selectedCap = &cap;
            break;
        }
    }
    if (!selectedCap && !set.capabilities.empty()) {
        selectedCap = &set.capabilities[0];
    }

    std::vector<std::string> fmts;
    if (selectedCap) {
        for (const auto& sr : selectedCap->samplerates) {
            if (sr.samplerate == sampleRate) {
                fmts = sr.formats;
                break;
            }
        }
    }
    std::sort(fmts.begin(), fmts.end(),
              [](const std::string& a, const std::string& b) { return formatPriority(a) > formatPriority(b); });
    return fmts;
}

DeviceConfig DeviceConfig::enforced() const {
    DeviceConfig res = *this;
    if (isHardwareBackend(res.backend)) {
        auto chs = res.supportedChannels();
        if (!chs.empty()) {
            auto it = std::find_if(chs.begin(), chs.end(), [&res](int c) { return c >= res.channels; });
            if (it != chs.end()) {
                res.deviceChannels = *it;
            } else {
                int maxPhys = *std::max_element(chs.begin(), chs.end());
                res.channels = maxPhys;
                res.deviceChannels = maxPhys;
            }
        }
        res.channels = std::max(1, std::min(res.deviceChannels, res.channels));

        auto rates = res.supportedRates();
        if (!rates.empty() && std::find(rates.begin(), rates.end(), res.sampleRate) == rates.end()) {
            res.sampleRate = bestRate(rates, res.sampleRate);
        }

        auto fmts = res.supportedFormats();
        if (!fmts.empty() && std::find(fmts.begin(), fmts.end(), res.format) == fmts.end()) {
            res.format = fmts.empty() ? "F32" : fmts[0];
        }
    } else if (res.backend == AudioBackendType::WavFile) {
        if (!res.filename.empty()) {
            if (auto wavInfo = parseWavHeader(res.filename)) {
                res.channels = wavInfo->first;
                res.sampleRate = wavInfo->second;
            }
        }
        res.channels = std::max(1, std::min(32, res.channels));
        res.deviceChannels = res.channels;
    } else {
        res.channels = std::max(1, std::min(32, res.channels));
        res.deviceChannels = res.channels;
    }
    return res;
}

std::optional<std::pair<int, int>> DeviceConfig::parseWavHeader(const std::string& path) {
    if (path.empty())
        return std::nullopt;
    if (!QFile::exists(QString::fromStdString(path)))
        return std::nullopt;
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    QByteArray riffData = file.read(12);
    if (riffData.size() < 12)
        return std::nullopt;

    std::string riff = riffData.left(4).toStdString();
    std::string wave = riffData.mid(8, 4).toStdString();
    if ((riff != "RIFF" && riff != "RF64") || wave != "WAVE")
        return std::nullopt;

    if (!file.seek(12))
        return std::nullopt;
    QByteArray remainingData = file.read(1024);
    int fmtOffset = remainingData.indexOf("fmt ");
    if (fmtOffset == -1 || fmtOffset + 16 > remainingData.size())
        return std::nullopt;

    uint16_t numChannels = 0;
    std::memcpy(&numChannels, remainingData.constData() + fmtOffset + 10, sizeof(uint16_t));

    uint32_t sampleRate = 0;
    std::memcpy(&sampleRate, remainingData.constData() + fmtOffset + 12, sizeof(uint32_t));

    if (numChannels < 1 || numChannels > 32)
        return std::nullopt;
    if (sampleRate < 8000 || sampleRate > 768000)
        return std::nullopt;

    return std::make_pair(static_cast<int>(numChannels), static_cast<int>(sampleRate));
}

int DeviceConfig::bestRate(const std::vector<int>& rates, int currentRate) {
    if (rates.empty())
        return 48000;
    if (std::find(rates.begin(), rates.end(), currentRate) != rates.end())
        return currentRate;
    for (int preferred : {48000, 44100, 96000, 192000}) {
        if (std::find(rates.begin(), rates.end(), preferred) != rates.end())
            return preferred;
    }
    int best = rates[0];
    int minDiff = std::abs(best - currentRate);
    for (int r : rates) {
        int diff = std::abs(r - currentRate);
        if (diff < minDiff) {
            minDiff = diff;
            best = r;
        }
    }
    return best;
}

CaptureDeviceConfig DeviceConfig::toCaptureDeviceConfig() const {
    CaptureDeviceConfig cap;
    cap.backend = backend;
    switch (backend) {
    case AudioBackendType::CoreAudio:
        cap.coreAudio.channels = channels;
        cap.coreAudio.device = deviceName();
        cap.coreAudio.bypassDoP = bypassDoP;
        cap.coreAudio.dopCutoffHz = dopCutoffHz;
        break;
    case AudioBackendType::WASAPI:
        cap.wasapi.channels = channels;
        cap.wasapi.device = deviceName();
        cap.wasapi.format = format;
        cap.wasapi.bypassDoP = bypassDoP;
        cap.wasapi.dopCutoffHz = dopCutoffHz;
        break;
    case AudioBackendType::ASIO:
        cap.asio.channels = channels;
        cap.asio.device = deviceName();
        cap.asio.format = format;
        cap.asio.bypassDoP = bypassDoP;
        cap.asio.dopCutoffHz = dopCutoffHz;
        break;
    case AudioBackendType::ALSA:
        cap.alsa.channels = channels;
        cap.alsa.device = deviceName();
        cap.alsa.format = format;
        break;
    case AudioBackendType::PulseAudio:
        cap.pulseAudio.channels = channels;
        cap.pulseAudio.device = deviceName();
        cap.pulseAudio.format = format;
        break;
    case AudioBackendType::WavFile:
        cap.wavFile.filename = filename.empty() ? "" : filename;
        cap.wavFile.extraSamples = extraSamples > 0 ? std::make_optional(extraSamples) : std::nullopt;
        break;
    case AudioBackendType::RawFile:
        cap.rawFile.filename = filename.empty() ? "" : filename;
        cap.rawFile.channels = channels;
        cap.rawFile.format = fileFormat;
        cap.rawFile.skipBytes = skipBytes > 0 ? std::make_optional(skipBytes) : std::nullopt;
        cap.rawFile.readBytes = readBytes > 0 ? std::make_optional(readBytes) : std::nullopt;
        cap.rawFile.extraSamples = extraSamples > 0 ? std::make_optional(extraSamples) : std::nullopt;
        break;
    case AudioBackendType::SignalGenerator:
        cap.generator.channels = channels;
        cap.generator.signal.type = generatorType;
        cap.generator.signal.freq = (generatorType == "WhiteNoise") ? std::nullopt : std::make_optional(generatorFreq);
        cap.generator.signal.level = generatorLevel;
        break;
    }
    return cap;
}

PlaybackDeviceConfig DeviceConfig::toPlaybackDeviceConfig() const {
    PlaybackDeviceConfig pb;
    pb.backend = backend;
    switch (backend) {
    case AudioBackendType::CoreAudio:
        pb.coreAudio.channels = channels;
        pb.coreAudio.device = deviceName();
        pb.coreAudio.outputDoP = outputDoP;
        pb.coreAudio.dsdEncoderFilter = dsdEncoderFilter;
        break;
    case AudioBackendType::WASAPI:
        pb.wasapi.channels = channels;
        pb.wasapi.device = deviceName();
        pb.wasapi.format = format;
        pb.wasapi.outputDoP = outputDoP;
        pb.wasapi.dsdEncoderFilter = dsdEncoderFilter;
        break;
    case AudioBackendType::ASIO:
        pb.asio.channels = channels;
        pb.asio.device = deviceName();
        pb.asio.format = format;
        pb.asio.outputDoP = outputDoP;
        pb.asio.dsdEncoderFilter = dsdEncoderFilter;
        break;
    case AudioBackendType::ALSA:
        pb.alsa.channels = channels;
        pb.alsa.device = deviceName();
        pb.alsa.format = format;
        break;
    case AudioBackendType::PulseAudio:
        pb.pulseAudio.channels = channels;
        pb.pulseAudio.device = deviceName();
        pb.pulseAudio.format = format;
        break;
    case AudioBackendType::RawFile:
    case AudioBackendType::WavFile:
        pb.rawFile.filename = filename.empty() ? "" : filename;
        pb.rawFile.channels = channels;
        pb.rawFile.format = fileFormat;
        pb.rawFile.wavHeader = (backend == AudioBackendType::WavFile || isWav);
        if (backend == AudioBackendType::WavFile || isWav) {
            pb.rawFile.useRf64 = useRf64;
        }
        break;
    case AudioBackendType::SignalGenerator:
        break;
    }
    return pb;
}

QJsonObject DeviceConfig::toJson() const {
    QJsonObject obj;
    obj["backend"] = QString::fromStdString(audioBackendTypeToString(backend));
    obj["channels"] = channels;
    obj["deviceChannels"] = deviceChannels;
    obj["sampleRate"] = sampleRate;
    obj["format"] = QString::fromStdString(format);
    obj["bypassDoP"] = bypassDoP;
    obj["dopCutoffHz"] = dopCutoffHz;
    obj["outputDoP"] = outputDoP;
    obj["dsdEncoderFilter"] = QString::fromStdString(sdmFilterToString(dsdEncoderFilter));
    obj["filename"] = QString::fromStdString(filename);
    obj["fileFormat"] = QString::fromStdString(fileFormat);
    obj["isWav"] = isWav;
    obj["useRf64"] = useRf64;
    obj["skipBytes"] = skipBytes;
    obj["readBytes"] = readBytes;
    obj["extraSamples"] = extraSamples;
    obj["generatorType"] = QString::fromStdString(generatorType);
    obj["generatorFreq"] = generatorFreq;
    obj["generatorLevel"] = generatorLevel;
    obj["capabilities"] = capabilities.toJson();
    return obj;
}

DeviceConfig DeviceConfig::fromJson(const QJsonObject& json) {
    DeviceConfig cfg;
    if (json.contains("backend"))
        cfg.backend = stringToAudioBackendType(json["backend"].toString().toStdString());
    if (json.contains("channels"))
        cfg.channels = json["channels"].toInt();
    if (json.contains("deviceChannels")) {
        cfg.deviceChannels = json["deviceChannels"].toInt();
    } else {
        cfg.deviceChannels = cfg.channels;
    }
    if (json.contains("sampleRate"))
        cfg.sampleRate = json["sampleRate"].toInt();
    if (json.contains("format"))
        cfg.format = json["format"].toString().toStdString();
    if (json.contains("bypassDoP"))
        cfg.bypassDoP = json["bypassDoP"].toBool();
    if (json.contains("dopCutoffHz"))
        cfg.dopCutoffHz = json["dopCutoffHz"].toDouble();
    if (json.contains("outputDoP"))
        cfg.outputDoP = json["outputDoP"].toBool();
    if (json.contains("dsdEncoderFilter"))
        cfg.dsdEncoderFilter = stringToSDMFilter(json["dsdEncoderFilter"].toString().toStdString());
    if (json.contains("filename"))
        cfg.filename = json["filename"].toString().toStdString();
    if (json.contains("fileFormat"))
        cfg.fileFormat = json["fileFormat"].toString().toStdString();
    if (json.contains("isWav"))
        cfg.isWav = json["isWav"].toBool();
    if (json.contains("useRf64"))
        cfg.useRf64 = json["useRf64"].toBool();
    if (json.contains("skipBytes"))
        cfg.skipBytes = json["skipBytes"].toInt();
    if (json.contains("readBytes"))
        cfg.readBytes = json["readBytes"].toInt();
    if (json.contains("extraSamples"))
        cfg.extraSamples = json["extraSamples"].toInt();
    if (json.contains("generatorType"))
        cfg.generatorType = json["generatorType"].toString().toStdString();
    if (json.contains("generatorFreq"))
        cfg.generatorFreq = json["generatorFreq"].toDouble();
    if (json.contains("generatorLevel"))
        cfg.generatorLevel = json["generatorLevel"].toDouble();
    if (json.contains("capabilities") && json["capabilities"].isObject()) {
        cfg.capabilities = AudioDeviceDescriptor::fromJson(json["capabilities"].toObject());
    } else if (json.contains("deviceName")) {
        cfg.capabilities.name = json["deviceName"].toString().toStdString();
    }
    return cfg;
}

bool DeviceConfig::operator==(const DeviceConfig& other) const {
    return backend == other.backend && capabilities == other.capabilities && channels == other.channels &&
           deviceChannels == other.deviceChannels && sampleRate == other.sampleRate && format == other.format &&
           bypassDoP == other.bypassDoP && dopCutoffHz == other.dopCutoffHz && outputDoP == other.outputDoP &&
           dsdEncoderFilter == other.dsdEncoderFilter && filename == other.filename && fileFormat == other.fileFormat &&
           isWav == other.isWav && useRf64 == other.useRf64 && skipBytes == other.skipBytes &&
           readBytes == other.readBytes && extraSamples == other.extraSamples && generatorType == other.generatorType &&
           generatorFreq == other.generatorFreq && generatorLevel == other.generatorLevel;
}
