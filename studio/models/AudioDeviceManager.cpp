#include "models/AudioDeviceManager.h"
#include <QSettings>
#include <QJsonDocument>
#include <QtConcurrent>
#include <set>
#include <algorithm>

AudioDeviceManager::AudioDeviceManager(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<AudioSettings> settings, QObject* parent)
    : QObject(parent), m_engine(engine), m_settings(settings) {
    loadSavedConfigs();
    m_isInitializing = false;
    fetchDevices();
}

void AudioDeviceManager::loadSavedConfigs() {
    QSettings s("DSPMonitor", "MonitorQt");
    if (s.contains("captureConfig")) {
        QByteArray data = s.value("captureConfig").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) captureConfig = DeviceConfig::fromJson(doc.object());
    }
    if (s.contains("playbackConfig")) {
        QByteArray data = s.value("playbackConfig").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) playbackConfig = DeviceConfig::fromJson(doc.object());
    }
    exclusiveMode = s.value("exclusiveMode", false).toBool();
}

void AudioDeviceManager::saveConfigs() {
    QSettings s("DSPMonitor", "MonitorQt");
    QJsonDocument docCap(captureConfig.toJson());
    s.setValue("captureConfig", docCap.toJson(QJsonDocument::Compact));

    QJsonDocument docPb(playbackConfig.toJson());
    s.setValue("playbackConfig", docPb.toJson(QJsonDocument::Compact));

    s.setValue("exclusiveMode", exclusiveMode);
}

void AudioDeviceManager::setCaptureConfig(const DeviceConfig& config) {
    if (m_isInitializing) return;
    DeviceConfig enforced = config.enforced();
    bool devChanged = (enforced.capabilities.name != captureConfig.capabilities.name || enforced.backend != captureConfig.backend);
    captureConfig = enforced;
    saveConfigs();

    if (devChanged) {
        refreshDeviceCapabilities();
    } else {
        validateSampleRates();
        emit configChanged();
        if (onConfigChanged) onConfigChanged();
    }
}

void AudioDeviceManager::setPlaybackConfig(const DeviceConfig& config) {
    if (m_isInitializing) return;
    DeviceConfig enforced = config.enforced();
    bool devChanged = (enforced.capabilities.name != playbackConfig.capabilities.name || enforced.backend != playbackConfig.backend);
    playbackConfig = enforced;
    saveConfigs();

    if (devChanged) {
        refreshDeviceCapabilities();
    } else {
        validateSampleRates();
        emit configChanged();
        if (onConfigChanged) onConfigChanged();
    }
}

void AudioDeviceManager::setExclusiveMode(bool exclusive) {
    if (m_isInitializing) return;
    exclusiveMode = exclusive;
    if (!exclusiveMode && playbackConfig.outputDoP) {
        playbackConfig.outputDoP = false;
    }
    saveConfigs();
    emit configChanged();
    if (onConfigChanged) onConfigChanged();
}

std::vector<int> AudioDeviceManager::captureRateOptions() const {
    if (m_settings->resamplerEnabled) return captureConfig.supportedRates();
    auto cap = captureConfig.supportedRates();
    auto pb = playbackConfig.supportedRates();
    if (cap.empty()) return pb;
    if (pb.empty()) return cap;

    std::vector<int> common;
    std::set<int> pbSet(pb.begin(), pb.end());
    for (int r : cap) {
        if (pbSet.count(r)) common.push_back(r);
    }
    std::sort(common.begin(), common.end());
    return common.empty() ? pb : common;
}

std::vector<int> AudioDeviceManager::playbackRateOptions() const {
    return m_settings->resamplerEnabled ? playbackConfig.supportedRates() : captureRateOptions();
}

double AudioDeviceManager::latencyMs() const {
    if (captureConfig.sampleRate <= 0) return 0.0;
    return (static_cast<double>(m_settings->chunkSize) / static_cast<double>(captureConfig.sampleRate)) * 1000.0;
}

bool AudioDeviceManager::devicesAvailable() const {
    if (auto name = captureConfig.deviceName()) {
        bool found = false;
        for (const auto& d : captureDevices) {
            if (d.name == name.value()) { found = true; break; }
        }
        if (!found) return false;
    }
    if (auto name = playbackConfig.deviceName()) {
        bool found = false;
        for (const auto& d : playbackDevices) {
            if (d.name == name.value()) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

void AudioDeviceManager::fetchDevices() {
    auto engine = m_engine;
    auto toLowerStr = [](std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return str;
    };
    std::string capBackendLower = toLowerStr(audioBackendTypeToString(captureConfig.backend));
    std::string pbBackendLower = toLowerStr(audioBackendTypeToString(playbackConfig.backend));

    QtConcurrent::run([this, engine, capBackendLower, pbBackendLower]() {
        auto cap = engine->getAvailableDevices(capBackendLower, true);
        auto pb = engine->getAvailableDevices(pbBackendLower, false);
        QMetaObject::invokeMethod(this, [this, cap, pb]() {
            captureDevices = cap;
            playbackDevices = pb;
            refreshDeviceCapabilities();
            emit devicesRefreshed();
        });
    });
}

void AudioDeviceManager::refreshDeviceCapabilities() {
    auto engine = m_engine;
    std::string capName = captureConfig.deviceName().value_or("");
    std::string pbName = playbackConfig.deviceName().value_or("");
    auto toLowerStr = [](std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return str;
    };
    std::string capBackendLower = toLowerStr(audioBackendTypeToString(captureConfig.backend));
    std::string pbBackendLower = toLowerStr(audioBackendTypeToString(playbackConfig.backend));

    bool isCapHw = (captureConfig.backend == AudioBackendType::CoreAudio || captureConfig.backend == AudioBackendType::WASAPI || captureConfig.backend == AudioBackendType::ASIO || captureConfig.backend == AudioBackendType::ALSA || captureConfig.backend == AudioBackendType::PulseAudio);
    bool isPbHw = (playbackConfig.backend == AudioBackendType::CoreAudio || playbackConfig.backend == AudioBackendType::WASAPI || playbackConfig.backend == AudioBackendType::ASIO || playbackConfig.backend == AudioBackendType::ALSA || playbackConfig.backend == AudioBackendType::PulseAudio);

    QtConcurrent::run([this, engine, capName, pbName, capBackendLower, pbBackendLower, isCapHw, isPbHw]() {
        std::optional<AudioDeviceDescriptor> capDesc;
        std::optional<AudioDeviceDescriptor> pbDesc;

        if (isCapHw) {
            capDesc = engine->getDeviceCapabilities(capBackendLower, capName, true);
        }
        if (isPbHw) {
            pbDesc = engine->getDeviceCapabilities(pbBackendLower, pbName, false);
        }

        QMetaObject::invokeMethod(this, [this, capDesc, pbDesc, isCapHw, isPbHw]() {
            if (isCapHw && capDesc.has_value()) {
                captureConfig.capabilities = capDesc.value();
            } else if (!isCapHw) {
                captureConfig.capabilities = AudioDeviceDescriptor();
            }
            if (isPbHw && pbDesc.has_value()) {
                playbackConfig.capabilities = pbDesc.value();
            } else if (!isPbHw) {
                playbackConfig.capabilities = AudioDeviceDescriptor();
            }

            captureConfig = captureConfig.enforced();
            playbackConfig = playbackConfig.enforced();
            validateSampleRates();

            emit configChanged();
            if (onConfigChanged) onConfigChanged();
        });
    });
}

void AudioDeviceManager::validateSampleRates() {
    if (m_isValidating) return;
    m_isValidating = true;

    auto pbOptions = playbackRateOptions();
    if (!pbOptions.empty() && std::find(pbOptions.begin(), pbOptions.end(), playbackConfig.sampleRate) == pbOptions.end()) {
        int best = DeviceConfig::bestRate(pbOptions, playbackConfig.sampleRate);
        if (playbackConfig.sampleRate != best) {
            playbackConfig.sampleRate = best;
        }
    }
    auto capOptions = captureRateOptions();
    if (!capOptions.empty() && std::find(capOptions.begin(), capOptions.end(), captureConfig.sampleRate) == capOptions.end()) {
        int best = DeviceConfig::bestRate(capOptions, captureConfig.sampleRate);
        if (captureConfig.sampleRate != best) {
            captureConfig.sampleRate = best;
        }
    }
    if (!m_settings->resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate) {
        captureConfig.sampleRate = playbackConfig.sampleRate;
    }

    m_isValidating = false;
}
