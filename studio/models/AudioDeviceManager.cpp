#include "models/AudioDeviceManager.h"

#include "models/LogManager.h"

#include <QJsonDocument>
#include <QMediaDevices>
#include <QSettings>
#include <QtConcurrent>
#include <algorithm>
#include <set>

namespace {
QString formatRates(const std::vector<int>& rates) {
    QStringList list;
    for (int r : rates) {
        list.push_back(QString::number(r));
    }
    return list.join(", ");
}

QString formatConfig(const DeviceConfig& cfg) {
    return QString("backend=%1, device='%2', channels=%3/%4, rate=%5Hz, exclusive=%6")
        .arg(QString::fromStdString(audioBackendTypeToString(cfg.backend)))
        .arg(QString::fromStdString(cfg.deviceName().value_or("<default>")))
        .arg(cfg.channels)
        .arg(cfg.deviceChannels)
        .arg(cfg.sampleRate)
        .arg(cfg.exclusive ? "true" : "false");
}

DeviceConfig loadConfig(QSettings& s, const QString& key, const DeviceConfig& defaultCfg) {
    if (s.contains(key)) {
        QByteArray data = s.value(key).toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject())
            return DeviceConfig::fromJson(doc.object());
    }
    return defaultCfg;
}

std::map<std::string, DeviceConfig> loadConfigMap(QSettings& s, const QString& key, const DeviceConfig& fallback) {
    std::map<std::string, DeviceConfig> result;
    if (s.contains(key)) {
        QByteArray data = s.value(key).toByteArray();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.value().isObject()) {
                    result[it.key().toStdString()] = DeviceConfig::fromJson(it.value().toObject());
                }
            }
        }
    }
    if (result.empty()) {
        result[fallback.deviceName().value_or("")] = fallback;
    }
    return result;
}

void saveConfig(QSettings& s, const QString& key, const DeviceConfig& cfg) {
    QJsonDocument doc(cfg.toJson());
    s.setValue(key, doc.toJson(QJsonDocument::Compact));
}

void saveConfigMap(QSettings& s, const QString& key, const std::map<std::string, DeviceConfig>& map) {
    QJsonObject obj;
    for (const auto& [name, cfg] : map) {
        obj.insert(QString::fromStdString(name), cfg.toJson());
    }
    s.setValue(key, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
} // namespace

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
    AppLogger::info("AudioDeviceManager", QString("Initialized AudioDeviceManager [Capture: %1] [Playback: %2]")
                                              .arg(formatConfig(captureConfig))
                                              .arg(formatConfig(playbackConfig)));
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
    captureConfig = loadConfig(s, "captureConfig", captureConfig);
    playbackConfig = loadConfig(s, "playbackConfig", playbackConfig);
    m_captureDeviceConfigs = loadConfigMap(s, "captureDeviceConfigs", captureConfig);
    m_playbackDeviceConfigs = loadConfigMap(s, "playbackDeviceConfigs", playbackConfig);
    exclusiveMode = s.value("exclusiveMode", false).toBool();
}

void AudioDeviceManager::saveConfigs() {
    QSettings s("DSPMonitor", "MonitorQt");
    saveConfig(s, "captureConfig", captureConfig);
    saveConfig(s, "playbackConfig", playbackConfig);
    saveConfigMap(s, "captureDeviceConfigs", m_captureDeviceConfigs);
    saveConfigMap(s, "playbackDeviceConfigs", m_playbackDeviceConfigs);
    s.setValue("exclusiveMode", exclusiveMode);
}

void AudioDeviceManager::setConfig(bool isCapture, const DeviceConfig& newConfig) {
    if (m_isInitializing)
        return;

    DeviceConfig enforced = newConfig.enforced();
    DeviceConfig& current = config(isCapture);
    bool backendChanged = (enforced.backend != current.backend);
    bool devChanged = (enforced.deviceName() != current.deviceName() || backendChanged);

    AppLogger::info("AudioDeviceManager", QString("Setting %1 config: %2")
                                              .arg(isCapture ? "capture" : "playback")
                                              .arg(formatConfig(enforced)));

    if (backendChanged) {
        enforced.setDeviceName("");
    } else if (!devChanged) {
        std::string name = enforced.deviceName().value_or("");
        deviceConfigCache(isCapture)[name] = enforced;
    }

    current = enforced;
    saveConfigs();

    if (backendChanged) {
        AppLogger::info("AudioDeviceManager", QString("%1 backend changed, re-fetching devices...")
                                                  .arg(isCapture ? "Capture" : "Playback"));
        fetchDevices();
    } else if (devChanged) {
        AppLogger::info("AudioDeviceManager", QString("%1 device changed to '%2', refreshing capabilities...")
                                                  .arg(isCapture ? "Capture" : "Playback")
                                                  .arg(QString::fromStdString(current.deviceName().value_or("<default>"))));
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

void AudioDeviceManager::setCaptureConfig(const DeviceConfig& config) {
    setConfig(true, config);
}

void AudioDeviceManager::setPlaybackConfig(const DeviceConfig& config) {
    setConfig(false, config);
}

void AudioDeviceManager::setExclusiveMode(bool exclusive) {
    if (m_isInitializing)
        return;
    exclusiveMode = exclusive;
    saveConfigs();
    AppLogger::info("AudioDeviceManager", QString("Exclusive mode set to %1").arg(exclusive ? "enabled" : "disabled"));
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

std::vector<int> AudioDeviceManager::commonRateOptions() const {
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
    return common;
}

std::vector<int> AudioDeviceManager::rateOptions(bool isCapture) const {
    if (m_settings && !m_settings->resamplerEnabled) {
        auto common = commonRateOptions();
        if (!common.empty())
            return common;
    }
    return isCapture ? captureConfig.supportedRates() : playbackConfig.supportedRates();
}

std::vector<int> AudioDeviceManager::captureRateOptions() const {
    return rateOptions(true);
}

std::vector<int> AudioDeviceManager::playbackRateOptions() const {
    return rateOptions(false);
}

double AudioDeviceManager::latencyMs() const {
    int chunkSize = m_settings ? m_settings->chunkSize : 1024;
    int rate = std::max(1, captureConfig.sampleRate);
    return (static_cast<double>(chunkSize) / static_cast<double>(rate)) * 1000.0;
}

bool AudioDeviceManager::isDeviceAvailable(const DeviceConfig& cfg, bool isCapture) const {
    if (!backendHasDeviceList(cfg.backend))
        return true;

    auto name = cfg.deviceName();
    if (!name || name->empty())
        return true;

    bool isWasapiLoopback = false;
#if defined(ENABLE_WASAPI)
    isWasapiLoopback = (isCapture && cfg.backend == AudioBackendType::WASAPI && cfg.loopback);
#endif
    const auto& list = deviceList(isCapture, isWasapiLoopback);
    return std::any_of(list.begin(), list.end(), [&](const AudioDevice& d) {
        return d.id == *name || d.name == *name;
    });
}

bool AudioDeviceManager::devicesAvailable() const {
    return isDeviceAvailable(captureConfig, true) && isDeviceAvailable(playbackConfig, false);
}

void AudioDeviceManager::validateDevicePresence(DeviceConfig& cfg, bool isCapture,
                                                const std::vector<AudioDevice>& capList,
                                                const std::vector<AudioDevice>& pbList) {
    if (!backendHasDeviceList(cfg.backend))
        return;

    auto name = cfg.deviceName();
    if (!name || name->empty())
        return;

    bool isWasapiLoopback = false;
#if defined(ENABLE_WASAPI)
    isWasapiLoopback = (isCapture && cfg.backend == AudioBackendType::WASAPI && cfg.loopback);
#endif
    const auto& list = (isWasapiLoopback || !isCapture) ? pbList : capList;
    bool found = std::any_of(list.begin(), list.end(), [&](const AudioDevice& d) {
        return d.id == *name || d.name == *name;
    });

    if (!found) {
        AppLogger::warn("AudioDeviceManager",
                        QString("Configured %1 device '%2' is disconnected/missing. Falling back to default.")
                            .arg(isCapture ? "capture" : "playback")
                            .arg(QString::fromStdString(*name)));
        cfg.setDeviceName("");
    }
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
    auto backendString = [&toLowerStr](const DeviceConfig& cfg) {
        auto b = backendHasDeviceList(cfg.backend) ? cfg.backend : defaultHardwareBackend();
        return toLowerStr(audioBackendTypeToString(b));
    };

    std::string capBackendLower = backendString(captureConfig);
    std::string pbBackendLower = backendString(playbackConfig);

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

            AppLogger::info("AudioDeviceManager", QString("Devices refreshed: %1 capture, %2 playback device(s) found")
                                                      .arg(cap.size())
                                                      .arg(pb.size()));

            validateDevicePresence(captureConfig, true, cap, pb);
            validateDevicePresence(playbackConfig, false, cap, pb);

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
    auto& cache = deviceConfigCache(isCapture);

    if (desc.has_value() && !desc->capability_sets.empty()) {
        cfg.capabilities = desc.value();
    } else {
        auto it = cache.find(name);
        if (it != cache.end() && !it->second.capabilities.capability_sets.empty()) {
            cfg.capabilities = it->second.capabilities;
        }
    }
    if (!origId.empty() && cfg.capabilities.name.empty()) {
        cfg.capabilities.name = origId;
    }
    if (!name.empty() && !cfg.capabilities.capability_sets.empty()) {
        cache[name] = cfg;
    }
}

void AudioDeviceManager::handleFormatChange(bool isCapture, int newRate) {
    m_capabilityRequestVersion++;
    m_fetchDevicesVersion++;
    m_devicesWatcher.cancel();
    m_capabilitiesWatcher.cancel();
    if (m_deviceChangeDebounceTimer) {
        m_deviceChangeDebounceTimer->stop();
    }

    AppLogger::info("AudioDeviceManager", QString("Handling %1 format change to %2 Hz")
                                              .arg(isCapture ? "capture" : "playback")
                                              .arg(newRate));

    config(isCapture).updateRate(newRate);

    if (m_settings && !m_settings->resamplerEnabled) {
        config(!isCapture).sampleRate = config(isCapture).sampleRate;
        config(!isCapture) = config(!isCapture).enforced();
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

            AppLogger::debug("AudioDeviceManager", QString("Capabilities refreshed (Capture rates: [%1], Playback rates: [%2])")
                                                       .arg(formatRates(captureConfig.supportedRates()))
                                                       .arg(formatRates(playbackConfig.supportedRates())));

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

    auto clampToOptions = [&changed](DeviceConfig& cfg, const std::vector<int>& options) {
        if (!options.empty() && std::find(options.begin(), options.end(), cfg.sampleRate) == options.end()) {
            int best = DeviceConfig::bestRate(options, cfg.sampleRate);
            if (cfg.sampleRate != best) {
                cfg.sampleRate = best;
                changed = true;
            }
        }
    };

    clampToOptions(playbackConfig, playbackRateOptions());
    clampToOptions(captureConfig, captureRateOptions());

    if (m_settings && !m_settings->resamplerEnabled && captureConfig.sampleRate != playbackConfig.sampleRate) {
        auto common = commonRateOptions();
        if (!common.empty()) {
            int target = isHardwareBackend(captureConfig.backend) ? captureConfig.sampleRate : playbackConfig.sampleRate;
            int best = DeviceConfig::bestRate(common, target);
            captureConfig.sampleRate = best;
            playbackConfig.sampleRate = best;
        } else if (isHardwareBackend(captureConfig.backend)) {
            playbackConfig.sampleRate = captureConfig.sampleRate;
        } else {
            captureConfig.sampleRate = playbackConfig.sampleRate;
        }
        changed = true;
    }
    if (m_settings && m_settings->resamplerEnabled && m_settings->resamplerType == ResamplerType::Slip &&
        captureConfig.sampleRate != playbackConfig.sampleRate) {
        m_settings->resamplerType = ResamplerType::Synchronous;
        m_settings->savePreferences();
        changed = true;
    }

    if (changed) {
        AppLogger::info("AudioDeviceManager", QString("Sample rates updated by validation: Capture=%1 Hz, Playback=%2 Hz")
                                                  .arg(captureConfig.sampleRate)
                                                  .arg(playbackConfig.sampleRate));
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
