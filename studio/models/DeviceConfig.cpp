#include "models/DeviceConfig.h"
#include <algorithm>
#include <set>
#include <cmath>

std::vector<int> DeviceConfig::supportedChannels() const {
    if (capabilities.capability_sets.empty()) return {};
    std::set<int> chs;
    for (const auto& cap : capabilities.capability_sets[0].capabilities) {
        chs.insert(cap.channels);
    }
    return std::vector<int>(chs.begin(), chs.end());
}

std::vector<int> DeviceConfig::supportedRates() const {
    if (capabilities.capability_sets.empty()) return {};
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
    if (fmt == "S32") return 4;
    if (fmt == "S24") return 3;
    if (fmt == "S16") return 2;
    if (fmt == "F32") return 1;
    if (fmt == "F64") return 0;
    return -1;
}

std::vector<std::string> DeviceConfig::supportedFormats() const {
    if (capabilities.capability_sets.empty()) return {};
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
    std::sort(fmts.begin(), fmts.end(), [](const std::string& a, const std::string& b) {
        return formatPriority(a) > formatPriority(b);
    });
    return fmts;
}

DeviceConfig DeviceConfig::enforced() const {
    DeviceConfig res = *this;
    if (res.backend == AudioBackendType::CoreAudio) {
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
    } else {
        res.channels = std::max(1, std::min(32, res.channels));
        res.deviceChannels = res.channels;
    }
    return res;
}

int DeviceConfig::bestRate(const std::vector<int>& rates, int currentRate) {
    if (rates.empty()) return 48000;
    if (std::find(rates.begin(), rates.end(), currentRate) != rates.end()) return currentRate;
    for (int preferred : {48000, 44100, 96000, 192000}) {
        if (std::find(rates.begin(), rates.end(), preferred) != rates.end()) return preferred;
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
    cap.coreAudio.channels = deviceChannels;
    cap.coreAudio.device = deviceName();
    cap.coreAudio.format = format;
    cap.coreAudio.bypassDoP = bypassDoP;
    cap.coreAudio.dopCutoffHz = dopCutoffHz;
    cap.wavFile.filename = filename;
    cap.rawFile.filename = filename;
    cap.rawFile.channels = channels;
    cap.rawFile.format = fileFormat;
    cap.generator.channels = channels;
    cap.generator.signal.type = generatorType;
    cap.generator.signal.freq = generatorFreq;
    cap.generator.signal.level = generatorLevel;
    return cap;
}

PlaybackDeviceConfig DeviceConfig::toPlaybackDeviceConfig() const {
    PlaybackDeviceConfig pb;
    pb.backend = backend;
    pb.coreAudio.channels = deviceChannels;
    pb.coreAudio.device = deviceName();
    pb.coreAudio.format = format;
    pb.coreAudio.outputDoP = outputDoP;
    pb.coreAudio.dopEncoderFilter = dopEncoderFilter;
    pb.rawFile.filename = filename;
    pb.rawFile.channels = channels;
    pb.rawFile.format = fileFormat;
    pb.rawFile.wavHeader = isWav;
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
    obj["dopEncoderFilter"] = QString::fromStdString(sdmFilterToString(dopEncoderFilter));
    obj["filename"] = QString::fromStdString(filename);
    obj["fileFormat"] = QString::fromStdString(fileFormat);
    obj["isWav"] = isWav;
    obj["skipBytes"] = skipBytes;
    obj["readBytes"] = readBytes;
    obj["extraSamples"] = extraSamples;
    obj["generatorType"] = QString::fromStdString(generatorType);
    obj["generatorFreq"] = generatorFreq;
    obj["generatorLevel"] = generatorLevel;
    if (!capabilities.name.empty()) {
        obj["deviceName"] = QString::fromStdString(capabilities.name);
    }
    return obj;
}

DeviceConfig DeviceConfig::fromJson(const QJsonObject& json) {
    DeviceConfig cfg;
    if (json.contains("backend")) cfg.backend = stringToAudioBackendType(json["backend"].toString().toStdString());
    if (json.contains("channels")) cfg.channels = json["channels"].toInt();
    if (json.contains("deviceChannels")) {
        cfg.deviceChannels = json["deviceChannels"].toInt();
    } else {
        cfg.deviceChannels = cfg.channels;
    }
    if (json.contains("sampleRate")) cfg.sampleRate = json["sampleRate"].toInt();
    if (json.contains("format")) cfg.format = json["format"].toString().toStdString();
    if (json.contains("bypassDoP")) cfg.bypassDoP = json["bypassDoP"].toBool();
    if (json.contains("dopCutoffHz")) cfg.dopCutoffHz = json["dopCutoffHz"].toDouble();
    if (json.contains("outputDoP")) cfg.outputDoP = json["outputDoP"].toBool();
    if (json.contains("dopEncoderFilter")) cfg.dopEncoderFilter = stringToSDMFilter(json["dopEncoderFilter"].toString().toStdString());
    if (json.contains("filename")) cfg.filename = json["filename"].toString().toStdString();
    if (json.contains("fileFormat")) cfg.fileFormat = json["fileFormat"].toString().toStdString();
    if (json.contains("isWav")) cfg.isWav = json["isWav"].toBool();
    if (json.contains("skipBytes")) cfg.skipBytes = json["skipBytes"].toInt();
    if (json.contains("readBytes")) cfg.readBytes = json["readBytes"].toInt();
    if (json.contains("extraSamples")) cfg.extraSamples = json["extraSamples"].toInt();
    if (json.contains("generatorType")) cfg.generatorType = json["generatorType"].toString().toStdString();
    if (json.contains("generatorFreq")) cfg.generatorFreq = json["generatorFreq"].toDouble();
    if (json.contains("generatorLevel")) cfg.generatorLevel = json["generatorLevel"].toDouble();
    if (json.contains("deviceName")) cfg.capabilities.name = json["deviceName"].toString().toStdString();
    return cfg;
}

bool DeviceConfig::operator==(const DeviceConfig& other) const {
    return backend == other.backend
        && capabilities.name == other.capabilities.name
        && channels == other.channels
        && deviceChannels == other.deviceChannels
        && sampleRate == other.sampleRate
        && format == other.format
        && bypassDoP == other.bypassDoP
        && dopCutoffHz == other.dopCutoffHz
        && outputDoP == other.outputDoP
        && dopEncoderFilter == other.dopEncoderFilter
        && filename == other.filename
        && fileFormat == other.fileFormat
        && isWav == other.isWav
        && skipBytes == other.skipBytes
        && readBytes == other.readBytes
        && extraSamples == other.extraSamples
        && generatorType == other.generatorType
        && generatorFreq == other.generatorFreq
        && generatorLevel == other.generatorLevel;
}
