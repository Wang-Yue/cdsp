#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include "config/DSPConfigTypes.h"

#include <QJsonObject>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct DeviceConfig {
#if defined(ENABLE_COREAUDIO)
    AudioBackendType backend = AudioBackendType::CoreAudio;
#elif defined(ENABLE_WASAPI)
    AudioBackendType backend = AudioBackendType::WASAPI;
#elif defined(ENABLE_ALSA)
    AudioBackendType backend = AudioBackendType::ALSA;
#else
    AudioBackendType backend = AudioBackendType::RawFile;
#endif
    AudioDeviceDescriptor capabilities;

    int channels = 2;
    int deviceChannels = 2;
    int sampleRate = 48000;
    std::string format = "F32";
    bool exclusive = false;
    bool loopback = false;
    bool polling = false;
    bool stopOnInactive = false;
    std::string linkVolumeControl;
    std::string linkMuteControl;
    bool bypassDoP = true;
    double dopCutoffHz = 20000.0;
    bool outputDoP = false;
    bool outputDSD = false;
    SDMFilter dsdEncoderFilter = SDMFilter::SDM6;

    // File Backend Settings
    std::string filename;
    std::string fileFormat = "S16_LE";
    bool isWav = false;
    bool useRf64 = false;
    int64_t skipBytes = 0;
    int64_t readBytes = 0;
    int64_t extraSamples = 0;

    // Generator Backend Settings
    std::string generatorType = "Sine";
    double generatorFreq = 1000.0;
    double generatorLevel = -6.0;

    // PipeWire Backend Settings
    std::string nodeName;
    std::string nodeDescription;
    std::string nodeGroupName;
    std::string autoconnectTo;

    std::optional<std::string> deviceName() const {
        return capabilities.name.empty() ? std::nullopt : std::make_optional(capabilities.name);
    }

    void setDeviceName(const std::string& name) {
        if (capabilities.name != name) {
            capabilities.name = name;
            capabilities.capability_sets.clear();
        }
    }

    std::vector<int> supportedChannels() const;
    std::vector<int> supportedRates() const;
    std::vector<std::string> supportedFormats() const;

    DeviceConfig enforced() const;
    static std::string defaultFormatForBackend(AudioBackendType backend);
    static int bestRate(const std::vector<int>& rates, int currentRate);
    static std::optional<std::pair<int, int>> parseWavHeader(const std::string& path);

    CaptureDeviceConfig toCaptureDeviceConfig() const;
    PlaybackDeviceConfig toPlaybackDeviceConfig() const;

    QJsonObject toJson() const;
    static DeviceConfig fromJson(const QJsonObject& json);

    bool operator==(const DeviceConfig& other) const;
    bool operator!=(const DeviceConfig& other) const { return !(*this == other); }
};

#endif // DEVICE_CONFIG_H
