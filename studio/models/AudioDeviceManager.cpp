#include "models/AudioDeviceManager.h"

#include "models/LogManager.h"

#include <QJsonDocument>
#include <QMediaDevices>
#include <QSettings>
#include <QtConcurrent>
#include <algorithm>
#include <set>

AudioDeviceManager::AudioDeviceManager(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<AudioSettings> settings,
                                       QObject* parent)
    : QObject(parent), m_engine(engine), m_settings(settings) {
    loadSavedConfigs();
    m_isInitializing = false;
    if (m_settings) {
        connect(m_settings.get(), &AudioSettings::settingsChanged, this, [this]() {
            if (!validateSampleRates()) {
                emit configChanged();
            }
        });
    }
    startDeviceChangeListener();
    fetchDevices();
}

AudioDeviceManager::~AudioDeviceManager() {
    stopDeviceChangeListener();
    m_fetchDevicesVersion++;
    m_capabilityRequestVersion++;
    m_devicesWatcher.cancel();
    m_devicesWatcher.waitForFinished();
    m_capabilitiesWatcher.cancel();
    m_capabilitiesWatcher.waitForFinished();
}

AudioBackendType AudioDeviceManager::defaultHardwareBackend() {
#if defined(ENABLE_COREAUDIO)
    return AudioBackendType::CoreAudio;
#elif defined(ENABLE_WASAPI)
    return AudioBackendType::WASAPI;
#elif defined(ENABLE_ALSA)
    return AudioBackendType::ALSA;
#elif defined(ENABLE_PIPEWIRE)
    return AudioBackendType::PipeWire;
#else
    return AudioBackendType::RawFile;
#endif
}

void AudioDeviceManager::refreshDevices() {
    fetchDevices();
}

void AudioDeviceManager::startDeviceChangeListener() {
    stopDeviceChangeListener();

    if (!m_deviceChangeDebounceTimer) {
        m_deviceChangeDebounceTimer = new QTimer(this);
        m_deviceChangeDebounceTimer->setSingleShot(true);
        connect(m_deviceChangeDebounceTimer, &QTimer::timeout, this, [this]() {
            if (!m_isFetchingDevices && (QDateTime::currentMSecsSinceEpoch() - m_lastFetchFinishedTime >= 1500)) {
                AppLogger::info("AudioDeviceManager", "Audio device change detected, refreshing devices...");
                refreshDevices();
            }
        });
    }

    auto handleDeviceChange = [this]() {
        if (m_isFetchingDevices || (QDateTime::currentMSecsSinceEpoch() - m_lastFetchFinishedTime < 1500)) {
            return;
        }
        m_deviceChangeDebounceTimer->start(1200);
    };

    m_inputsConnection = connect(&m_mediaDevices, &QMediaDevices::audioInputsChanged, this, handleDeviceChange);
    m_outputsConnection = connect(&m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, handleDeviceChange);
}

void AudioDeviceManager::stopDeviceChangeListener() {
    if (m_deviceChangeDebounceTimer) {
        m_deviceChangeDebounceTimer->stop();
    }
    if (m_inputsConnection) {
        disconnect(m_inputsConnection);
        m_inputsConnection = {};
    }
    if (m_outputsConnection) {
        disconnect(m_outputsConnection);
        m_outputsConnection = {};
    }
}

void AudioDeviceManager::loadSavedConfigs() {
    QSettings s("DSPMonitor", "MonitorQt");
    if (s.contains("captureConfig")) {
        QByteArray data = s.value("captureConfig").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject())
            captureConfig = DeviceConfig::fromJson(doc.object());
    }
    if (s.contains("playbackConfig")) {
        QByteArray data = s.value("playbackConfig").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject())
            playbackConfig = DeviceConfig::fromJson(doc.object());
    }

    if (s.contains("captureDeviceConfigs")) {
        QByteArray data = s.value("captureDeviceConfigs").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.value().isObject()) {
                    m_captureDeviceConfigs[it.key().toStdString()] = DeviceConfig::fromJson(it.value().toObject());
                }
            }
        }
    } else {
        m_captureDeviceConfigs[captureConfig.deviceName().value_or("")] = captureConfig;
    }

    if (s.contains("playbackDeviceConfigs")) {
        QByteArray data = s.value("playbackDeviceConfigs").toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.value().isObject()) {
                    m_playbackDeviceConfigs[it.key().toStdString()] = DeviceConfig::fromJson(it.value().toObject());
                }
            }
        }
    } else {
        m_playbackDeviceConfigs[playbackConfig.deviceName().value_or("")] = playbackConfig;
    }

    exclusiveMode = s.value("exclusiveMode", false).toBool();
}

void AudioDeviceManager::saveConfigs() {
    QSettings s("DSPMonitor", "MonitorQt");
    QJsonDocument docCap(captureConfig.toJson());
    s.setValue("captureConfig", docCap.toJson(QJsonDocument::Compact));

    QJsonDocument docPb(playbackConfig.toJson());
    s.setValue("playbackConfig", docPb.toJson(QJsonDocument::Compact));

    QJsonObject capDictObj;
    for (const auto& [name, cfg] : m_captureDeviceConfigs) {
        capDictObj.insert(QString::fromStdString(name), cfg.toJson());
    }
    s.setValue("captureDeviceConfigs", QJsonDocument(capDictObj).toJson(QJsonDocument::Compact));

    QJsonObject pbDictObj;
    for (const auto& [name, cfg] : m_playbackDeviceConfigs) {
        pbDictObj.insert(QString::fromStdString(name), cfg.toJson());
    }
    s.setValue("playbackDeviceConfigs", QJsonDocument(pbDictObj).toJson(QJsonDocument::Compact));

    s.setValue("exclusiveMode", exclusiveMode);
}

void AudioDeviceManager::setCaptureConfig(const DeviceConfig& config) {
    if (m_isInitializing)
        return;
    DeviceConfig enforced = config.enforced();
    bool backendChanged = (enforced.backend != captureConfig.backend);
    bool devChanged = (enforced.deviceName() != captureConfig.deviceName() || backendChanged);

    if (backendChanged) {
        enforced.setDeviceName("");
    } else if (!devChanged) {
        std::string name = enforced.deviceName().value_or("");
        m_captureDeviceConfigs[name] = enforced;
    }

    captureConfig = enforced;
    saveConfigs();

    if (backendChanged) {
        fetchDevices();
    } else if (devChanged) {
        refreshDeviceCapabilities();
    } else {
        bool rateChanged = validateSampleRates();
        if (!rateChanged) {
            emit configChanged();
            if (onConfigChanged)
                onConfigChanged();
        }
    }
}

void AudioDeviceManager::setPlaybackConfig(const DeviceConfig& config) {
    if (m_isInitializing)
        return;
    DeviceConfig enforced = config.enforced();
    bool backendChanged = (enforced.backend != playbackConfig.backend);
    bool devChanged = (enforced.deviceName() != playbackConfig.deviceName() || backendChanged);

    if (backendChanged) {
        enforced.setDeviceName("");
    } else if (!devChanged) {
        std::string name = enforced.deviceName().value_or("");
        m_playbackDeviceConfigs[name] = enforced;
    }

    playbackConfig = enforced;
    saveConfigs();

    if (backendChanged) {
        fetchDevices();
    } else if (devChanged) {
        refreshDeviceCapabilities();
    } else {
        bool rateChanged = validateSampleRates();
        if (!rateChanged) {
            emit configChanged();
            if (onConfigChanged)
                onConfigChanged();
        }
    }
}

void AudioDeviceManager::setExclusiveMode(bool exclusive) {
    if (m_isInitializing)
        return;
    exclusiveMode = exclusive;
    saveConfigs();
    if (!exclusiveMode && playbackConfig.outputDoP) {
        DeviceConfig newPb = playbackConfig;
        newPb.outputDoP = false;
        setPlaybackConfig(newPb);
    } else {
        emit configChanged();
        if (onConfigChanged)
            onConfigChanged();
    }
}

std::vector<int> AudioDeviceManager::captureRateOptions() const {
    if (!m_settings || m_settings->resamplerEnabled)
        return captureConfig.supportedRates();
    auto cap = captureConfig.supportedRates();
    auto pb = playbackConfig.supportedRates();
    if (cap.empty())
        return pb;
    if (pb.empty())
        return cap;

    std::vector<int> common;
    std::set<int> pbSet(pb.begin(), pb.end());
    for (int r : cap) {
        if (pbSet.count(r))
            common.push_back(r);
    }
    std::sort(common.begin(), common.end());
    return common.empty() ? pb : common;
}

std::vector<int> AudioDeviceManager::playbackRateOptions() const {
    if (!m_settings)
        return playbackConfig.supportedRates();
    return m_settings->resamplerEnabled ? playbackConfig.supportedRates() : captureRateOptions();
}

double AudioDeviceManager::latencyMs() const {
    int chunkSize = m_settings ? m_settings->chunkSize : 1024;
    int rate = std::max(1, captureConfig.sampleRate);
    return (static_cast<double>(chunkSize) / static_cast<double>(rate)) * 1000.0;
}

bool AudioDeviceManager::devicesAvailable() const {
    if (backendHasDeviceList(captureConfig.backend)) {
        if (auto name = captureConfig.deviceName()) {
            if (!name.value().empty()) {
                bool found = false;
                bool isWasapiLoopback = false;
#if defined(ENABLE_WASAPI)
                isWasapiLoopback = (captureConfig.backend == AudioBackendType::WASAPI && captureConfig.loopback);
#endif
                const auto& devList = isWasapiLoopback ? playbackDevices : captureDevices;
                for (const auto& d : devList) {
                    if (d.id == name.value() || d.name == name.value()) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
        }
    }
    if (backendHasDeviceList(playbackConfig.backend)) {
        if (auto name = playbackConfig.deviceName()) {
            if (!name.value().empty()) {
                bool found = false;
                for (const auto& d : playbackDevices) {
                    if (d.id == name.value() || d.name == name.value()) {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return false;
            }
        }
    }
    return true;
}

void AudioDeviceManager::fetchDevices() {
    auto engine = m_engine;
    if (!engine)
        return;

    uint64_t version = ++m_fetchDevicesVersion;

    auto toLowerStr = [](std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return str;
    };
    std::string capBackendLower = backendHasDeviceList(captureConfig.backend)
                                      ? toLowerStr(audioBackendTypeToString(captureConfig.backend))
                                      : toLowerStr(audioBackendTypeToString(defaultHardwareBackend()));
    std::string pbBackendLower = backendHasDeviceList(playbackConfig.backend)
                                     ? toLowerStr(audioBackendTypeToString(playbackConfig.backend))
                                     : toLowerStr(audioBackendTypeToString(defaultHardwareBackend()));

    m_isFetchingDevices = true;
    m_devicesWatcher.setFuture(QtConcurrent::run([this, engine, version, capBackendLower, pbBackendLower]() {
        auto cap = engine->getAvailableDevices(capBackendLower, true);
        auto pb = engine->getAvailableDevices(pbBackendLower, false);
        QMetaObject::invokeMethod(this, [this, version, cap, pb]() {
            m_isFetchingDevices = false;
            m_lastFetchFinishedTime = QDateTime::currentMSecsSinceEpoch();
            if (version != m_fetchDevicesVersion)
                return;
            captureDevices = cap;
            playbackDevices = pb;

            // Safe fallback logic: if configured hardware device is disconnected/unplugged, fallback to default
            // hardware device ("")
            if (backendHasDeviceList(captureConfig.backend)) {
                if (auto name = captureConfig.deviceName()) {
                    if (!name.value().empty()) {
                        bool found = false;
                        bool isWasapiLoopback = false;
#if defined(ENABLE_WASAPI)
                        isWasapiLoopback =
                            (captureConfig.backend == AudioBackendType::WASAPI && captureConfig.loopback);
#endif
                        const auto& devList = isWasapiLoopback ? pb : cap;
                        for (const auto& d : devList) {
                            if (d.id == name.value() || d.name == name.value()) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            AppLogger::warn("AudioDeviceManager",
                                            QString("Configured capture device %1 is disconnected/missing. Falling "
                                                    "back to default input device.")
                                                .arg(QString::fromStdString(name.value())));
                            captureConfig.setDeviceName("");
                        }
                    }
                }
            }

            if (backendHasDeviceList(playbackConfig.backend)) {
                if (auto name = playbackConfig.deviceName()) {
                    if (!name.value().empty()) {
                        bool found = false;
                        for (const auto& d : pb) {
                            if (d.id == name.value() || d.name == name.value()) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            AppLogger::warn("AudioDeviceManager",
                                            QString("Configured playback device %1 is disconnected/missing. Falling "
                                                    "back to default output device.")
                                                .arg(QString::fromStdString(name.value())));
                            playbackConfig.setDeviceName("");
                        }
                    }
                }
            }

            refreshDeviceCapabilities();
            emit devicesRefreshed();
        });
    }));
}

std::optional<AudioDeviceDescriptor> AudioDeviceManager::queryDeviceCapabilities(const DeviceConfig& cfg,
                                                                                 bool isCapture) const {
    if (!m_engine || !backendHasDeviceCapabilities(cfg.backend))
        return std::nullopt;
    auto toLowerStr = [](std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        return str;
    };
    std::string backendLower = toLowerStr(audioBackendTypeToString(cfg.backend));
    std::string devName = cfg.deviceName().value_or("");
    bool isQueryCapture = isCapture;
#if defined(ENABLE_WASAPI)
    if (cfg.backend == AudioBackendType::WASAPI && cfg.loopback) {
        isQueryCapture = false;
    }
#endif
    return m_engine->getDeviceCapabilities(backendLower, devName, isQueryCapture);
}

void AudioDeviceManager::updateCapabilitiesFromDescriptor(DeviceConfig& cfg, bool isCapture,
                                                          const std::optional<AudioDeviceDescriptor>& desc) {
    if (!backendHasDeviceCapabilities(cfg.backend)) {
        cfg.capabilities = AudioDeviceDescriptor();
        return;
    }
    std::string name = cfg.deviceName().value_or("");
    std::string origId = cfg.capabilities.name;
    if (desc.has_value() && !desc->capability_sets.empty()) {
        cfg.capabilities = desc.value();
    } else {
        auto& dict = isCapture ? m_captureDeviceConfigs : m_playbackDeviceConfigs;
        auto it = dict.find(name);
        if (it != dict.end() && !it->second.capabilities.capability_sets.empty()) {
            cfg.capabilities = it->second.capabilities;
        }
    }
    if (!origId.empty() && cfg.capabilities.name.empty()) {
        cfg.capabilities.name = origId;
    }
    if (!name.empty() && !cfg.capabilities.capability_sets.empty()) {
        if (isCapture)
            m_captureDeviceConfigs[name] = cfg;
        else
            m_playbackDeviceConfigs[name] = cfg;
    }
}

void AudioDeviceManager::handleFormatChange(bool isCapture, int newRate) {
    DeviceConfig& primary = isCapture ? captureConfig : playbackConfig;
    DeviceConfig& secondary = isCapture ? playbackConfig : captureConfig;

    updateCapabilitiesFromDescriptor(primary, isCapture, queryDeviceCapabilities(primary, isCapture));
    primary.sampleRate = newRate;
    primary = primary.enforced();

    if (m_settings && !m_settings->resamplerEnabled) {
        updateCapabilitiesFromDescriptor(secondary, !isCapture, queryDeviceCapabilities(secondary, !isCapture));
        secondary.sampleRate = primary.sampleRate;
        secondary = secondary.enforced();
    }

    validateSampleRates();
    saveConfigs();
    emit configChanged();
    if (onConfigChanged)
        onConfigChanged();
}

void AudioDeviceManager::refreshDeviceCapabilities() {
    if (!m_engine)
        return;

    uint64_t version = ++m_capabilityRequestVersion;
    DeviceConfig capCfg = captureConfig;
    DeviceConfig pbCfg = playbackConfig;

    m_capabilitiesWatcher.setFuture(QtConcurrent::run([this, version, capCfg, pbCfg]() {
        auto capDesc = queryDeviceCapabilities(capCfg, true);
        auto pbDesc = queryDeviceCapabilities(pbCfg, false);

        QMetaObject::invokeMethod(this, [this, version, capDesc, pbDesc]() {
            if (version != m_capabilityRequestVersion)
                return;

            updateCapabilitiesFromDescriptor(captureConfig, true, capDesc);
            updateCapabilitiesFromDescriptor(playbackConfig, false, pbDesc);

            captureConfig = captureConfig.enforced();
            playbackConfig = playbackConfig.enforced();

            bool rateChanged = validateSampleRates();
            if (!rateChanged) {
                saveConfigs();
                emit configChanged();
                if (onConfigChanged)
                    onConfigChanged();
            }
        });
    }));
}

bool AudioDeviceManager::validateSampleRates() {
    if (m_isValidating)
        return false;
    m_isValidating = true;

    bool changed = false;

    auto pbOptions = playbackRateOptions();
    if (!pbOptions.empty() &&
        std::find(pbOptions.begin(), pbOptions.end(), playbackConfig.sampleRate) == pbOptions.end()) {
        int best = DeviceConfig::bestRate(pbOptions, playbackConfig.sampleRate);
        if (playbackConfig.sampleRate != best) {
            playbackConfig.sampleRate = best;
            changed = true;
        }
    }
    auto capOptions = captureRateOptions();
    if (!capOptions.empty() &&
        std::find(capOptions.begin(), capOptions.end(), captureConfig.sampleRate) == capOptions.end()) {
        int best = DeviceConfig::bestRate(capOptions, captureConfig.sampleRate);
        if (captureConfig.sampleRate != best) {
            captureConfig.sampleRate = best;
            changed = true;
        }
    }
    if (m_settings && !m_settings->resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate) {
        captureConfig.sampleRate = playbackConfig.sampleRate;
        changed = true;
    }
    if (m_settings && m_settings->resamplerEnabled && m_settings->resamplerType == ResamplerType::Slip &&
        captureConfig.sampleRate != playbackConfig.sampleRate) {
        m_settings->resamplerType = ResamplerType::Synchronous;
        m_settings->savePreferences();
        changed = true;
    }

    if (changed) {
        captureConfig = captureConfig.enforced();
        playbackConfig = playbackConfig.enforced();
        saveConfigs();
        emit configChanged();
        if (onConfigChanged)
            onConfigChanged();
    }

    m_isValidating = false;
    return changed;
}
