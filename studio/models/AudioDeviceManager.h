#ifndef AUDIO_DEVICE_MANAGER_H
#define AUDIO_DEVICE_MANAGER_H

#include "engine/CDSPEngine.h"
#include "models/AudioSettings.h"
#include "models/DeviceConfig.h"

#include <QFutureWatcher>
#include <QMediaDevices>
#include <QObject>
#include <memory>
#include <vector>

class AudioDeviceManager : public QObject {
    Q_OBJECT

public:
    AudioDeviceManager(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<AudioSettings> settings,
                       QObject* parent = nullptr);
    ~AudioDeviceManager();

    DeviceConfig captureConfig;
    DeviceConfig playbackConfig;
    bool exclusiveMode = false;

    std::vector<AudioDevice> captureDevices;
    std::vector<AudioDevice> playbackDevices;

    void fetchDevices();
    void refreshDeviceCapabilities();
    void validateSampleRates();
    void startDeviceChangeListener();
    void stopDeviceChangeListener();

    std::vector<int> captureRateOptions() const;
    std::vector<int> playbackRateOptions() const;
    double latencyMs() const;

    bool devicesAvailable() const;

    void setCaptureConfig(const DeviceConfig& config);
    void setPlaybackConfig(const DeviceConfig& config);
    void setExclusiveMode(bool exclusive);
    void saveConfigs();

    std::function<void()> onConfigChanged;

signals:
    void configChanged();
    void devicesRefreshed();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioSettings> m_settings;
    bool m_isInitializing = true;
    bool m_isValidating = false;

    QFutureWatcher<void> m_devicesWatcher;
    QFutureWatcher<void> m_capabilitiesWatcher;

    QMediaDevices m_mediaDevices;
    QMetaObject::Connection m_inputsConnection;
    QMetaObject::Connection m_outputsConnection;

    void loadSavedConfigs();
};

#endif // AUDIO_DEVICE_MANAGER_H
