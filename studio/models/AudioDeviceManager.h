#ifndef AUDIO_DEVICE_MANAGER_H
#define AUDIO_DEVICE_MANAGER_H

#include "models/DeviceConfig.h"
#include "models/AudioSettings.h"
#include "engine/CDSPEngine.h"
#include <QObject>
#include <vector>
#include <memory>
#include <functional>

class AudioDeviceManager : public QObject {
    Q_OBJECT

public:
    AudioDeviceManager(std::shared_ptr<CDSPEngine> engine, std::shared_ptr<AudioSettings> settings, QObject* parent = nullptr);
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

    std::function<void()> onConfigChanged;

signals:
    void configChanged();
    void devicesRefreshed();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioSettings> m_settings;
    bool m_isInitializing = true;
    bool m_isValidating = false;

    void loadSavedConfigs();
    void saveConfigs();
};

#endif // AUDIO_DEVICE_MANAGER_H
