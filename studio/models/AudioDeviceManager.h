#ifndef AUDIO_DEVICE_MANAGER_H
#define AUDIO_DEVICE_MANAGER_H

#include "config/DSPConfigTypes.h" // for AudioDevice, AudioDeviceDescriptor, AudioBackendType
#include "engine/CDSPEngine.h"     // for CDSPEngine
#include "models/AudioSettings.h"  // for AudioSettings
#include "models/DeviceConfig.h"   // for DeviceConfig

#include <QFutureWatcher> // for QFutureWatcher
#include <QMediaDevices>  // for QMediaDevices
#include <QMetaObject>    // for QMetaObject
#include <QObject>        // for QObject, Q_OBJECT, signals
#include <QTimer>         // for QTimer
#include <QtGlobal>       // for qint64
#include <functional>     // for function
#include <map>            // for map
#include <memory>         // for shared_ptr
#include <optional>       // for optional
#include <stdint.h>       // for uint64_t
#include <string>         // for basic_string, string
#include <vector>         // for vector

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
    void refreshDevices();
    void refreshDeviceCapabilities();
    void handleFormatChange(bool isCapture, int newRate);
    bool validateSampleRates();
    void startDeviceChangeListener();
    void stopDeviceChangeListener();

    std::vector<int> commonRateOptions() const;
    std::vector<int> rateOptions(bool isCapture) const;
    std::vector<int> captureRateOptions() const;
    std::vector<int> playbackRateOptions() const;
    double latencyMs() const;

    bool devicesAvailable() const;

    void setCaptureConfig(const DeviceConfig& config);
    void setPlaybackConfig(const DeviceConfig& config);
    void setExclusiveMode(bool exclusive);
    void saveConfigs();

    static AudioBackendType defaultHardwareBackend();

    std::function<void()> onConfigChanged;

signals:
    void configChanged();
    void devicesRefreshed();

private:
    std::shared_ptr<CDSPEngine> m_engine;
    std::shared_ptr<AudioSettings> m_settings;
    bool m_isInitializing = true;
    bool m_isValidating = false;

    std::map<std::string, DeviceConfig> m_captureDeviceConfigs;
    std::map<std::string, DeviceConfig> m_playbackDeviceConfigs;

    QFutureWatcher<void> m_devicesWatcher;
    QFutureWatcher<void> m_capabilitiesWatcher;

    QMediaDevices m_mediaDevices;
    QMetaObject::Connection m_inputsConnection;
    QMetaObject::Connection m_outputsConnection;
    QTimer* m_deviceChangeDebounceTimer = nullptr;
    bool m_isFetchingDevices = false;
    qint64 m_lastFetchFinishedTime = 0;

    uint64_t m_fetchDevicesVersion = 0;
    uint64_t m_capabilityRequestVersion = 0;

    DeviceConfig& config(bool isCapture) { return isCapture ? captureConfig : playbackConfig; }
    const DeviceConfig& config(bool isCapture) const { return isCapture ? captureConfig : playbackConfig; }
    const std::vector<AudioDevice>& deviceList(bool isCapture, bool isWasapiLoopback = false) const {
        return (isWasapiLoopback || !isCapture) ? playbackDevices : captureDevices;
    }
    std::map<std::string, DeviceConfig>& deviceConfigCache(bool isCapture) {
        return isCapture ? m_captureDeviceConfigs : m_playbackDeviceConfigs;
    }

    void setConfig(bool isCapture, const DeviceConfig& config);
    bool isDeviceAvailable(const DeviceConfig& cfg, bool isCapture) const;
    void validateDevicePresence(DeviceConfig& cfg, bool isCapture, const std::vector<AudioDevice>& capList,
                                const std::vector<AudioDevice>& pbList);

    std::optional<AudioDeviceDescriptor> queryDeviceCapabilities(const DeviceConfig& cfg, bool isCapture) const;
    void updateCapabilitiesFromDescriptor(DeviceConfig& cfg, bool isCapture,
                                          const std::optional<AudioDeviceDescriptor>& desc);
    void loadSavedConfigs();
};

#endif // AUDIO_DEVICE_MANAGER_H
